//! A single track cut out of a FLAC image, presented as a standalone FLAC file.
//!
//! Output layout:
//!   [fLaC + STREAMINFO + VORBIS_COMMENT + PICTURE...]
//!   [head: VERBATIM frame for the partial source frame at the start]   (optional)
//!   [copied source frames, headers rewritten to track-relative sample numbers]
//!   [tail: VERBATIM frame for the partial source frame at the end]     (optional)
//!
//! Every size is computable from the frame index alone, so `size()` is exact
//! before any audio is decoded.

use crate::flac::{self, FlacMeta, FrameHeader, FrameIndex, MetaBlock, StreamInfo};
use crate::tags::Tags;
use crate::vfile::{VFile, copy_overlap, read_full_at};
use anyhow::{Context, Result, ensure};
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};

/// Blocks shorter than this are only legal as the last frame of a stream.
const MIN_BLOCK: u64 = 16;

pub struct FlacImage {
    pub path: PathBuf,
    pub meta: FlacMeta,
    pub index: FrameIndex,
    /// sample-rate code and explicit sample-rate bytes of the source frames
    pub sr_code: u8,
    pub sr_extra: Vec<u8>,
}

impl FlacImage {
    pub fn open(path: PathBuf, meta: FlacMeta, index: FrameIndex) -> Result<FlacImage> {
        let f = File::open(&path)?;
        let mut h = [0u8; 16];
        f.read_exact_at(&mut h, index.offsets[0])?;
        let hdr = FrameHeader::parse(&h).context("first frame header")?;
        let ex = 4 + hdr.number_len;
        let bs_extra = match hdr.bs_code {
            6 => 1,
            7 => 2,
            _ => 0,
        };
        let sr_extra = h[ex + bs_extra..ex + hdr.extra_len].to_vec();
        Ok(FlacImage { path, meta, index, sr_code: hdr.sr_code, sr_extra })
    }

    pub fn si(&self) -> &StreamInfo {
        &self.meta.streaminfo
    }

    fn bs(&self) -> u64 {
        self.index.block_size as u64
    }

    fn total(&self) -> u64 {
        self.si().total_samples
    }

    /// End sample (exclusive) of source frame k.
    fn frame_end(&self, k: u64) -> u64 {
        ((k + 1) * self.bs()).min(self.total())
    }

    /// Decode samples [s, e) (per channel) by decoding the covering source frames.
    pub fn decode_range(&self, s: u64, e: u64) -> Result<Vec<Vec<i32>>> {
        let bs = self.bs();
        let k0 = s / bs;
        let k1 = (e - 1) / bs;
        let a = self.index.offsets[k0 as usize];
        let b = self.index.offsets[k1 as usize + 1];
        let mut buf = vec![0u8; (b - a) as usize];
        let f = File::open(&self.path)?;
        ensure!(read_full_at(&f, &mut buf, a)? == buf.len(), "short read");
        let ch = self.si().channels as usize;
        let mut out: Vec<Vec<i32>> = vec![Vec::with_capacity((e - s) as usize); ch];
        for k in k0..=k1 {
            let fa = (self.index.offsets[k as usize] - a) as usize;
            let fb = (self.index.offsets[k as usize + 1] - a) as usize;
            let dec = flac::decode_frame(&buf[fa..fb], self.si().bps)?;
            let fs = k * bs;
            let lo = s.max(fs) - fs;
            let hi = e.min(self.frame_end(k)) - fs;
            for c in 0..ch {
                out[c].extend_from_slice(&dec[c][lo as usize..hi as usize]);
            }
        }
        Ok(out)
    }
}

