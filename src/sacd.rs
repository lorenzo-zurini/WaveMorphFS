//! SACD ISO reader (Scarlet Book) and DSF track generator.
//!
//! Plain-DSD and DST-compressed areas are supported (DST is decoded with the
//! port in `dst.rs`). The stereo area is used.
//! A one-time sequential scan of the area records where every 1/75 s audio
//! frame starts; DSF output is then produced on demand by de-interleaving the
//! byte-interleaved channels into 4096-byte blocks and reversing bit order
//! (SACD stores DSD MSB-first, DSF LSB-first). Audio bits are untouched.

use crate::cache::{Cache, SrcKey, atomic_write};
use crate::dst::DstDecoder;
use crate::id3;
use crate::tags::Tags;
use crate::vfile::{VFile, copy_overlap, read_full_at};
use anyhow::{Context, Result, bail, ensure};
use parking_lot::Mutex;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;

const SECTOR: u64 = 2048;
const MASTER_TOC: u64 = 510;
/// bytes per channel per 1/75 s frame at 64 x 44.1 kHz
pub const FRAME_BYTES: u64 = 2_822_400 / 8 / 75; // 4704
const DSF_BLOCK: u64 = 4096;

pub fn is_sacd(path: &Path) -> bool {
    let Ok(f) = File::open(path) else { return false };
    let mut b = [0u8; 8];
    f.read_exact_at(&mut b, MASTER_TOC * SECTOR).is_ok() && &b == b"SACDMTOC"
}

#[derive(Debug, Clone)]
pub struct SacdTrack {
    /// first and one-past-last audio frame (area-relative)
    pub start: u64,
    pub end: u64,
    pub title: Option<String>,
    pub performer: Option<String>,
    pub songwriter: Option<String>,
    pub composer: Option<String>,
    pub arranger: Option<String>,
}

pub struct SacdDisc {
    pub path: PathBuf,
    pub channels: u32,
    pub tracks: Vec<SacdTrack>,
    pub album: Tags,
    /// per frame: (sector, byte offset of the frame's first audio packet)
    frames: Vec<(u32, u16)>,
    /// last sector of the area's audio
    area_end: u64,
    pub dst: bool,
    /// small cache of recently decoded frame runs: (first frame, per-channel bytes)
    recent: Mutex<Option<(u64, Vec<Vec<u8>>)>>,
}

static BITREV: [u8; 256] = {
    let mut t = [0u8; 256];
    let mut i = 0;
    while i < 256 {
        t[i] = (i as u8).reverse_bits();
        i += 1;
    }
    t
};

fn be16(b: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([b[o], b[o + 1]])
}
fn be32(b: &[u8], o: usize) -> u32 {
    u32::from_be_bytes(b[o..o + 4].try_into().unwrap())
}
fn tc(b: &[u8], o: usize) -> u64 {
    (b[o] as u64 * 60 + b[o + 1] as u64) * 75 + b[o + 2] as u64
}

fn decode_text(bytes: &[u8], charset: u8) -> String {
    let enc = match charset {
        3 => encoding_rs::SHIFT_JIS,
        4 => encoding_rs::EUC_KR,
        5 => encoding_rs::GBK,
        6 => encoding_rs::BIG5,
        _ => encoding_rs::WINDOWS_1252, // ISO646 / ISO-8859-1
    };
    let (s, _, _) = enc.decode(bytes);
    s.trim().to_string()
}

fn cstr_at(b: &[u8], o: usize, charset: u8) -> Option<String> {
    if o == 0 || o >= b.len() {
        return None;
    }
    let end = b[o..].iter().position(|&c| c == 0).map_or(b.len(), |e| o + e);
    let s = decode_text(&b[o..end], charset);
    (!s.is_empty()).then_some(s)
}

const GENRES: &[&str] = &[
    "", "", "", "Adult Contemporary", "Alternative Rock", "Children's Music", "Classical", "Contemporary Christian", "Country", "Dance",
    "Easy Listening", "Erotic", "Folk", "Gospel", "Hip Hop", "Jazz", "Latin", "Musical", "New Age", "Opera", "Operetta", "Pop Music", "Rap",
    "Reggae", "Rock Music", "Rhythm & Blues", "Sound Effects", "Soundtrack", "Spoken Word", "World Music",
];

