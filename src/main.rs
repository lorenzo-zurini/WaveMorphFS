mod cache;
mod cue;
mod dst;
mod flac;
mod fs;
mod id3;
mod mp4;
mod retag;
mod sacd;
mod scan;
mod sidecar;
mod tags;
mod track;
mod vfile;

use anyhow::{Context, Result, bail};
use clap::{Parser, Subcommand};
use scan::{Config, EntryKind, Library, Root};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use vfile::VFile;

#[derive(Parser)]
#[command(
    name = "wavemorphfs",
    about = "Present pristine audio downloads as split, tagged tracks via FUSE"
)]
struct Cli {
    /// Library root as NAME=PATH (repeatable)
    #[arg(long = "root", global = true, value_parser = parse_root)]
    roots: Vec<Root>,
    /// Directory holding sidecar tag trees (<tags-dir>/<root name>/<relative dir>/wavemorph.toml)
    #[arg(long, global = true, default_value = "~/Storage/WaveMorph/tags")]
    tags_dir: String,
    /// Cache directory (frame indexes, converted images)
    #[arg(long, global = true, default_value = "~/Storage/WaveMorph/cache")]
    cache_dir: String,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Mount the filesystem (runs in the foreground)
    Mount {
        mountpoint: PathBuf,
        #[arg(long, default_value_t = 2)]
        workers: usize,
        #[arg(long, default_value_t = 8)]
        threads: usize,
        /// Let other users (e.g. a container's user or root) access the mount
        #[arg(long)]
        allow_other: bool,
        /// Re-walk the library this often (seconds) to pre-process new albums
        #[arg(long, default_value_t = 900)]
        prescan: u64,
        /// Also expose SACD multichannel areas as separate "(Multichannel)" albums
        /// (env WAVEMORPH_SACD_MULTICHANNEL=1; default off)
        #[arg(long, env = "WAVEMORPH_SACD_MULTICHANNEL")]
        sacd_multichannel: bool,
    },
    /// Show what a source directory looks like through the filesystem
    Ls { dir: PathBuf },
    /// Validate every virtual track in a source directory with the reference decoders
    Verify { dir: PathBuf },
    /// Compare the virtual tracks of DIR with already-split files in SPLIT_DIR (PCM MD5)
    Compare { dir: PathBuf, split_dir: PathBuf },
    /// Write a sidecar pre-filled with the current effective tags of DIR
    TagsInit {
        dir: PathBuf,
        #[arg(long)]
        force: bool,
    },
}

fn expand(p: &str) -> PathBuf {
    match p.strip_prefix("~/") {
        Some(rest) => PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(rest),
        None => PathBuf::from(p),
    }
}

fn parse_root(s: &str) -> Result<Root, String> {
    let (n, p) = s.split_once('=').ok_or("expected NAME=PATH")?;
    Ok(Root {
        name: n.to_string(),
        path: expand(p),
    })
}

fn default_roots() -> Vec<Root> {
    let home = expand("~/Storage");
    vec![
        Root {
            name: "Music".into(),
            path: home.join("Music"),
        },
        Root {
            name: "Classical Music".into(),
            path: home.join("Classical Music"),
        },
    ]
}

fn library(cli: &Cli, workers: usize) -> Result<Arc<Library>> {
    let sacd_multichannel = matches!(
        cli.cmd,
        Cmd::Mount {
            sacd_multichannel: true,
            ..
        }
    );
    let roots = if cli.roots.is_empty() {
        default_roots()
    } else {
        cli.roots.clone()
    };
    for r in &roots {
        if !r.path.is_dir() {
            bail!("root {} does not exist: {}", r.name, r.path.display());
        }
    }
    Library::new(Config {
        roots,
        tags_dir: expand(&cli.tags_dir),
        cache_dir: expand(&cli.cache_dir),
        workers,
        sacd_multichannel,
    })
}

