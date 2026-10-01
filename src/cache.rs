//! On-disk caches, keyed by source path + size + mtime so any change to the
//! source invalidates them:
//!   - `flacidx/<key>.idx`   frame index of a FLAC image
//!   - `images/<key>.flac`   lossless FLAC conversion of a non-FLAC image (APE, WV, ...)
//!   - `images/<key>.json`   conversion metadata (source tags, verified PCM MD5)

use crate::flac::{FlacMeta, FrameIndex};
use anyhow::{Context, Result, bail, ensure};
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::UNIX_EPOCH;

#[derive(Debug, Clone)]
pub struct Cache {
    pub dir: PathBuf,
}

/// Identity of a source file version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SrcKey {
    pub size: u64,
    pub mtime_ns: i128,
}

impl SrcKey {
    pub fn of(path: &Path) -> Result<SrcKey> {
        let md = std::fs::metadata(path)?;
        let mt = md.modified()?;
        let ns = match mt.duration_since(UNIX_EPOCH) {
            Ok(d) => d.as_nanos() as i128,
            Err(e) => -(e.duration().as_nanos() as i128),
        };
        Ok(SrcKey {
            size: md.len(),
            mtime_ns: ns,
        })
    }
}

fn name_for(path: &Path, key: SrcKey) -> String {
    let mut h = DefaultHasher::new();
    path.hash(&mut h);
    key.hash(&mut h);
    format!("{:016x}", h.finish())
}

const IDX_MAGIC: &[u8; 8] = b"WMIDX001";

impl Cache {
    pub fn new(dir: PathBuf) -> Result<Cache> {
        std::fs::create_dir_all(dir.join("flacidx"))?;
        std::fs::create_dir_all(dir.join("images"))?;
        std::fs::create_dir_all(dir.join("md5"))?;
        Ok(Cache { dir })
    }

    fn idx_path(&self, src: &Path, key: SrcKey) -> PathBuf {
        self.dir
            .join("flacidx")
            .join(format!("{}.idx", name_for(src, key)))
    }

    pub fn load_index(&self, src: &Path, key: SrcKey) -> Option<FrameIndex> {
        let b = std::fs::read(self.idx_path(src, key)).ok()?;
        if b.len() < 24 || &b[..8] != IDX_MAGIC {
            return None;
        }
        let bs = u32::from_le_bytes(b[8..12].try_into().ok()?);
        let bps_in_header = b[12] != 0;
        let n = u64::from_le_bytes(b[16..24].try_into().ok()?) as usize;
        if b.len() != 24 + n * 8 {
            return None;
        }
        let offsets = b[24..]
            .chunks_exact(8)
            .map(|c| u64::from_le_bytes(c.try_into().unwrap()))
            .collect();
        Some(FrameIndex {
            block_size: bs,
            offsets,
            bps_in_header,
        })
    }

    pub fn store_index(&self, src: &Path, key: SrcKey, idx: &FrameIndex) -> Result<()> {
        let mut b = Vec::with_capacity(24 + idx.offsets.len() * 8);
        b.extend_from_slice(IDX_MAGIC);
        b.extend_from_slice(&idx.block_size.to_le_bytes());
        b.push(idx.bps_in_header as u8);
        b.extend_from_slice(&[0, 0, 0]);
        b.extend_from_slice(&(idx.offsets.len() as u64).to_le_bytes());
        for o in &idx.offsets {
            b.extend_from_slice(&o.to_le_bytes());
        }
        atomic_write(&self.idx_path(src, key), &b)
    }