/// One parsed audio sector.
struct AudioSector {
    dst: bool,
    /// (offset in sector, length, data_type, frame_start)
    packets: Vec<(usize, usize, u8, bool)>,
    /// time codes of frames starting in this sector, in order
    frame_tcs: Vec<u64>,
}

fn parse_sector(s: &[u8]) -> Result<AudioSector> {
    let h = s[0];
    let npk = (h >> 5) as usize;
    let nfi = ((h >> 2) & 7) as usize;
    let dst = h & 1 == 1;
    let mut p = 1;
    let mut pk = Vec::with_capacity(npk);
    for _ in 0..npk {
        let w = be16(s, p);
        p += 2;
        pk.push(((w >> 15) & 1 == 1, ((w >> 11) & 7) as u8, (w & 0x7FF) as usize));
    }
    let mut tcs = Vec::with_capacity(nfi);
    for _ in 0..nfi {
        tcs.push(tc(s, p));
        p += if dst { 4 } else { 3 };
    }
    let mut packets = Vec::with_capacity(npk);
    for (fs, dt, len) in pk {
        ensure!(p + len <= SECTOR as usize, "packet overruns sector");
        packets.push((p, len, dt, fs));
        p += len;
    }
    Ok(AudioSector { dst, packets, frame_tcs: tcs })
}

const DATA_AUDIO: u8 = 2;
const IDX_MAGIC: &[u8; 8] = b"WMSACD01";

