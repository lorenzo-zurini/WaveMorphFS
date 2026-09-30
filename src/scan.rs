//! The library: turns a source directory into the list of entries the
//! filesystem exposes, and runs background workers for the slow parts
//! (indexing images, converting non-FLAC images, scanning SACD ISOs).
//!
//! Rules per source directory:
//!  * `X.cue` + existing single-file image  -> virtual per-track FLAC files; cue and image hidden
//!    (the image stays hidden while it is still downloading or being processed)
//!  * `*.iso` with an SACD master TOC        -> virtual per-track DSF files; ISO hidden
//!  * other files                            -> passthrough (FLAC files are retagged if a sidecar applies)
//!  * `*.!qB`, `*.parts`, dotfiles, sidecars -> hidden
//!  * cover image from the sidecar tree      -> exposed as cover.jpg/png if the folder has none

use crate::cache::{Cache, SrcKey};
use crate::cue::CueSheet;
use crate::flac::{FlacMeta, MetaBlock};
use crate::retag::RetagFlac;
use crate::sacd::{self, SacdDisc};
use crate::sidecar::Sidecar;
use crate::tags::{self, Tags};
use crate::track::{FlacImage, FlacTrack};
use crate::vfile::{Passthrough, VFile};
use anyhow::{Context, Result, bail};
use log::{info, warn};
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::mpsc::{Receiver, Sender, channel};
use std::time::{Duration, Instant, SystemTime};

pub const AUDIO_IMAGE_EXT: &[&str] = &["flac", "ape", "wv", "tta", "tak", "wav", "m4a", "aiff", "aif"];
/// A file modified more recently than this is assumed to still be written.
const SETTLE: Duration = Duration::from_secs(60);
const RECHECK: Duration = Duration::from_millis(1500);

#[derive(Debug, Clone)]
pub struct Root {
    pub name: String,
    pub path: PathBuf,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub roots: Vec<Root>,
    pub tags_dir: PathBuf,
    pub cache_dir: PathBuf,
    pub workers: usize,
    /// also expose SACD multichannel areas ("MC NN - Title.dsf", album "... (Multichannel)")
    pub sacd_multichannel: bool,
}

#[derive(Clone)]
pub enum EntryKind {
    Dir(PathBuf),
    File(Arc<dyn VFile>),
}

#[derive(Clone)]
pub struct Entry {
    pub name: OsString,
    pub kind: EntryKind,
    pub mtime: SystemTime,
}

pub struct Listing {
    pub entries: Vec<Entry>,
    pub mtime: SystemTime,
    sig: Sig,
    checked: Mutex<Instant>,
    /// rebuild after this instant even if nothing changed (files still settling)
    retry_at: Option<Instant>,
}

impl Listing {
    pub fn find(&self, name: &OsStr) -> Option<&Entry> {
        self.entries.iter().find(|e| e.name == name)
    }
}

#[derive(Debug, Clone, PartialEq)]
struct Sig {
    dir_mtime: Option<SystemTime>,
    sidecar_mtime: Option<SystemTime>,
    overlay_mtime: Option<SystemTime>,
    generation: u64,
}

enum Work<T> {
    Pending,
    /// modified too recently; retry after the instant
    Settling(Instant),
    Ready(Arc<T>),
    Failed(String),
}

// Manual impl: cloning only clones the Arc, so T itself need not be Clone.
impl<T> Clone for Work<T> {
    fn clone(&self) -> Self {
        match self {
            Work::Pending => Work::Pending,
            Work::Settling(i) => Work::Settling(*i),
            Work::Ready(a) => Work::Ready(Arc::clone(a)),
            Work::Failed(e) => Work::Failed(e.clone()),
        }
    }
}

/// A processed image: the FLAC data to split plus the source's own tags.
pub struct ReadyImage {
    pub flac: Arc<FlacImage>,
    pub source_tags: Tags,
    pub pictures: Vec<MetaBlock>,
}

enum Job {
    Image { src: PathBuf, dir: PathBuf },
    Sacd { src: PathBuf, dir: PathBuf, mc: bool },
    Md5 { image: Arc<FlacImage>, ranges: Vec<(u64, u64)>, dir: PathBuf },
}