/// MD5 of the decoded audio (FLAC STREAMINFO convention: interleaved, little-endian,
/// ceil(bps/8) bytes per sample) for each sample range, in one pass over the image.
pub fn track_md5s(img: &FlacImage, ranges: &[(u64, u64)]) -> Result<Vec<[u8; 16]>> {
    use md5::{Digest, Md5};
    let si = img.si();
    let bs = img.bs();
    let bytes = si.bps.div_ceil(8) as usize;
    let ch = si.channels as usize;
    let mut ctx: Vec<Md5> = ranges.iter().map(|_| Md5::new()).collect();
    let f = File::open(&img.path)?;
    let offs = &img.index.offsets;
    let nf = img.index.nframes();
    const WINDOW: u64 = 8 << 20;
    let mut k = 0u64;
    let mut pcm: Vec<u8> = Vec::new();
    while k < nf {
        // read a window of whole frames
        let a = offs[k as usize];
        let mut k_end = k + 1;
        while k_end < nf && offs[k_end as usize + 1] - a <= WINDOW {
            k_end += 1;
        }
        let b = offs[k_end as usize];
        let mut buf = vec![0u8; (b - a) as usize];
        ensure!(read_full_at(&f, &mut buf, a)? == buf.len(), "short read");
        for kk in k..k_end {
            let fa = (offs[kk as usize] - a) as usize;
            let fb = (offs[kk as usize + 1] - a) as usize;
            let dec = flac::decode_frame(&buf[fa..fb], si.bps)?;
            let fs = kk * bs;
            let fe = img.frame_end(kk);
            for (ri, &(s, e)) in ranges.iter().enumerate() {
                if s >= fe || e <= fs {
                    continue;
                }
                let lo = (s.max(fs) - fs) as usize;
                let hi = (e.min(fe) - fs) as usize;
                pcm.clear();
                pcm.reserve((hi - lo) * ch * bytes);
                for i in lo..hi {
                    for c in dec.iter().take(ch) {
                        pcm.extend_from_slice(&c[i].to_le_bytes()[..bytes]);
                    }
                }
                ctx[ri].update(&pcm);
            }
        }
        k = k_end;
    }
    Ok(ctx.into_iter().map(|c| c.finalize().into()).collect())
}

/// A partial frame re-emitted as VERBATIM.
#[derive(Debug, Clone, Copy)]
struct Seg {
    s: u64,
    e: u64,
    /// first sample number in the output stream
    out_sample: u64,
    size: u64,
}

pub struct FlacTrack {
    img: Arc<FlacImage>,
    pub start: u64,
    pub end: u64,
    header: Vec<u8>,
    head: Option<Seg>,
    /// copied source frames [k1, k2)
    k1: u64,
    k2: u64,
    copy_size: u64,
    tail: Option<Seg>,
    size: u64,
    head_bytes: OnceLock<Vec<u8>>,
    tail_bytes: OnceLock<Vec<u8>>,
    /// virtual start offset (relative to copy region) of each copied frame, + end
    vstarts: OnceLock<Vec<u64>>,
}