impl SacdDisc {
    pub fn open(path: &Path, cache: Option<&Cache>) -> Result<SacdDisc> {
        let f = File::open(path)?;
        let flen = f.metadata()?.len();
        let sec = |n: u64, count: u64| -> Result<Vec<u8>> {
            let mut b = vec![0u8; (SECTOR * count) as usize];
            ensure!(read_full_at(&f, &mut b, n * SECTOR)? == b.len(), "short read at sector {n}");
            Ok(b)
        };
        let m = sec(MASTER_TOC, 1)?;
        ensure!(&m[..8] == b"SACDMTOC", "no SACD master TOC");
        let area1 = be32(&m, 64) as u64;
        let area2 = be32(&m, 72) as u64;
        let catalog = String::from_utf8_lossy(&m[24..40]).trim().to_string();
        let set_size = be16(&m, 16);
        let set_seq = be16(&m, 18);
        let year = be16(&m, 120);
        let genre = (m[104] == 1).then(|| GENRES.get(be16(&m, 106) as usize).copied()).flatten().filter(|g| !g.is_empty());
        let master_charset = m[138]; // first locale: [lang, lang, charset, reserved] at 136

        // choose the stereo area, fall back to multichannel if it is the only one
        let area_start = if area1 != 0 { area1 } else { area2 };
        ensure!(area_start != 0, "no audio area");
        let at = sec(area_start, 1)?;
        ensure!(&at[..8] == b"TWOCHTOC" || &at[..8] == b"MULCHTOC", "bad area TOC signature");
        let toc_size = be16(&at, 10) as u64;
        let frame_format = at[0x15] & 0x0F;
        ensure!(matches!(frame_format, 0 | 2 | 3), "unknown frame format {frame_format}");
        let dst = frame_format == 0;
        ensure!(at[0x14] == 4, "unsupported sample rate code {}", at[0x14]);
        let channels = at[0x20] as u32;
        ensure!((1..=6).contains(&channels), "bad channel count {channels}");
        let ntracks = at[0x45] as usize;
        let area_audio_start = be32(&at, 0x48) as u64;
        let area_audio_end = be32(&at, 0x4C) as u64;
        ensure!(ntracks >= 1 && area_audio_end > area_audio_start, "bad track info");
        ensure!(flen >= (area_audio_end + 1) * SECTOR, "ISO is truncated (still downloading?)");
        let area_charset = at[0x5A];

        // locate SACDTRL2 and SACDTTxt inside the area TOC
        let toc = sec(area_start, toc_size.max(1))?;
        let find = |sig: &[u8]| (0..toc_size as usize).map(|i| i * SECTOR as usize).find(|&o| &toc[o..o + 8] == sig);
        let trl2 = find(b"SACDTRL2").context("no SACDTRL2")?;
        let mut tracks = Vec::with_capacity(ntracks);
        for i in 0..ntracks {
            let start = tc(&toc, trl2 + 8 + 4 * i);
            let dur = tc(&toc, trl2 + 8 + 1020 + 4 * i);
            tracks.push(SacdTrack { start, end: start + dur, title: None, performer: None, songwriter: None, composer: None, arranger: None });
        }
        if let Some(tt) = find(b"SACDTTxt") {
            for (i, t) in tracks.iter_mut().enumerate() {
                let off = be16(&toc, tt + 8 + 2 * i) as usize;
                if off == 0 {
                    continue;
                }
                let base = tt + off;
                if base + 4 > toc.len() {
                    continue;
                }
                let count = toc[base] as usize;
                let mut p = base + 4;
                for _ in 0..count {
                    if p + 2 > toc.len() {
                        break;
                    }
                    let kind = toc[p];
                    let end = toc[p + 2..].iter().position(|&c| c == 0).map_or(toc.len(), |e| p + 2 + e);
                    let text = decode_text(&toc[p + 2..end], area_charset);
                    let text = (!text.is_empty()).then_some(text);
                    match kind {
                        1 => t.title = text,
                        2 => t.performer = text,
                        3 => t.songwriter = text,
                        4 => t.composer = text,
                        5 => t.arranger = text,
                        _ => {}
                    }
                    // items are padded to 4-byte boundaries relative to the entry start
                    let len = end + 1 - p;
                    p += len.div_ceil(4) * 4;
                }
            }
        }

        // album-level tags from the master text (first language)
        let mt = sec(MASTER_TOC + 1, 1)?;
        let mut album = Tags::new();
        if &mt[..8] == b"SACDText" {
            let pos = |i: usize| be16(&mt, 16 + 2 * i) as usize;
            let cs = if master_charset != 0 { master_charset } else { area_charset };
            let album_title = cstr_at(&mt, pos(0), cs).or_else(|| cstr_at(&mt, pos(8), cs));
            let album_artist = cstr_at(&mt, pos(1), cs).or_else(|| cstr_at(&mt, pos(9), cs));
            let publisher = cstr_at(&mt, pos(2), cs).or_else(|| cstr_at(&mt, pos(10), cs));
            let copyright = cstr_at(&mt, pos(3), cs).or_else(|| cstr_at(&mt, pos(11), cs));
            if let Some(v) = album_title {
                album.set("ALBUM", v);
            }
            if let Some(v) = album_artist {
                album.set("ALBUMARTIST", v);
            }
            if let Some(v) = publisher {
                album.set("LABEL", v);
            }
            if let Some(v) = copyright {
                album.set("COPYRIGHT", v);
            }
        }
        if !catalog.is_empty() {
            album.set("CATALOGNUMBER", catalog);
        }
        if (1900..2200).contains(&year) {
            album.set("DATE", year.to_string());
        }
        if let Some(g) = genre {
            album.set("GENRE", g);
        }
        if set_size > 1 && set_seq >= 1 {
            album.set("DISCNUMBER", set_seq.to_string());
            album.set("DISCTOTAL", set_size.to_string());
        }
        album.set("MEDIA", "SACD");

        let key = SrcKey::of(path)?;
        let frames = match cache.and_then(|c| load_frames(c, path, key)) {
            Some(fr) => fr,
            None => {
                let fr = scan_frames(&f, area_audio_start, area_audio_end, channels, dst)?;
                if let Some(c) = cache {
                    store_frames(c, path, key, &fr)?;
                }
                fr
            }
        };
        let last = tracks.last().unwrap().end;
        ensure!(last <= frames.len() as u64, "track list ends at frame {last} but the area has {} frames", frames.len());
        Ok(SacdDisc { path: path.to_path_buf(), channels, tracks, album, frames, area_end: area_audio_end, dst, recent: Mutex::new(None) })
    }