pub struct Library {
    pub cfg: Config,
    pub cache: Cache,
    images: Mutex<HashMap<PathBuf, (SrcKey, Work<ReadyImage>)>>,
    sacds: Mutex<HashMap<(PathBuf, bool), (SrcKey, Work<SacdDisc>)>>,
    listings: Mutex<HashMap<PathBuf, Arc<Listing>>>,
    generations: Mutex<HashMap<PathBuf, (u64, SystemTime)>>,
    dir_locks: Mutex<HashMap<PathBuf, Arc<Mutex<()>>>>,
    jobs: Mutex<Option<Sender<Job>>>,
    /// images whose per-track MD5 job is queued or running
    md5_queued: Mutex<HashSet<PathBuf>>,
}

impl Library {
    pub fn new(cfg: Config) -> Result<Arc<Library>> {
        let cache = Cache::new(cfg.cache_dir.clone())?;
        std::fs::create_dir_all(&cfg.tags_dir)?;
        Ok(Arc::new(Library {
            cfg,
            cache,
            images: Mutex::new(HashMap::new()),
            sacds: Mutex::new(HashMap::new()),
            listings: Mutex::new(HashMap::new()),
            generations: Mutex::new(HashMap::new()),
            dir_locks: Mutex::new(HashMap::new()),
            jobs: Mutex::new(None),
            md5_queued: Mutex::new(HashSet::new()),
        }))
    }

    /// Start background workers. Without workers, processing happens inline
    /// (used by the CLI tools).
    pub fn start_workers(self: &Arc<Self>) {
        let (tx, rx) = channel::<Job>();
        *self.jobs.lock() = Some(tx);
        let rx = Arc::new(Mutex::new(rx));
        for i in 0..self.cfg.workers.max(1) {
            let lib = Arc::clone(self);
            let rx: Arc<Mutex<Receiver<Job>>> = Arc::clone(&rx);
            std::thread::Builder::new()
                .name(format!("wm-worker-{i}"))
                .spawn(move || {
                    loop {
                        let job = { rx.lock().recv() };
                        let Ok(job) = job else { break };
                        lib.run_job(job);
                        lib.write_status();
                    }
                })
                .expect("spawn worker");
        }
    }

    /// Walk all roots periodically so work is queued before anyone asks.
    pub fn start_prescan(self: &Arc<Self>, every: Duration) {
        let lib = Arc::clone(self);
        std::thread::Builder::new()
            .name("wm-prescan".into())
            .spawn(move || {
                loop {
                    let t = Instant::now();
                    let mut n = 0usize;
                    for r in lib.cfg.roots.clone() {
                        lib.walk(&r.path, &mut n);
                    }
                    info!("prescan: {n} directories in {:?}", t.elapsed());
                    lib.write_status();
                    std::thread::sleep(every);
                }
            })
            .expect("spawn prescan");
    }

    fn walk(&self, dir: &Path, n: &mut usize) {
        *n += 1;
        let Ok(listing) = self.list_dir(dir) else { return };
        for e in &listing.entries {
            if let EntryKind::Dir(p) = &e.kind {
                self.walk(p, n);
            }
        }
    }

    fn run_job(&self, job: Job) {
        match job {
            Job::Image { src, dir } => {
                let key = SrcKey::of(&src).ok();
                let res = self.process_image(&src);
                let state = match res {
                    Ok(r) => {
                        info!("image ready: {}", src.display());
                        Work::Ready(Arc::new(r))
                    }
                    Err(e) => {
                        warn!("image failed: {}: {e:#}", src.display());
                        Work::Failed(format!("{e:#}"))
                    }
                };
                if let Some(k) = key {
                    self.images.lock().insert(src, (k, state));
                }
                self.bump(&dir);
            }
            Job::Md5 { image, ranges, dir } => {
                match crate::track::track_md5s(&image, &ranges) {
                    Ok(d) => {
                        let entries: Vec<_> = ranges.iter().copied().zip(d).collect();
                        if let Err(e) = self.cache.store_md5s(&image.path, &entries) {
                            warn!("storing MD5s for {}: {e:#}", image.path.display());
                        } else {
                            info!("track MD5s ready: {}", image.path.display());
                        }
                    }
                    Err(e) => warn!("MD5 of {}: {e:#}", image.path.display()),
                }
                self.md5_queued.lock().remove(&image.path);
                self.bump(&dir);
            }
            Job::Sacd { src, dir, mc } => {
                let key = SrcKey::of(&src).ok();
                let area = if mc { "multichannel" } else { "stereo" };
                let state = match SacdDisc::open(&src, Some(&self.cache), mc) {
                    Ok(d) => {
                        info!("SACD {area} area ready: {} ({} tracks, {} ch{})", src.display(), d.tracks.len(), d.channels, if d.dst { ", DST" } else { "" });
                        Work::Ready(Arc::new(d))
                    }
                    Err(e) => {
                        let msg = format!("{e:#}");
                        if msg.contains("no multichannel area") {
                            log::debug!("{}: {msg}", src.display());
                        } else {
                            warn!("SACD {area} area failed: {}: {msg}", src.display());
                        }
                        Work::Failed(msg)
                    }
                };
                if let Some(k) = key {
                    self.sacds.lock().insert((src, mc), (k, state));
                }
                self.bump(&dir);
            }
        }
    }