    /// True if everything needed to present this image is already cached
    /// (index, and for non-FLAC sources the verified conversion), so it can be
    /// opened inline in milliseconds instead of queued.
    pub fn image_is_cached(&self, src: &Path) -> bool {
        let Ok(key) = SrcKey::of(src) else {
            return false;
        };
        let is_flac = src
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("flac"));
        let flac = if is_flac {
            src.to_path_buf()
        } else {
            self.converted_path(src, key)
        };
        let Ok(fkey) = SrcKey::of(&flac) else {
            return false;
        };
        self.idx_path(&flac, fkey).exists()
    }

    /// Index for a FLAC file, from cache or built (and cached) now.
    pub fn index_for(&self, src: &Path, meta: &FlacMeta) -> Result<FrameIndex> {
        let key = SrcKey::of(src)?;
        if let Some(i) = self.load_index(src, key) {
            return Ok(i);
        }
        let idx = FrameIndex::build(src, meta)?;
        // the file must not have changed while we read it (e.g. still downloading)
        ensure!(
            SrcKey::of(src)? == key,
            "{} changed while indexing",
            src.display()
        );
        self.store_index(src, key, &idx)?;
        Ok(idx)
    }

    fn md5_path(&self, flac: &Path, key: SrcKey) -> PathBuf {
        self.dir
            .join("md5")
            .join(format!("{}.txt", name_for(flac, key)))
    }

    /// Cached per-range audio MD5s of a FLAC image: (start, end) -> md5
    pub fn load_md5s(&self, flac: &Path) -> std::collections::HashMap<(u64, u64), [u8; 16]> {
        let mut m = std::collections::HashMap::new();
        let Ok(key) = SrcKey::of(flac) else { return m };
        let Ok(text) = std::fs::read_to_string(self.md5_path(flac, key)) else {
            return m;
        };
        for line in text.lines() {
            let mut it = line.split_whitespace();
            let (Some(a), Some(b), Some(h)) = (it.next(), it.next(), it.next()) else {
                continue;
            };
            let (Ok(a), Ok(b)) = (a.parse(), b.parse()) else {
                continue;
            };
            if h.len() == 32 {
                let mut d = [0u8; 16];
                if (0..16).all(|i| {
                    u8::from_str_radix(&h[2 * i..2 * i + 2], 16)
                        .map(|v| d[i] = v)
                        .is_ok()
                }) {
                    m.insert((a, b), d);
                }
            }
        }
        m
    }

    pub fn store_md5s(&self, flac: &Path, entries: &[((u64, u64), [u8; 16])]) -> Result<()> {
        let key = SrcKey::of(flac)?;
        let mut m = self.load_md5s(flac);
        for (r, d) in entries {
            m.insert(*r, *d);
        }
        let mut lines: Vec<String> = m
            .iter()
            .map(|((a, b), d)| {
                format!(
                    "{a} {b} {}",
                    d.iter().map(|x| format!("{x:02x}")).collect::<String>()
                )
            })
            .collect();
        lines.sort();
        atomic_write(
            &self.md5_path(flac, key),
            (lines.join("\n") + "\n").as_bytes(),
        )
    }

    pub fn converted_path(&self, src: &Path, key: SrcKey) -> PathBuf {
        self.dir
            .join("images")
            .join(format!("{}.flac", name_for(src, key)))
    }

    /// Losslessly convert a non-FLAC image (APE, WavPack, TTA, TAK, WAV, ALAC...)
    /// to FLAC in the cache, verifying the decoded PCM MD5 against the source.
    /// Returns the cached FLAC path.
    pub fn convert_image(&self, src: &Path) -> Result<PathBuf> {
        let key = SrcKey::of(src)?;
        let out = self.converted_path(src, key);
        if out.exists() {
            return Ok(out);
        }
        let probe = Command::new("ffprobe")
            .args([
                "-v",
                "error",
                "-select_streams",
                "a:0",
                "-show_entries",
                "stream=sample_fmt,bits_per_raw_sample,bits_per_sample,channels,sample_rate",
                "-of",
                "default=nw=1",
            ])
            .arg(src)
            .output()?;
        ensure!(
            probe.status.success(),
            "ffprobe failed: {}",
            String::from_utf8_lossy(&probe.stderr)
        );
        let info = String::from_utf8_lossy(&probe.stdout).to_string();
        let field = |k: &str| {
            info.lines()
                .find_map(|l| l.strip_prefix(&format!("{k}=")))
                .unwrap_or("")
                .to_string()
        };
        let mut bits: u32 = field("bits_per_raw_sample").parse().unwrap_or(0);
        if bits == 0 {
            bits = field("bits_per_sample").parse().unwrap_or(0);
        }
        if bits == 0 {
            bits = match field("sample_fmt").as_str() {
                "u8" | "u8p" => 8,
                "s16" | "s16p" => 16,
                _ => 24,
            };
        }
        let (codec, fmt) = match bits {
            1..=8 => ("pcm_u8", "u8"),
            9..=16 => ("pcm_s16le", "s16le"),
            _ => ("pcm_s24le", "s24le"),
        };
        // unique per process+thread, and ending in .flac because the flac CLI wants that
        let tmp = PathBuf::from(format!("{}.flac", unique_tmp(&out).display()));
        // decode with ffmpeg, encode with the reference encoder
        let status = Command::new("sh")
            .arg("-c")
            .arg("ffmpeg -v error -i \"$1\" -map 0:a:0 -c:a $2 -f wav - | flac -s -f --ignore-chunk-sizes --compression-level-5 --blocksize=4096 -o \"$3\" -")
            .arg("sh")
            .arg(src)
            .arg(codec)
            .arg(&tmp)
            .status()?;
        if !status.success() {
            let _ = std::fs::remove_file(&tmp);
            bail!("conversion of {} failed", src.display());
        }
        // verify: MD5 of the source PCM (ffmpeg) == STREAMINFO MD5 written by flac
        let md5 = Command::new("ffmpeg")
            .args(["-v", "error", "-i"])
            .arg(src)
            .args(["-map", "0:a:0", "-c:a", codec, "-f", "md5", "-"])
            .output()?;
        let src_md5 = String::from_utf8_lossy(&md5.stdout)
            .trim()
            .trim_start_matches("MD5=")
            .to_string();
        let meta = FlacMeta::read(&tmp)?;
        let got: String = meta
            .streaminfo
            .md5
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        let _ = fmt;
        if src_md5 != got {
            let _ = std::fs::remove_file(&tmp);
            bail!(
                "PCM MD5 mismatch after converting {} ({src_md5} vs {got})",
                src.display()
            );
        }
        ensure!(
            SrcKey::of(src)? == key,
            "{} changed during conversion",
            src.display()
        );
        if out.exists() {
            // another process finished the same conversion first; keep theirs
            let _ = std::fs::remove_file(&tmp);
            return Ok(out);
        }
        std::fs::rename(&tmp, &out)?;
        // source tags (APEv2 etc.) for later use as album-level tags
        let tags = Command::new("ffprobe")
            .args(["-v", "error", "-show_entries", "format_tags", "-of", "json"])
            .arg(src)
            .output()?;
        std::fs::write(out.with_extension("json"), &tags.stdout)?;
        Ok(out)
    }

    /// Tags of the original (non-FLAC) image saved during conversion.
    pub fn converted_tags(&self, converted: &Path) -> Vec<(String, String)> {
        let Ok(b) = std::fs::read(converted.with_extension("json")) else {
            return Vec::new();
        };
        parse_ffprobe_tags(&b)
    }
}