    pub fn track_tags(&self, i: usize) -> Tags {
        let t = &self.tracks[i];
        let mut tg = self.album.clone();
        if let Some(v) = &t.title {
            tg.set("TITLE", v);
        }
        match (&t.performer, tg.0.get("ALBUMARTIST").cloned()) {
            (Some(p), _) => tg.set("ARTIST", p),
            (None, Some(aa)) => tg.set_many("ARTIST", aa),
            _ => {}
        }
        if let Some(v) = t.composer.as_ref().or(t.songwriter.as_ref()) {
            tg.set("COMPOSER", v);
        }
        if let Some(v) = &t.arranger {
            tg.set("ARRANGER", v);
        }
        tg
    }

    /// Coded bytes of frames [f0, f1): the audio packets between each frame's
    /// start marker and the next one (fixed 4704*ch bytes for plain DSD,
    /// variable for DST).
    fn coded_frames(&self, f0: u64, f1: u64) -> Result<Vec<Vec<u8>>> {
        let start = self.frames[f0 as usize];
        let end = self.frames.get(f1 as usize).copied().unwrap_or((self.area_end as u32 + 1, 0));
        let (s0, s1) = (start.0 as u64, end.0 as u64);
        let count = s1 - s0 + 1;
        let file = File::open(&self.path)?;
        let mut buf = vec![0u8; (count * SECTOR) as usize];
        let n = read_full_at(&file, &mut buf, s0 * SECTOR)?;
        buf.truncate(n - n % SECTOR as usize);
        let mut out: Vec<Vec<u8>> = Vec::with_capacity((f1 - f0) as usize);
        'sectors: for i in 0..(buf.len() as u64 / SECTOR) {
            let sec_no = s0 + i;
            // the end marker's own sector is only part of the range if the next frame
            // starts inside it; never parse beyond (e.g. the area's backup TOC)
            if sec_no > end.0 as u64 || (sec_no == end.0 as u64 && end.1 == 0) {
                break;
            }
            let sb = &buf[(i * SECTOR) as usize..((i + 1) * SECTOR) as usize];
            let parsed = parse_sector(sb)?;
            for &(po, len, dt, fs) in &parsed.packets {
                let pos = (sec_no as u32, po as u16);
                if pos < start {
                    continue;
                }
                if pos >= end {
                    break 'sectors;
                }
                if dt == DATA_AUDIO {
                    if fs || out.is_empty() {
                        out.push(Vec::with_capacity(FRAME_BYTES as usize * self.channels as usize));
                    }
                    out.last_mut().unwrap().extend_from_slice(&sb[po..po + len]);
                }
            }
        }
        ensure!(out.len() as u64 == f1 - f0, "expected {} frames, found {}", f1 - f0, out.len());
        Ok(out)
    }

    /// Per-channel DSD bytes (MSB-first, as stored) for frames [f0, f1).
    fn read_frames(&self, f0: u64, f1: u64) -> Result<Vec<Vec<u8>>> {
        let ch = self.channels as usize;
        let per_frame = FRAME_BYTES as usize * ch;
        let coded = self.coded_frames(f0, f1)?;
        let mut out = vec![Vec::with_capacity(FRAME_BYTES as usize * coded.len()); ch];
        let mut dec = if self.dst { Some(DstDecoder::new(ch)?) } else { None };
        let mut raw = vec![0u8; per_frame];
        for (i, c) in coded.iter().enumerate() {
            let inter: &[u8] = match dec.as_mut() {
                Some(d) => {
                    d.decode(c, &mut raw).map_err(|e| anyhow::anyhow!("frame {}: {e}", f0 + i as u64))?;
                    &raw
                }
                None => {
                    ensure!(c.len() == per_frame, "frame {} has {} bytes", f0 + i as u64, c.len());
                    c
                }
            };
            // de-interleave (byte-interleaved channels)
            for (j, b) in inter.iter().enumerate() {
                out[j % ch].push(*b);
            }
        }
        Ok(out)
    }

    /// Per-channel bytes for track-relative channel byte range, with a small cache.
    fn channel_bytes(&self, f_first: u64, f_last: u64) -> Result<(u64, Vec<Vec<u8>>)> {
        if let Some((f, v)) = self.recent.lock().as_ref()
            && *f <= f_first
            && f + (v[0].len() as u64 / FRAME_BYTES) >= f_last
        {
            return Ok((*f, v.clone()));
        }
        let v = self.read_frames(f_first, f_last)?;
        *self.recent.lock() = Some((f_first, v.clone()));
        Ok((f_first, v))
    }
}