    /// Index (and if needed convert) an image.
    pub fn process_image(&self, src: &Path) -> Result<ReadyImage> {
        let is_flac = ext_lower(src).as_deref() == Some("flac");
        let (flac_path, source_tags) = if is_flac {
            (src.to_path_buf(), None)
        } else {
            let conv = self.cache.convert_image(src)?;
            let t = Tags::from_pairs(self.cache.converted_tags(&conv));
            (conv, Some(t))
        };
        let meta = FlacMeta::read(&flac_path)?;
        let idx = self.cache.index_for(&flac_path, &meta)?;
        let source_tags = source_tags.unwrap_or_else(|| Tags::from_pairs(meta.vorbis_comments()));
        let pictures: Vec<MetaBlock> = meta
            .pictures()
            .into_iter()
            .filter(|b| b.data.len() <= 2 << 20 && picture_type(&b.data) == Some(3))
            .take(1)
            .cloned()
            .collect();
        let img = FlacImage::open(flac_path, meta, idx)?;
        Ok(ReadyImage { flac: Arc::new(img), source_tags, pictures })
    }

    /// Human-readable summary of processing state, written next to the cache dir.
    pub fn write_status(&self) {
        let mut out = String::new();
        let now = SystemTime::now();
        let ts = now.duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        out.push_str(&format!("WaveMorphFS status (unix time {ts})\n\n"));
        let mut sections: Vec<(&str, Vec<String>)> = vec![("ready", vec![]), ("pending", vec![]), ("settling (still being written)", vec![]), ("FAILED", vec![])];
        let mut push = |w: &str, label: String| {
            let i = match w {
                "ready" => 0,
                "pending" => 1,
                "settling" => 2,
                _ => 3,
            };
            sections[i].1.push(label);
        };
        for (p, (_, st)) in self.images.lock().iter() {
            let label = format!("image  {}", p.display());
            match st {
                Work::Ready(_) => push("ready", label),
                Work::Pending => push("pending", label),
                Work::Settling(_) => push("settling", label),
                Work::Failed(e) => push("failed", format!("{label}\n         {e}")),
            }
        }
        for ((p, mc), (_, st)) in self.sacds.lock().iter() {
            let label = format!("SACD{} {}", if *mc { "/MC" } else { "   " }, p.display());
            match st {
                Work::Ready(d) => push("ready", format!("{label}  ({} tracks, {} ch{})", d.tracks.len(), d.channels, if d.dst { ", DST" } else { "" })),
                Work::Pending => push("pending", label),
                Work::Settling(_) => push("settling", label),
                Work::Failed(e) if e.contains("no multichannel area") => {}
                Work::Failed(e) => push("failed", format!("{label}\n         {e}")),
            }
        }
        for (title, mut items) in sections {
            items.sort();
            out.push_str(&format!("== {title}: {}\n", items.len()));
            if title != "ready" {
                for i in &items {
                    out.push_str(&format!("   {i}\n"));
                }
            }
            out.push('\n');
        }
        let path = self.cfg.cache_dir.parent().unwrap_or(&self.cfg.cache_dir).join("status.txt");
        let _ = crate::cache::atomic_write(&path, out.as_bytes());
    }

    fn bump(&self, dir: &Path) {
        let mut g = self.generations.lock();
        let e = g.entry(dir.to_path_buf()).or_insert((0, SystemTime::now()));
        e.0 += 1;
        e.1 = SystemTime::now();
    }

    fn enqueue(&self, job: Job) -> bool {
        match self.jobs.lock().as_ref() {
            Some(tx) => tx.send(job).is_ok(),
            None => false,
        }
    }