impl FlacTrack {
    /// Track covering samples [start, end) of `img`.
    pub fn new(img: Arc<FlacImage>, start: u64, end: u64, tags: &Tags, pictures: &[MetaBlock], md5: Option<[u8; 16]>) -> Result<FlacTrack> {
        let total = img.total();
        let bs = img.bs();
        let nf = img.index.nframes();
        ensure!(start < end && end <= total, "bad track range {start}..{end} (total {total})");

        let mut k1 = start.div_ceil(bs);
        let mut k2 = if end == total { nf } else { end / bs };
        let mut head = None;
        let mut tail = None;
        if k1 > k2 {
            // start and end inside the same source frame
            head = Some((start, end));
            k1 = k2;
        } else {
            if start % bs != 0 {
                head = Some((start, (k1 * bs).min(end)));
            }
            if end != total && end % bs != 0 {
                tail = Some((k2 * bs, end));
            }
            // Avoid tiny non-final blocks: merge them with the neighbouring full frame.
            if let Some((s, e)) = head
                && e - s < MIN_BLOCK
                && k1 < k2
            {
                head = Some((s, img.frame_end(k1)));
                k1 += 1;
            }
            if let Some((s, e)) = tail
                && e - s < MIN_BLOCK
                && k1 < k2
            {
                tail = Some(((k2 - 1) * bs, e));
                let _ = s;
                k2 -= 1;
            }
        }

        let si = img.si();
        let seg = |(s, e): (u64, u64)| {
            let out_sample = s - start;
            Seg { s, e, out_sample, size: flac::verbatim_size((e - s) as u32, si.channels, si.bps, out_sample, img.sr_extra.len()) }
        };
        let head = head.map(seg);
        let tail = tail.map(seg);

        let mut copy_size = 0u64;
        if k1 < k2 {
            copy_size = img.index.offsets[k2 as usize] - img.index.offsets[k1 as usize];
            for k in k1..k2 {
                let new_len = flac::coded_len(k * bs - start) as i64;
                let old_len = flac::coded_len(k) as i64;
                copy_size = (copy_size as i64 + new_len - old_len) as u64;
            }
        }

        // STREAMINFO for the track
        let mut blocks: Vec<u64> = Vec::new();
        if let Some(h) = head {
            blocks.push(h.e - h.s);
        }
        for k in k1..k2 {
            blocks.push(img.frame_end(k) - k * bs);
        }
        if let Some(t) = tail {
            blocks.push(t.e - t.s);
        }
        let non_last = &blocks[..blocks.len().saturating_sub(1).max(if blocks.len() == 1 { 1 } else { 0 })];
        let min_b = non_last.iter().copied().min().unwrap_or(blocks[0]);
        let max_b = blocks.iter().copied().max().unwrap();
        let tsi = StreamInfo {
            min_block: min_b as u16,
            max_block: max_b as u16,
            sample_rate: si.sample_rate,
            channels: si.channels,
            bps: si.bps,
            total_samples: end - start,
            md5: md5.unwrap_or([0; 16]), // all-zero = unknown until the background job has computed it
        };
        let mut meta = vec![MetaBlock { kind: flac::BLOCK_VORBIS, data: flac::build_vorbis("WaveMorphFS", &tags.to_pairs()) }];
        meta.extend(pictures.iter().cloned());
        let header = flac::build_header(&tsi.encode(0, 0), &meta);

        let size = header.len() as u64 + head.map_or(0, |h| h.size) + copy_size + tail.map_or(0, |t| t.size);
        Ok(FlacTrack {
            img,
            start,
            end,
            header,
            head,
            k1,
            k2,
            copy_size,
            tail,
            size,
            head_bytes: OnceLock::new(),
            tail_bytes: OnceLock::new(),
            vstarts: OnceLock::new(),
        })
    }

    fn verbatim(&self, seg: &Seg) -> Result<Vec<u8>> {
        let chans = self.img.decode_range(seg.s, seg.e)?;
        let refs: Vec<&[i32]> = chans.iter().map(|c| c.as_slice()).collect();
        let fr = flac::encode_verbatim(&refs, self.img.si().bps, seg.out_sample, self.img.sr_code, &self.img.sr_extra);
        ensure!(fr.len() as u64 == seg.size, "verbatim size mismatch {} != {}", fr.len(), seg.size);
        Ok(fr)
    }

    fn seg_bytes<'a>(&'a self, cell: &'a OnceLock<Vec<u8>>, seg: &Seg) -> Result<&'a [u8]> {
        if let Some(v) = cell.get() {
            return Ok(v);
        }
        let v = self.verbatim(seg)?;
        Ok(cell.get_or_init(|| v))
    }

    fn vstarts(&self) -> &[u64] {
        self.vstarts.get_or_init(|| {
            let bs = self.img.bs();
            let offs = &self.img.index.offsets;
            let mut v = Vec::with_capacity((self.k2 - self.k1 + 1) as usize);
            let mut pos = 0u64;
            for k in self.k1..self.k2 {
                v.push(pos);
                let orig = offs[k as usize + 1] - offs[k as usize];
                let delta = flac::coded_len(k * bs - self.start) as i64 - flac::coded_len(k) as i64;
                pos = (pos as i64 + orig as i64 + delta) as u64;
            }
            v.push(pos);
            debug_assert_eq!(pos, self.copy_size);
            v
        })
    }