fn parse_ffprobe_tags(b: &[u8]) -> Vec<(String, String)> {
    let Ok(v) = serde_json::from_slice::<serde_json::Value>(b) else {
        return Vec::new();
    };
    let Some(tags) = v.pointer("/format/tags").and_then(|t| t.as_object()) else {
        return Vec::new();
    };
    tags.iter()
        .filter_map(|(k, v)| v.as_str().map(|s| (k.to_ascii_uppercase(), s.to_string())))
        .collect()
}

/// A temp path next to `path` that is unique to this process and thread, so
/// concurrent writers (the mount service and a CLI run) never share a temp file.
pub fn unique_tmp(path: &Path) -> PathBuf {
    let tid = format!("{:?}", std::thread::current().id())
        .chars()
        .filter(|c| c.is_ascii_digit())
        .collect::<String>();
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(format!(".tmp.{}.{tid}", std::process::id()));
    path.with_file_name(name)
}

pub fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    let tmp = unique_tmp(path);
    std::fs::write(&tmp, data).with_context(|| format!("write {}", tmp.display()))?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffprobe_tags() {
        let j = br#"{
    "format": {
        "tags": {
            "Artist": "Neutral Milk Hotel",
            "Album": "In the \"Aeroplane\"",
            "Year": "1998"
        }
    }
}"#;
        let t = parse_ffprobe_tags(j);
        assert!(t.contains(&("ARTIST".into(), "Neutral Milk Hotel".into())));
        assert!(t.contains(&("ALBUM".into(), "In the \"Aeroplane\"".into())));
        assert!(t.contains(&("YEAR".into(), "1998".into())));
    }
}