    /// Current state of an image; queues work if needed. Without workers the
    /// work is done inline.
    fn image_state(&self, src: &Path, dir: &Path) -> Work<ReadyImage> {
        let Ok(key) = SrcKey::of(src) else { return Work::Failed("stat failed".into()) };
        {
            let map = self.images.lock();
            if let Some((k, st)) = map.get(src)
                && *k == key
            {
                match st {
                    Work::Settling(until) if Instant::now() < *until => return st.clone(),
                    Work::Settling(_) => {}
                    other => return other.clone(),
                }
            }
        }
        if recently_modified(src) {
            let st = Work::Settling(Instant::now() + SETTLE);
            self.images.lock().insert(src.to_path_buf(), (key, st.clone()));
            return st;
        }
        // Cached work is cheap to reopen: do it inline so albums never vanish
        // from a listing just because the process restarted.
        let cached = self.cache.image_is_cached(src);
        self.images.lock().insert(src.to_path_buf(), (key, Work::Pending));
        if !cached && self.enqueue(Job::Image { src: src.to_path_buf(), dir: dir.to_path_buf() }) {
            return Work::Pending;
        }
        // inline mode
        let st = match self.process_image(src) {
            Ok(r) => Work::Ready(Arc::new(r)),
            Err(e) => Work::Failed(format!("{e:#}")),
        };
        self.images.lock().insert(src.to_path_buf(), (key, st.clone()));
        st
    }

    fn sacd_state(&self, src: &Path, dir: &Path, mc: bool) -> Work<SacdDisc> {
        let mkey = (src.to_path_buf(), mc);
        let Ok(key) = SrcKey::of(src) else { return Work::Failed("stat failed".into()) };
        {
            let map = self.sacds.lock();
            if let Some((k, st)) = map.get(&mkey)
                && *k == key
            {
                match st {
                    Work::Settling(until) if Instant::now() < *until => return st.clone(),
                    Work::Settling(_) => {}
                    other => return other.clone(),
                }
            }
        }
        if recently_modified(src) {
            let st = Work::Settling(Instant::now() + SETTLE);
            self.sacds.lock().insert(mkey.clone(), (key, st.clone()));
            return st;
        }
        let cached = sacd::is_cached(&self.cache, src, mc);
        self.sacds.lock().insert(mkey.clone(), (key, Work::Pending));
        if !cached && self.enqueue(Job::Sacd { src: src.to_path_buf(), dir: dir.to_path_buf(), mc }) {
            return Work::Pending;
        }
        let st = match SacdDisc::open(src, Some(&self.cache), mc) {
            Ok(d) => Work::Ready(Arc::new(d)),
            Err(e) => Work::Failed(format!("{e:#}")),
        };
        self.sacds.lock().insert(mkey.clone(), (key, st.clone()));
        st
    }

    /// Root this source path belongs to and the path relative to it.
    pub fn root_of(&self, p: &Path) -> Option<(&Root, PathBuf)> {
        self.cfg.roots.iter().find_map(|r| p.strip_prefix(&r.path).ok().map(|rel| (r, rel.to_path_buf())))
    }

    pub fn overlay_dir(&self, dir: &Path) -> Option<PathBuf> {
        let (root, rel) = self.root_of(dir)?;
        Some(self.cfg.tags_dir.join(&root.name).join(rel))
    }

    fn signature(&self, dir: &Path) -> Sig {
        let ov = self.overlay_dir(dir);
        Sig {
            dir_mtime: mtime(dir),
            sidecar_mtime: mtime(&dir.join(crate::sidecar::FILE_NAME)),
            overlay_mtime: ov.as_ref().and_then(|o| newest_mtime(o)),
            generation: self.generations.lock().get(dir).map_or(0, |g| g.0),
        }
    }

    /// Entries for a source directory (cached; rebuilt when anything relevant changes).
    pub fn list_dir(&self, dir: &Path) -> Result<Arc<Listing>> {
        if let Some(l) = self.listings.lock().get(dir).cloned()
            && l.checked.lock().elapsed() < RECHECK
        {
            return Ok(l);
        }
        let lock = self.dir_locks.lock().entry(dir.to_path_buf()).or_default().clone();
        let _g = lock.lock();
        let sig = self.signature(dir);
        if let Some(l) = self.listings.lock().get(dir).cloned()
            && l.sig == sig
            && !l.has_pending_work(self)
        {
            *l.checked.lock() = Instant::now();
            return Ok(l);
        }
        let l = Arc::new(self.build_listing(dir, sig)?);
        self.listings.lock().insert(dir.to_path_buf(), Arc::clone(&l));
        Ok(l)
    }