fn scan_frames(f: &File, start: u64, end: u64, channels: u32, dst: bool) -> Result<Vec<(u32, u16)>> {
    let mut frames: Vec<(u32, u16)> = Vec::new();
    let frame_bytes = FRAME_BYTES as usize * channels as usize;
    let mut acc = 0usize; // audio bytes of the current frame seen so far
    const CHUNK: u64 = 2048; // sectors per read (4 MiB)
    let mut s = start;
    while s <= end {
        let n = CHUNK.min(end + 1 - s);
        let mut buf = vec![0u8; (n * SECTOR) as usize];
        ensure!(read_full_at(f, &mut buf, s * SECTOR)? == buf.len(), "short read at sector {s}");
        for i in 0..n {
            let sec_no = s + i;
            let sb = &buf[(i * SECTOR) as usize..((i + 1) * SECTOR) as usize];
            let p = parse_sector(sb).with_context(|| format!("sector {sec_no}"))?;
            ensure!(p.dst == dst, "sector {sec_no}: DST flag {} in a {} area", p.dst, if dst { "DST" } else { "plain DSD" });
            let mut tc_iter = p.frame_tcs.iter();
            for &(po, len, dt, fs) in &p.packets {
                if dt != DATA_AUDIO {
                    continue;
                }
                if fs {
                    ensure!(dst || frames.is_empty() || acc == frame_bytes, "frame {} has {acc} bytes, expected {frame_bytes}", frames.len() - 1);
                    let expect = frames.len() as u64;
                    if let Some(&t) = tc_iter.next() {
                        ensure!(t == expect, "sector {sec_no}: time code {t} where frame {expect} was expected");
                    }
                    frames.push((sec_no as u32, po as u16));
                    acc = 0;
                }
                acc += len;
            }
        }
        s += n;
    }
    ensure!(!frames.is_empty(), "no audio frames found");
    ensure!(dst || acc == frame_bytes, "last frame incomplete ({acc} bytes)");
    Ok(frames)
}

fn frames_cache_path(c: &Cache, src: &Path, key: SrcKey) -> PathBuf {
    let idx = c.dir.join("flacidx").join("x.idx");
    let dir = idx.parent().unwrap().to_path_buf();
    let mut h = std::collections::hash_map::DefaultHasher::new();
    std::hash::Hash::hash(&src, &mut h);
    std::hash::Hash::hash(&key, &mut h);
    dir.join(format!("{:016x}.sacdidx", std::hash::Hasher::finish(&h)))
}

fn load_frames(c: &Cache, src: &Path, key: SrcKey) -> Option<Vec<(u32, u16)>> {
    let b = std::fs::read(frames_cache_path(c, src, key)).ok()?;
    if b.len() < 16 || &b[..8] != IDX_MAGIC {
        return None;
    }
    let n = u64::from_le_bytes(b[8..16].try_into().ok()?) as usize;
    if b.len() != 16 + n * 6 {
        return None;
    }
    Some(b[16..].chunks_exact(6).map(|c| (u32::from_le_bytes(c[..4].try_into().unwrap()), u16::from_le_bytes([c[4], c[5]]))).collect())
}

fn store_frames(c: &Cache, src: &Path, key: SrcKey, fr: &[(u32, u16)]) -> Result<()> {
    let mut b = IDX_MAGIC.to_vec();
    b.extend_from_slice(&(fr.len() as u64).to_le_bytes());
    for (s, o) in fr {
        b.extend_from_slice(&s.to_le_bytes());
        b.extend_from_slice(&o.to_le_bytes());
    }
    atomic_write(&frames_cache_path(c, src, key), &b)
}

/// One SACD track as a DSF file.
pub struct DsfTrack {
    disc: Arc<SacdDisc>,
    f0: u64,
    bytes_per_ch: u64,
    header: Vec<u8>,
    data_len: u64,
    id3: Vec<u8>,
}