    /// Bytes [off, off+len) of the copy region (offsets relative to its start).
    fn read_copy(&self, off: u64, len: usize, out: &mut Vec<u8>) -> Result<()> {
        let vs = self.vstarts();
        let end = off + len as u64;
        // first frame i with vs[i] <= off < vs[i+1]
        let i0 = vs.partition_point(|&v| v <= off) - 1;
        let mut i1 = i0;
        while i1 + 1 < vs.len() && vs[i1] < end {
            i1 += 1;
        }
        // frames i0..i1 (exclusive) overlap the request
        let offs = &self.img.index.offsets;
        let (k_first, k_last) = (self.k1 + i0 as u64, self.k1 + i1 as u64);
        let a = offs[k_first as usize];
        let b = offs[k_last as usize];
        let mut raw = vec![0u8; (b - a) as usize];
        let f = File::open(&self.img.path)?;
        ensure!(read_full_at(&f, &mut raw, a)? == raw.len(), "short read in {}", self.img.path.display());
        let bs = self.img.bs();
        for (j, k) in (k_first..k_last).enumerate() {
            let fa = (offs[k as usize] - a) as usize;
            let fb = (offs[k as usize + 1] - a) as usize;
            let orig = &raw[fa..fb];
            let h = FrameHeader::parse(orig).with_context(|| format!("frame {k} header"))?;
            let mut frame = flac::rewrite_header(orig, &h, k * bs - self.start, None);
            frame.extend_from_slice(&orig[h.len..orig.len() - 2]);
            let crc = flac::crc16(&frame);
            frame.extend_from_slice(&crc.to_be_bytes());
            copy_overlap(out, &frame, vs[i0 + j], off, len);
        }
        Ok(())
    }
}