    fn build_listing(&self, dir: &Path, sig: Sig) -> Result<Listing> {
        let mut names: Vec<(OsString, std::fs::Metadata)> = Vec::new();
        for de in std::fs::read_dir(dir).with_context(|| format!("read_dir {}", dir.display()))? {
            let de = de?;
            let Ok(md) = std::fs::metadata(de.path()) else { continue };
            names.push((de.file_name(), md));
        }
        names.sort_by(|a, b| a.0.cmp(&b.0));
        let name_set: HashSet<String> = names.iter().map(|(n, _)| n.to_string_lossy().to_string()).collect();
        let downloading = |n: &str| name_set.contains(&format!("{n}.!qB")) || name_set.contains(&format!("{n}.!qb"));

        let overlay = self.overlay_dir(dir);
        let sidecar = match Sidecar::load(dir, overlay.as_deref()) {
            Ok(s) => s,
            Err(e) => {
                warn!("sidecar in {}: {e:#}", dir.display());
                None
            }
        };
        let sidecar_mtime = sidecar.as_ref().and_then(|s| s.mtime);
        let mut hidden: HashSet<OsString> = HashSet::new();
        let mut virtuals: Vec<Entry> = Vec::new();
        let mut pending = false;
        let mut retry_at: Option<Instant> = None;
        let mut note_settle = |until: Instant| retry_at = Some(retry_at.map_or(until, |r: Instant| r.min(until)));

        // --- CUE + image groups
        let mut groups: Vec<(OsString, CueSheet, OsString)> = Vec::new();
        for (n, md) in &names {
            if !md.is_file() || ext_lower(Path::new(n)).as_deref() != Some("cue") {
                continue;
            }
            let cue = match CueSheet::read(&dir.join(n)) {
                Ok(c) if c.is_image() => c,
                Ok(_) => continue,
                Err(e) => {
                    warn!("cue {}: {e:#}", dir.join(n).display());
                    continue;
                }
            };
            if let Some(img) = resolve_image(&cue, n, &names) {
                groups.push((n.clone(), cue, img));
            } else if cue.files.first().is_some_and(|f| downloading(Path::new(f).file_name().map(|x| x.to_string_lossy().to_string()).as_deref().unwrap_or(""))) {
                // image still downloading under its final name + .!qB
                hidden.insert(n.clone());
                pending = true;
            }
        }
        let multi = groups.len() > 1;
        for (gi, (cue_name, cue, img_name)) in groups.iter().enumerate() {
            let img_path = dir.join(img_name);
            if downloading(&img_name.to_string_lossy()) {
                hidden.insert(cue_name.clone());
                hidden.insert(img_name.clone());
                pending = true;
                continue;
            }
            match self.image_state(&img_path, dir) {
                Work::Ready(ready) => {
                    let disc = cue.fields.get("DISCNUMBER").and_then(|d| d.split('/').next()?.trim().parse::<u32>().ok()).or(if multi { Some(gi as u32 + 1) } else { None });
                    match self.image_tracks(dir, cue, &ready, disc, multi, groups.len(), sidecar.as_ref(), &[mtime(&dir.join(cue_name)), mtime(&img_path), sidecar_mtime]) {
                        Ok(v) => {
                            hidden.insert(cue_name.clone());
                            hidden.insert(img_name.clone());
                            virtuals.extend(v);
                        }
                        Err(e) => warn!("splitting {}: {e:#}", img_path.display()),
                    }
                }
                Work::Pending | Work::Settling(_) => {
                    if let Work::Settling(u) = self.image_state(&img_path, dir) {
                        note_settle(u);
                    }
                    hidden.insert(cue_name.clone());
                    hidden.insert(img_name.clone());
                    pending = true;
                }
                Work::Failed(_) => {} // leave cue + image visible as-is
            }
        }

        // --- SACD ISOs
        let isos: Vec<&(OsString, std::fs::Metadata)> = names.iter().filter(|(n, md)| md.is_file() && ext_lower(Path::new(n)).as_deref() == Some("iso")).collect();
        let multi_iso = isos.len() > 1;
        for (ii, (n, _)) in isos.iter().enumerate() {
            let p = dir.join(n);
            if downloading(&n.to_string_lossy()) || !sacd::is_sacd(&p) {
                continue;
            }
            let disc_no = if multi_iso { Some(ii as u32 + 1) } else { None };
            match self.sacd_state(&p, dir, false) {
                Work::Ready(disc) => {
                    virtuals.extend(self.sacd_tracks(dir, &disc, disc_no, isos.len(), sidecar.as_ref(), &[mtime(&p), sidecar_mtime], false));
                    hidden.insert(n.clone());
                    if self.cfg.sacd_multichannel
                        && let Work::Ready(mc) = self.sacd_state(&p, dir, true)
                    {
                        virtuals.extend(self.sacd_tracks(dir, &mc, disc_no, isos.len(), sidecar.as_ref(), &[mtime(&p), sidecar_mtime], true));
                    }
                }
                Work::Pending | Work::Settling(_) => {
                    if let Work::Settling(u) = self.sacd_state(&p, dir, false) {
                        note_settle(u);
                    }
                    hidden.insert(n.clone());
                    pending = true;
                }
                Work::Failed(_) => {}
            }
        }

        // --- everything else
        let mut entries: Vec<Entry> = Vec::new();
        let mut has_cover = false;
        for (n, md) in &names {
            let s = n.to_string_lossy();
            if hidden.contains(n) || is_ignored(&s) {
                continue;
            }
            let p = dir.join(n);
            let mt = md.modified().unwrap_or(SystemTime::UNIX_EPOCH);
            if md.is_dir() {
                entries.push(Entry { name: n.clone(), kind: EntryKind::Dir(p), mtime: mt });
                continue;
            }
            if is_cover_name(&s) {
                has_cover = true;
            }
            let vf: Arc<dyn VFile> = match (&sidecar, ext_lower(&p).as_deref()) {
                (Some(sc), Some("flac")) if !sc.album.is_empty() || sc.file(&s).is_some() => {
                    let mut ov = sc.album.clone();
                    if let Some(f) = sc.file(&s) {
                        ov.overlay(f);
                    }
                    match FlacMeta::read(&p).and_then(|m| RetagFlac::new(p.clone(), &m, &ov)) {
                        Ok(r) => Arc::new(r),
                        Err(e) => {
                            warn!("retag {}: {e:#}", p.display());
                            Arc::new(Passthrough { path: p.clone(), size: md.len() })
                        }
                    }
                }
                _ => Arc::new(Passthrough { path: p.clone(), size: md.len() }),
            };
            let mt = mt.max(if matches!(ext_lower(&p).as_deref(), Some("flac")) { sidecar_mtime.unwrap_or(mt) } else { mt });
            entries.push(Entry { name: n.clone(), kind: EntryKind::File(vf), mtime: mt });
        }

        // cover art exported into the sidecar tree
        if !has_cover
            && !virtuals.is_empty()
            && let Some(ov) = &overlay
        {
            for c in ["cover.jpg", "cover.png"] {
                let p = ov.join(c);
                if let Ok(md) = std::fs::metadata(&p) {
                    entries.push(Entry { name: c.into(), kind: EntryKind::File(Arc::new(Passthrough { path: p, size: md.len() })), mtime: md.modified().unwrap_or(SystemTime::UNIX_EPOCH) });
                    break;
                }
            }
        }

        // virtual tracks, de-duplicated against real names
        let taken: HashSet<OsString> = entries.iter().map(|e| e.name.clone()).collect();
        for mut v in virtuals {
            let mut name = v.name.clone();
            let mut i = 2;
            while taken.contains(&name) {
                let s = v.name.to_string_lossy();
                let (stem, ext) = s.rsplit_once('.').unwrap_or((&s, ""));
                name = format!("{stem} ({i}).{ext}").into();
                i += 1;
            }
            v.name = name;
            entries.push(v);
        }
        entries.sort_by(|a, b| a.name.cmp(&b.name));

        let gen_time = self.generations.lock().get(dir).map(|g| g.1);
        let dir_mt = [sig.dir_mtime, sig.sidecar_mtime, sig.overlay_mtime, gen_time].into_iter().flatten().max().unwrap_or(SystemTime::UNIX_EPOCH);
        if pending {
            // make sure someone re-checks soon even if nothing else changes
            let _ = pending;
        }
        Ok(Listing { entries, mtime: dir_mt, sig, checked: Mutex::new(Instant::now()), retry_at })
    }