impl DsfTrack {
    pub fn new(disc: Arc<SacdDisc>, track: usize, tags: &Tags) -> DsfTrack {
        let t = &disc.tracks[track];
        let ch = disc.channels as u64;
        let bytes_per_ch = (t.end - t.start) * FRAME_BYTES;
        let data_len = bytes_per_ch.div_ceil(DSF_BLOCK) * DSF_BLOCK * ch;
        let id3 = id3::build(tags);
        let total = 28 + 52 + 12 + data_len + id3.len() as u64;
        let mut h = Vec::with_capacity(92);
        h.extend_from_slice(b"DSD ");
        h.extend_from_slice(&28u64.to_le_bytes());
        h.extend_from_slice(&total.to_le_bytes());
        h.extend_from_slice(&(28 + 52 + 12 + data_len).to_le_bytes()); // metadata pointer
        h.extend_from_slice(b"fmt ");
        h.extend_from_slice(&52u64.to_le_bytes());
        h.extend_from_slice(&1u32.to_le_bytes()); // format version
        h.extend_from_slice(&0u32.to_le_bytes()); // DSD raw
        let ch_type: u32 = match ch {
            1 => 1,
            2 => 2,
            3 => 3,
            4 => 4,
            5 => 6,
            _ => 7,
        };
        h.extend_from_slice(&ch_type.to_le_bytes());
        h.extend_from_slice(&(ch as u32).to_le_bytes());
        h.extend_from_slice(&2_822_400u32.to_le_bytes());
        h.extend_from_slice(&1u32.to_le_bytes()); // bits per sample: 1 = LSB first
        h.extend_from_slice(&(bytes_per_ch * 8).to_le_bytes()); // sample count per channel
        h.extend_from_slice(&(DSF_BLOCK as u32).to_le_bytes());
        h.extend_from_slice(&0u32.to_le_bytes());
        h.extend_from_slice(b"data");
        h.extend_from_slice(&(12 + data_len).to_le_bytes());
        DsfTrack { f0: t.start, disc, bytes_per_ch, header: h, data_len, id3 }
    }

    /// Bytes [a, b) of the data region.
    fn read_data(&self, a: u64, b: u64, out: &mut Vec<u8>) -> Result<()> {
        let ch = self.disc.channels as u64;
        let group = DSF_BLOCK * ch;
        let g0 = a / group;
        let g1 = (b - 1) / group;
        // channel byte range needed
        let j0 = g0 * DSF_BLOCK;
        let j1 = ((g1 + 1) * DSF_BLOCK).min(self.bytes_per_ch);
        let frames = if j0 < j1 {
            let fa = self.f0 + j0 / FRAME_BYTES;
            let fb = self.f0 + j1.div_ceil(FRAME_BYTES);
            Some(self.disc.channel_bytes(fa, fb)?)
        } else {
            None
        };
        let mut pos = a;
        while pos < b {
            let g = pos / group;
            let c = (pos % group) / DSF_BLOCK;
            let i = pos % DSF_BLOCK;
            let run = (DSF_BLOCK - i).min(b - pos);
            let j = g * DSF_BLOCK + i; // channel byte index
            for k in 0..run {
                let jj = j + k;
                let byte = if jj < self.bytes_per_ch {
                    let (ff, v) = frames.as_ref().unwrap();
                    let idx = (self.f0 * FRAME_BYTES + jj) - ff * FRAME_BYTES;
                    BITREV[v[c as usize][idx as usize] as usize]
                } else {
                    0
                };
                out.push(byte);
            }
            pos += run;
        }
        Ok(())
    }
}

impl VFile for DsfTrack {
    fn size(&self) -> u64 {
        self.header.len() as u64 + self.data_len + self.id3.len() as u64
    }

    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        let size = self.size();
        let mut out = Vec::with_capacity(len);
        if off >= size {
            return Ok(out);
        }
        let len = len.min((size - off) as usize);
        let end = off + len as u64;
        let h = self.header.len() as u64;
        copy_overlap(&mut out, &self.header, 0, off, len);
        if off < h + self.data_len && end > h {
            let a = off.max(h) - h;
            let b = end.min(h + self.data_len) - h;
            self.read_data(a, b, &mut out)?;
        }
        copy_overlap(&mut out, &self.id3, h + self.data_len, off, len);
        ensure!(out.len() == len, "internal: produced {} of {len}", out.len());
        Ok(out)
    }

    fn describe(&self) -> String {
        format!("sacd:{}#frames={}+{}", self.disc.path.display(), self.f0, self.bytes_per_ch / FRAME_BYTES)
    }
}