fn abs(p: &Path) -> Result<PathBuf> {
    std::fs::canonicalize(p).with_context(|| format!("{}", p.display()))
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_secs()
        .init();
    let cli = Cli::parse();
    match &cli.cmd {
        Cmd::Mount {
            mountpoint,
            workers,
            threads,
            allow_other,
            prescan,
            ..
        } => {
            let lib = library(&cli, *workers)?;
            lib.start_workers();
            lib.start_prescan(Duration::from_secs(*prescan));
            let mut cfg = fuser::Config::default();
            cfg.mount_options = vec![
                fuser::MountOption::RO,
                fuser::MountOption::FSName("wavemorphfs".into()),
                fuser::MountOption::Subtype("wavemorphfs".into()),
                fuser::MountOption::NoAtime,
            ];
            if *allow_other {
                cfg.acl = fuser::SessionACL::All;
            }
            cfg.n_threads = Some(*threads);
            log::info!("mounting at {}", mountpoint.display());
            fuser::mount(fs::WaveFs::new(lib), mountpoint, &cfg)?;
        }
        Cmd::Ls { dir } => {
            let lib = library(&cli, 0)?;
            let l = lib.list_dir(&abs(dir)?)?;
            for e in &l.entries {
                match &e.kind {
                    EntryKind::Dir(_) => println!("{:>14}  {}/", "<dir>", e.name.to_string_lossy()),
                    EntryKind::File(vf) => println!(
                        "{:>14}  {}\n{:>16}{}",
                        vf.size(),
                        e.name.to_string_lossy(),
                        "",
                        vf.describe()
                    ),
                }
            }
        }
        Cmd::Verify { dir } => {
            let lib = library(&cli, 0)?;
            let l = lib.list_dir(&abs(dir)?)?;
            let tmp = tempdir()?;
            let (mut ok, mut n) = (0, 0);
            for e in &l.entries {
                let EntryKind::File(vf) = &e.kind else {
                    continue;
                };
                let d = vf.describe();
                if !(d.starts_with("flac-image:")
                    || d.starts_with("sacd:")
                    || d.starts_with("retagged-flac:")
                    || d.starts_with("retagged-mp3:")
                    || d.starts_with("retagged-m4a:"))
                {
                    continue;
                }
                n += 1;
                let out = tmp.join(&e.name);
                dump(vf.as_ref(), &out)?;
                let good = if let Some(src) = d
                    .strip_prefix("retagged-mp3:")
                    .or_else(|| d.strip_prefix("retagged-m4a:"))
                {
                    // same compressed audio as the source, and it decodes
                    let a = stream_md5(&out);
                    a.is_some()
                        && a == stream_md5(Path::new(src))
                        && run_ok(
                            "ffmpeg",
                            &[
                                "-v",
                                "error",
                                "-xerror",
                                "-i",
                                &out.to_string_lossy(),
                                "-f",
                                "null",
                                "-",
                            ],
                            false,
                        )
                } else if d.starts_with("sacd:") {
                    run_ok(
                        "ffmpeg",
                        &[
                            "-v",
                            "error",
                            "-xerror",
                            "-i",
                            &out.to_string_lossy(),
                            "-f",
                            "null",
                            "-",
                        ],
                        true,
                    )
                } else {
                    run_ok("flac", &["-t", "-s", &out.to_string_lossy()], false)
                };
                if good {
                    ok += 1;
                }
                println!(
                    "{}  {:>11}  {}",
                    if good { "OK  " } else { "FAIL" },
                    vf.size(),
                    e.name.to_string_lossy()
                );
                std::fs::remove_file(&out).ok();
            }
            println!("{ok}/{n} virtual tracks valid");
            if ok != n {
                std::process::exit(1);
            }
        }
        Cmd::Compare { dir, split_dir } => {
            let lib = library(&cli, 0)?;
            let l = lib.list_dir(&abs(dir)?)?;
            let virt: Vec<_> = l
                .entries
                .iter()
                .filter_map(|e| match &e.kind {
                    EntryKind::File(vf) if vf.describe().starts_with("flac-image:") => {
                        Some((e.name.clone(), Arc::clone(vf)))
                    }
                    _ => None,
                })
                .collect();
            let mut split: Vec<PathBuf> = std::fs::read_dir(split_dir)?
                .flatten()
                .map(|d| d.path())
                .filter(|p| {
                    p.extension()
                        .is_some_and(|e| e.eq_ignore_ascii_case("flac"))
                })
                .collect();
            split.sort();
            if virt.len() != split.len() {
                bail!(
                    "{} virtual tracks vs {} split files",
                    virt.len(),
                    split.len()
                );
            }
            let tmp = tempdir()?;
            let mut same = 0;
            for ((name, vf), sp) in virt.iter().zip(&split) {
                let out = tmp.join(name);
                dump(vf.as_ref(), &out)?;
                let a = pcm_md5(&out)?;
                let b = pcm_md5(sp)?;
                std::fs::remove_file(&out).ok();
                let eq = !a.is_empty() && a == b;
                if eq {
                    same += 1;
                }
                println!(
                    "{}  {}  <->  {}",
                    if eq { "IDENTICAL" } else { "DIFFERENT" },
                    name.to_string_lossy(),
                    sp.file_name().unwrap().to_string_lossy()
                );
            }
            println!("{same}/{} identical", virt.len());
            if same != virt.len() {
                std::process::exit(1);
            }
        }
        Cmd::TagsInit { dir, force } => {
            let lib = library(&cli, 0)?;
            let dir = abs(dir)?;
            let ov = lib
                .overlay_dir(&dir)
                .context("directory is not inside a configured root")?;
            let target = ov.join(sidecar::FILE_NAME);
            if target.exists() && !force {
                bail!("{} exists (use --force)", target.display());
            }
            let l = lib.list_dir(&dir)?;
            let mut per: Vec<(String, tags::Tags)> = Vec::new();
            for e in &l.entries {
                let EntryKind::File(vf) = &e.kind else {
                    continue;
                };
                let name = e.name.to_string_lossy().to_string();
                if let Some(t) = read_tags(vf.as_ref(), &name) {
                    per.push((name, t));
                }
            }
            if per.is_empty() {
                bail!("no taggable audio files in {}", dir.display());
            }
            let mut album = per[0].1.clone();
            album.0.retain(|k, v| {
                per.iter().all(|(_, t)| t.0.get(k) == Some(v))
                    && !tags::TRACK_SPECIFIC.iter().any(|n| k.is(n))
            });
            let files: Vec<(String, tags::Tags)> = per
                .into_iter()
                .map(|(n, mut t)| {
                    t.0.retain(|k, _| !album.0.contains_key(k));
                    (n, t)
                })
                .collect();
            std::fs::create_dir_all(&ov)?;
            std::fs::write(&target, sidecar::Sidecar::render(&album, &[], &files))?;
            println!("wrote {}", target.display());
        }
    }
    Ok(())
}