    #[allow(clippy::too_many_arguments)]
    fn image_tracks(
        &self,
        _dir: &Path,
        cue: &CueSheet,
        ready: &ReadyImage,
        disc: Option<u32>,
        multi: bool,
        ndiscs: usize,
        sidecar: Option<&Sidecar>,
        mtimes: &[Option<SystemTime>],
    ) -> Result<Vec<Entry>> {
        let img = &ready.flac;
        let si = img.si();
        if si.sample_rate % 75 != 0 {
            bail!("sample rate {} is not a multiple of 75 (cue frames)", si.sample_rate);
        }
        let spf = si.sample_rate as u64 / 75;
        let total = si.total_samples;
        let base = {
            let mut t = ready.source_tags.without_track_specific();
            t.overlay(&tags::from_cue_disc(&cue.fields));
            t
        };
        let mt = mtimes.iter().flatten().max().copied().unwrap_or(SystemTime::UNIX_EPOCH);
        let ntracks = cue.tracks.len();
        let ranges: Vec<(u64, u64)> = cue
            .tracks
            .iter()
            .enumerate()
            .map(|(i, t)| (t.index01 * spf, cue.tracks.get(i + 1).map(|n| n.index01 * spf).unwrap_or(total).min(total)))
            .collect();
        let md5s = self.cache.load_md5s(&img.path);
        if ranges.iter().any(|r| !md5s.contains_key(r)) && self.md5_queued.lock().insert(img.path.clone()) {
            let queued = self.enqueue(Job::Md5 { image: Arc::clone(img), ranges: ranges.clone(), dir: _dir.to_path_buf() });
            if !queued {
                self.md5_queued.lock().remove(&img.path);
            }
        }
        let mut out = Vec::new();
        for (i, t) in cue.tracks.iter().enumerate() {
            let (s, e) = ranges[i];
            if s >= e {
                bail!("track {} starts beyond the end of the image", t.number);
            }
            let mut tg = base.clone();
            tg.overlay(&tags::from_cue_track(&t.fields));
            tg.set("TRACKNUMBER", t.number.to_string());
            tg.set("TRACKTOTAL", ntracks.to_string());
            if let Some(d) = disc {
                tg.set("DISCNUMBER", d.to_string());
                if multi {
                    tg.set("DISCTOTAL", ndiscs.to_string());
                }
            }
            if tg.get("ARTIST").is_none()
                && let Some(aa) = tg.0.get("ALBUMARTIST").cloned()
            {
                tg.set_many("ARTIST", aa);
            }
            if let Some(sc) = sidecar {
                tg.overlay(&sc.album);
                if let Some(tt) = sc.track(if multi { disc } else { None }, t.number) {
                    tg.overlay(tt);
                }
            }
            let title = tg.get("TITLE").map(tags::sanitize_name).unwrap_or_else(|| format!("Track {:02}", t.number));
            let name = match (multi, disc) {
                (true, Some(d)) => format!("{d}-{:02} - {title}.flac", t.number),
                _ => format!("{:02} - {title}.flac", t.number),
            };
            if let Some(sc) = sidecar
                && let Some(ft) = sc.file(&name)
            {
                tg.overlay(ft);
            }
            let tr = FlacTrack::new(Arc::clone(img), s, e, &tg, &ready.pictures, md5s.get(&(s, e)).copied())?;
            out.push(Entry { name: name.into(), kind: EntryKind::File(Arc::new(tr)), mtime: mt });
        }
        Ok(out)
    }