impl VFile for FlacTrack {
    fn size(&self) -> u64 {
        self.size
    }

    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        let mut out = Vec::with_capacity(len);
        if off >= self.size {
            return Ok(out);
        }
        let len = len.min((self.size - off) as usize);
        let end = off + len as u64;
        let mut pos = 0u64;
        // header
        copy_overlap(&mut out, &self.header, pos, off, len);
        pos += self.header.len() as u64;
        if let Some(h) = &self.head {
            if off < pos + h.size && end > pos {
                let b = self.seg_bytes(&self.head_bytes, h)?;
                copy_overlap(&mut out, b, pos, off, len);
            }
            pos += h.size;
        }
        if self.copy_size > 0 {
            if off < pos + self.copy_size && end > pos {
                let a = off.max(pos) - pos;
                let b = end.min(pos + self.copy_size) - pos;
                self.read_copy(a, (b - a) as usize, &mut out)?;
            }
            pos += self.copy_size;
        }
        if let Some(t) = &self.tail
            && off < pos + t.size
            && end > pos
        {
            let b = self.seg_bytes(&self.tail_bytes, t)?;
            copy_overlap(&mut out, b, pos, off, len);
        }
        ensure!(out.len() == len, "internal: produced {} of {} bytes at {off}", out.len(), len);
        Ok(out)
    }

    fn describe(&self) -> String {
        format!("flac-image:{}#samples={}..{}", self.img.path.display(), self.start, self.end)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    /// Deterministic, somewhat compressible test signal.
    fn signal(n: usize, ch: usize, bps: u32) -> Vec<Vec<i32>> {
        let amp = ((1i64 << (bps - 1)) - 1) as f64 * 0.6;
        let mut seed = 12345u64;
        (0..ch)
            .map(|c| {
                (0..n)
                    .map(|i| {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                        let noise = ((seed >> 33) as f64 / (1u64 << 31) as f64 - 0.5) * amp * 0.1;
                        let t = i as f64 / 44100.0;
                        (amp * 0.8 * (t * 440.0 * (c as f64 + 1.0) * std::f64::consts::TAU).sin() + noise) as i32
                    })
                    .collect()
            })
            .collect()
    }

    fn write_raw(path: &std::path::Path, chans: &[Vec<i32>], bps: u32) {
        let bytes = (bps / 8) as usize;
        let mut raw = Vec::new();
        for i in 0..chans[0].len() {
            for c in chans {
                raw.extend_from_slice(&c[i].to_le_bytes()[..bytes]);
            }
        }
        std::fs::write(path, raw).unwrap();
    }

    fn decode_raw(path: &std::path::Path) -> Vec<u8> {
        let o = Command::new("flac")
            .args(["-d", "-c", "-s", "--force-raw-format", "--endian=little", "--sign=signed"])
            .arg(path)
            .output()
            .unwrap();
        assert!(o.status.success(), "flac -d failed: {}", String::from_utf8_lossy(&o.stderr));
        o.stdout
    }

    fn run_case(rate: u32, ch: usize, bps: u32, bs: u32, total: usize, ranges: &[(u64, u64)]) {
        let dir = tempfile::tempdir().unwrap();
        let raw = dir.path().join("in.raw");
        let img = dir.path().join("img.flac");
        let sig = signal(total, ch, bps);
        write_raw(&raw, &sig, bps);
        let st = Command::new("flac")
            .args(["-s", "-f", "--force-raw-format", "--endian=little", "--sign=signed"])
            .arg(format!("--channels={ch}"))
            .arg(format!("--bps={bps}"))
            .arg(format!("--sample-rate={rate}"))
            .arg(format!("--blocksize={bs}"))
            .arg("-o")
            .arg(&img)
            .arg(&raw)
            .status()
            .unwrap();
        assert!(st.success());
        let meta = FlacMeta::read(&img).unwrap();
        let idx = FrameIndex::build(&img, &meta).unwrap();
        assert_eq!(idx.block_size, bs);
        let image = Arc::new(FlacImage::open(img.clone(), meta, idx).unwrap());
        let frame_bytes = (bps / 8) as usize * ch;
        let all = std::fs::read(&raw).unwrap();
        for &(s, e) in ranges {
            let mut tags = Tags::new();
            tags.set("TITLE", format!("{s}-{e}"));
            let md5 = track_md5s(&image, &[(s, e)]).unwrap()[0];
            let tr = FlacTrack::new(image.clone(), s, e, &tags, &[], Some(md5)).unwrap();
            let bytes = tr.read_at(0, tr.size() as usize).unwrap();
            assert_eq!(bytes.len() as u64, tr.size());
            let out = dir.path().join("t.flac");
            std::fs::write(&out, &bytes).unwrap();
            let t = Command::new("flac").args(["-t", "-s"]).arg(&out).output().unwrap();
            assert!(t.status.success(), "flac -t failed for {rate}/{ch}ch/{bps}bit/bs{bs} range {s}..{e}: {}", String::from_utf8_lossy(&t.stderr));
            let got = decode_raw(&out);
            let want = &all[s as usize * frame_bytes..e as usize * frame_bytes];
            assert!(got == want, "PCM mismatch for {rate}/{ch}ch/{bps}bit/bs{bs} range {s}..{e}");
            eprintln!("  verified {rate}Hz {ch}ch {bps}bit bs{bs} range {s}..{e} ({} bytes)", bytes.len());
        }
    }

    fn edge_ranges(bs: u64, total: u64) -> Vec<(u64, u64)> {
        vec![
            (0, total),                     // whole image
            (0, bs),                        // exactly one frame
            (bs, 3 * bs),                   // aligned both ends
            (1, bs + 1),                    // off by one
            (bs - 1, 3 * bs + 1),           // 1-sample head (merged), 1-sample tail (merged)
            (bs - 15, 3 * bs + 15),         // 15-sample head/tail (merged)
            (bs - 16, 3 * bs + 16),         // 16-sample head/tail (not merged)
            (bs + 7, bs + 12),              // 5 samples inside one frame
            (bs - 3, bs + 3),               // 6 samples straddling a boundary
            (2 * bs + 100, total),          // to the end (tiny last frame)
            (total - 5, total),             // last 5 samples only
            (total - bs - 2, total),        // tail merge near the end
        ]
    }

    #[test]
    fn cd_audio_4096() {
        let total = 4096 * 20 + 3; // tiny last frame
        run_case(44100, 2, 16, 4096, total, &edge_ranges(4096, total as u64));
    }

    #[test]
    fn odd_blocksize_24bit() {
        let total = 1152 * 30 + 1000;
        run_case(48000, 2, 24, 1152, total, &edge_ranges(1152, total as u64));
    }

    #[test]
    fn mono_hires_4608() {
        let total = 4608 * 12 + 17;
        run_case(96000, 1, 24, 4608, total, &edge_ranges(4608, total as u64));
    }

    #[test]
    fn multichannel_8bit() {
        let total = 576 * 40 + 9;
        run_case(22050, 6, 8, 576, total, &edge_ranges(576, total as u64));
    }
}