fn tempdir() -> Result<PathBuf> {
    let d = std::env::temp_dir().join(format!("wavemorphfs-{}", std::process::id()));
    std::fs::create_dir_all(&d)?;
    Ok(d)
}

fn dump(vf: &dyn VFile, out: &Path) -> Result<()> {
    use std::io::Write;
    let mut f = std::io::BufWriter::new(std::fs::File::create(out)?);
    let mut off = 0;
    while off < vf.size() {
        let b = vf.read_at(off, 1 << 20)?;
        if b.is_empty() {
            bail!("short read at {off}");
        }
        off += b.len() as u64;
        f.write_all(&b)?;
    }
    Ok(())
}

/// Run a checker; with `strict_stderr`, any stderr output counts as failure.
fn run_ok(cmd: &str, args: &[&str], strict_stderr: bool) -> bool {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| o.status.success() && (!strict_stderr || o.stderr.is_empty()))
        .unwrap_or(false)
}

/// MD5 of a file's audio packets (stream copy, no decoding).
fn stream_md5(p: &Path) -> Option<String> {
    let o = std::process::Command::new("ffmpeg")
        .args(["-v", "error", "-i"])
        .arg(p)
        .args(["-map", "0:a", "-c", "copy", "-f", "md5", "-"])
        .output()
        .ok()?;
    o.status
        .success()
        .then(|| String::from_utf8_lossy(&o.stdout).trim().to_string())
}

fn pcm_md5(p: &Path) -> Result<String> {
    let o = std::process::Command::new("sh")
        .arg("-c")
        .arg("set -o pipefail; flac -d -c -s --force-raw-format --endian=little --sign=signed \"$1\" | md5sum")
        .arg("sh")
        .arg(p)
        .output()?;
    if !o.status.success() {
        return Ok(String::new());
    }
    Ok(String::from_utf8_lossy(&o.stdout)
        .split_whitespace()
        .next()
        .unwrap_or("")
        .to_string())
}

/// Tags of a (virtual) FLAC file, read from its generated header.
fn read_tags(vf: &dyn VFile, name: &str) -> Option<tags::Tags> {
    if !name.to_ascii_lowercase().ends_with(".flac") {
        return None;
    }
    let head = vf.read_at(0, 1 << 20).ok()?;
    let mut p = 4;
    if head.get(..4)? != b"fLaC" {
        return None;
    }
    loop {
        let h = head.get(p..p + 4)?;
        let last = h[0] & 0x80 != 0;
        let len = u32::from_be_bytes([0, h[1], h[2], h[3]]) as usize;
        if h[0] & 0x7F == flac::BLOCK_VORBIS {
            return Some(tags::Tags::from_pairs(flac::parse_vorbis(
                head.get(p + 4..p + 4 + len)?,
            )));
        }
        if last {
            return None;
        }
        p += 4 + len;
    }
}