    #[allow(clippy::too_many_arguments)]
    fn sacd_tracks(&self, _dir: &Path, disc: &Arc<SacdDisc>, disc_no: Option<u32>, ndiscs: usize, sidecar: Option<&Sidecar>, mtimes: &[Option<SystemTime>], mc: bool) -> Vec<Entry> {
        let mt = mtimes.iter().flatten().max().copied().unwrap_or(SystemTime::UNIX_EPOCH);
        let n = disc.tracks.len();
        let mut out = Vec::new();
        for (i, _) in disc.tracks.iter().enumerate() {
            let num = i as u32 + 1;
            let mut tg = disc.track_tags(i);
            tg.set("TRACKNUMBER", num.to_string());
            tg.set("TRACKTOTAL", n.to_string());
            if let Some(d) = disc_no {
                tg.set("DISCNUMBER", d.to_string());
                tg.set("DISCTOTAL", ndiscs.to_string());
            }
            if let Some(sc) = sidecar {
                tg.overlay(&sc.album);
                if let Some(tt) = sc.track(disc_no, num) {
                    tg.overlay(tt);
                }
            }
            if mc {
                // a separate album in music servers, e.g. "Pictures at an Exhibition (Multichannel)"
                let album = tg.get("ALBUM").unwrap_or("SACD").to_string();
                tg.set("ALBUM", format!("{album} (Multichannel)"));
            }
            let title = tg.get("TITLE").map(tags::sanitize_name).unwrap_or_else(|| format!("Track {num:02}"));
            let prefix = if mc { "MC " } else { "" };
            let name = match disc_no {
                Some(d) => format!("{prefix}{d}-{num:02} - {title}.dsf"),
                None => format!("{prefix}{num:02} - {title}.dsf"),
            };
            if let Some(sc) = sidecar
                && let Some(ft) = sc.file(&name)
            {
                tg.overlay(ft);
            }
            let vf = sacd::DsfTrack::new(Arc::clone(disc), i, &tg);
            out.push(Entry { name: name.into(), kind: EntryKind::File(Arc::new(vf)), mtime: mt });
        }
        out
    }
}

impl Listing {
    fn has_pending_work(&self, _lib: &Library) -> bool {
        self.retry_at.is_some_and(|t| Instant::now() >= t)
    }
}

/// Find the audio file a single-FILE cue refers to. Rips are often converted
/// after the cue was written (cue says .wav, file is .flac), so fall back to
/// matching by stem, then by the cue's own stem.
fn resolve_image(cue: &CueSheet, cue_name: &OsStr, names: &[(OsString, std::fs::Metadata)]) -> Option<OsString> {
    let referenced = Path::new(&cue.files[0].replace('\\', "/")).file_name()?.to_string_lossy().to_string();
    let is_audio = |n: &str| AUDIO_IMAGE_EXT.contains(&ext_lower(Path::new(n)).unwrap_or_default().as_str());
    let files: Vec<&OsString> = names.iter().filter(|(_, md)| md.is_file()).map(|(n, _)| n).collect();
    if let Some(n) = files.iter().find(|n| n.to_string_lossy() == referenced && is_audio(&referenced)) {
        return Some((*n).clone());
    }
    // case-insensitive exact
    if let Some(n) = files.iter().find(|n| n.to_string_lossy().eq_ignore_ascii_case(&referenced) && is_audio(&referenced)) {
        return Some((*n).clone());
    }
    let stem_of = |s: &str| Path::new(s).file_stem().map(|x| x.to_string_lossy().to_string()).unwrap_or_default();
    let ref_stem = stem_of(&referenced);
    let cue_stem = stem_of(&cue_name.to_string_lossy());
    for want in [&ref_stem, &cue_stem] {
        if let Some(n) = files.iter().find(|n| {
            let s = n.to_string_lossy();
            is_audio(&s) && stem_of(&s) == **want
        }) {
            return Some((*n).clone());
        }
    }
    None
}

fn picture_type(data: &[u8]) -> Option<u32> {
    data.get(0..4).map(|b| u32::from_be_bytes(b.try_into().unwrap()))
}

fn ext_lower(p: &Path) -> Option<String> {
    p.extension().map(|e| e.to_string_lossy().to_ascii_lowercase())
}

fn is_ignored(name: &str) -> bool {
    let l = name.to_ascii_lowercase();
    name.starts_with('.') || l.ends_with(".!qb") || l.ends_with(".parts") || l == crate::sidecar::FILE_NAME
}

fn is_cover_name(name: &str) -> bool {
    let l = name.to_ascii_lowercase();
    let stem = l.rsplit_once('.').map(|(s, _)| s).unwrap_or(&l);
    matches!(stem, "cover" | "folder" | "front" | "albumart" | "album") && (l.ends_with(".jpg") || l.ends_with(".jpeg") || l.ends_with(".png") || l.ends_with(".webp"))
}

fn mtime(p: &Path) -> Option<SystemTime> {
    std::fs::metadata(p).ok()?.modified().ok()
}

fn newest_mtime(dir: &Path) -> Option<SystemTime> {
    let mut best = mtime(dir)?;
    for e in std::fs::read_dir(dir).ok()?.flatten() {
        if let Ok(md) = e.metadata()
            && md.is_file()
            && let Ok(m) = md.modified()
        {
            best = best.max(m);
        }
    }
    Some(best)
}

fn recently_modified(p: &Path) -> bool {
    mtime(p).and_then(|m| m.elapsed().ok()).is_some_and(|age| age < SETTLE)
}

#[allow(dead_code)]
fn _unused(_: Receiver<()>) {}
