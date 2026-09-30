//! FLAC primitives: metadata parsing, frame header parsing/rewriting, CRCs,
//! a full-file frame indexer and a VERBATIM frame encoder.
//!
//! Splitting strategy (see `track.rs`): frames that lie entirely inside a track
//! are copied byte-for-byte, only their header is rewritten to a variable-blocksize
//! header carrying the track-relative sample number (plus new CRC-8/CRC-16). The
//! partial frames at track boundaries are decoded and re-emitted as VERBATIM
//! frames, so the output is sample-exact without re-encoding the track.

use anyhow::{Context, Result, bail, ensure};
use std::fs::File;
use std::io::Read;
use std::os::unix::fs::FileExt;
use std::path::Path;

// ---------------------------------------------------------------------------
// CRCs

const fn make_crc8() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u8;
        let mut j = 0;
        while j < 8 {
            c = if c & 0x80 != 0 { (c << 1) ^ 0x07 } else { c << 1 };
            j += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

const fn make_crc16() -> [u16; 256] {
    let mut t = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = (i as u16) << 8;
        let mut j = 0;
        while j < 8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x8005 } else { c << 1 };
            j += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

static CRC8: [u8; 256] = make_crc8();
static CRC16: [u16; 256] = make_crc16();

pub fn crc8(data: &[u8]) -> u8 {
    data.iter().fold(0u8, |c, &b| CRC8[(c ^ b) as usize])
}

pub fn crc16_update(mut crc: u16, data: &[u8]) -> u16 {
    for &b in data {
        crc = (crc << 8) ^ CRC16[((crc >> 8) as u8 ^ b) as usize];
    }
    crc
}

pub fn crc16(data: &[u8]) -> u16 {
    crc16_update(0, data)
}

// ---------------------------------------------------------------------------
// FLAC's extended UTF-8 number coding (frame/sample numbers, up to 36 bits)

pub fn coded_len(v: u64) -> usize {
    match v {
        0..0x80 => 1,
        0x80..0x800 => 2,
        0x800..0x1_0000 => 3,
        0x1_0000..0x20_0000 => 4,
        0x20_0000..0x400_0000 => 5,
        0x400_0000..0x8000_0000 => 6,
        _ => 7,
    }
}

pub fn encode_number(v: u64, out: &mut Vec<u8>) {
    let n = coded_len(v);
    if n == 1 {
        out.push(v as u8);
        return;
    }
    let lead: u8 = match n {
        2 => 0xC0,
        3 => 0xE0,
        4 => 0xF0,
        5 => 0xF8,
        6 => 0xFC,
        _ => 0xFE,
    };
    let rest = n - 1;
    let first_bits = if n == 7 { 0 } else { (v >> (6 * rest)) as u8 };
    out.push(lead | first_bits);
    for i in (0..rest).rev() {
        out.push(0x80 | ((v >> (6 * i)) & 0x3F) as u8);
    }
}

/// Decode a coded number; returns (value, byte length).
pub fn decode_number(b: &[u8]) -> Option<(u64, usize)> {
    let first = *b.first()?;
    let (n, mut v) = match first {
        0x00..=0x7F => return Some((first as u64, 1)),
        0xC0..=0xDF => (2, (first & 0x1F) as u64),
        0xE0..=0xEF => (3, (first & 0x0F) as u64),
        0xF0..=0xF7 => (4, (first & 0x07) as u64),
        0xF8..=0xFB => (5, (first & 0x03) as u64),
        0xFC..=0xFD => (6, (first & 0x01) as u64),
        0xFE => (7, 0u64),
        _ => return None,
    };
    if b.len() < n {
        return None;
    }
    for &c in &b[1..n] {
        if c & 0xC0 != 0x80 {
            return None;
        }
        v = (v << 6) | (c & 0x3F) as u64;
    }
    Some((v, n))
}

// ---------------------------------------------------------------------------
// Frame headers

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameHeader {
    pub variable: bool,
    pub bs_code: u8,
    pub sr_code: u8,
    pub ch_code: u8,
    pub bps_code: u8,
    /// frame number (fixed blocksize) or first sample number (variable)
    pub number: u64,
    pub number_len: usize,
    pub block_size: u32,
    /// bytes of explicit block-size / sample-rate fields following the number
    pub extra_len: usize,
    /// total header length including the CRC-8 byte
    pub len: usize,
}

impl FrameHeader {
    /// Parse and CRC-check a frame header at the start of `b`.
    pub fn parse(b: &[u8]) -> Option<FrameHeader> {
        if b.len() < 6 || b[0] != 0xFF || (b[1] & 0xFE) != 0xF8 {
            return None;
        }
        let variable = b[1] & 1 == 1;
        let bs_code = b[2] >> 4;
        let sr_code = b[2] & 0x0F;
        let ch_code = b[3] >> 4;
        let bps_code = (b[3] >> 1) & 0x07;
        if bs_code == 0 || sr_code == 0x0F || ch_code > 0x0A || bps_code == 3 || (b[3] & 1) != 0 {
            return None;
        }
        let (number, number_len) = decode_number(&b[4..])?;
        if !variable && number_len > 6 {
            return None;
        }
        let mut p = 4 + number_len;
        let block_size = match bs_code {
            1 => 192,
            2..=5 => 576u32 << (bs_code - 2),
            6 => {
                let v = *b.get(p)? as u32 + 1;
                p += 1;
                v
            }
            7 => {
                let v = u16::from_be_bytes([*b.get(p)?, *b.get(p + 1)?]) as u32 + 1;
                p += 2;
                v
            }
            _ => 256u32 << (bs_code - 8),
        };
        match sr_code {
            12 => p += 1,
            13 | 14 => p += 2,
            _ => {}
        }
        let crc = *b.get(p)?;
        if crc8(&b[..p]) != crc {
            return None;
        }
        Some(FrameHeader {
            variable,
            bs_code,
            sr_code,
            ch_code,
            bps_code,
            number,
            number_len,
            block_size,
            extra_len: p - 4 - number_len,
            len: p + 1,
        })
    }

}

/// Rewrite an existing header (`orig`, parsed as `h`) into a variable-blocksize
/// header carrying `sample_number`. Optionally force an explicit bps code.
pub fn rewrite_header(orig: &[u8], h: &FrameHeader, sample_number: u64, bps_code: Option<u8>) -> Vec<u8> {
    let mut out = Vec::with_capacity(h.len + 2);
    out.push(0xFF);
    out.push(0xF9);
    out.push(orig[2]);
    let b3 = match bps_code {
        Some(c) => (orig[3] & 0xF1) | (c << 1),
        None => orig[3],
    };
    out.push(b3);
    encode_number(sample_number, &mut out);
    let extra_start = 4 + h.number_len;
    out.extend_from_slice(&orig[extra_start..extra_start + h.extra_len]);
    out.push(crc8(&out));
    out
}

pub fn bps_code_for(bps: u32) -> u8 {
    match bps {
        8 => 1,
        12 => 2,
        16 => 4,
        20 => 5,
        24 => 6,
        32 => 7,
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// Metadata

#[derive(Debug, Clone, Copy, Default)]
pub struct StreamInfo {
    pub min_block: u16,
    pub max_block: u16,
    pub sample_rate: u32,
    pub channels: u32,
    pub bps: u32,
    pub total_samples: u64,
    pub md5: [u8; 16],
}

impl StreamInfo {
    pub fn parse(b: &[u8]) -> Result<StreamInfo> {
        ensure!(b.len() >= 34, "short STREAMINFO");
        let x = u64::from_be_bytes(b[10..18].try_into().unwrap());
        Ok(StreamInfo {
            min_block: u16::from_be_bytes([b[0], b[1]]),
            max_block: u16::from_be_bytes([b[2], b[3]]),
            sample_rate: (x >> 44) as u32,
            channels: ((x >> 41) & 0x7) as u32 + 1,
            bps: ((x >> 36) & 0x1F) as u32 + 1,
            total_samples: x & 0xF_FFFF_FFFF,
            md5: b[18..34].try_into().unwrap(),
        })
    }

    pub fn encode(&self, min_frame: u32, max_frame: u32) -> [u8; 34] {
        let mut b = [0u8; 34];
        b[0..2].copy_from_slice(&self.min_block.to_be_bytes());
        b[2..4].copy_from_slice(&self.max_block.to_be_bytes());
        b[4..7].copy_from_slice(&min_frame.to_be_bytes()[1..]);
        b[7..10].copy_from_slice(&max_frame.to_be_bytes()[1..]);
        let x = ((self.sample_rate as u64) << 44)
            | (((self.channels - 1) as u64) << 41)
            | (((self.bps - 1) as u64) << 36)
            | (self.total_samples & 0xF_FFFF_FFFF);
        b[10..18].copy_from_slice(&x.to_be_bytes());
        b[18..34].copy_from_slice(&self.md5);
        b
    }
}

pub const BLOCK_STREAMINFO: u8 = 0;
pub const BLOCK_PADDING: u8 = 1;
pub const BLOCK_VORBIS: u8 = 4;
pub const BLOCK_PICTURE: u8 = 6;

#[derive(Debug, Clone)]
pub struct MetaBlock {
    pub kind: u8,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct FlacMeta {
    pub streaminfo: StreamInfo,
    pub blocks: Vec<MetaBlock>,
    /// byte offset of the first audio frame
    pub audio_start: u64,
}

impl FlacMeta {
    pub fn read(path: &Path) -> Result<FlacMeta> {
        let mut f = File::open(path).with_context(|| format!("open {}", path.display()))?;
        let mut magic = [0u8; 4];
        f.read_exact(&mut magic)?;
        let mut pos = 4u64;
        if &magic == b"ID3\x03" || &magic[..3] == b"ID3" {
            // leading ID3v2 (non-standard but seen in the wild): skip it
            let mut rest = [0u8; 6];
            f.read_exact(&mut rest)?;
            let sz = rest[2..6].iter().fold(0u64, |a, &b| (a << 7) | (b & 0x7F) as u64);
            pos = 10 + sz;
            f.read_exact_at(&mut magic, pos)?;
            pos += 4;
        }
        ensure!(&magic == b"fLaC", "not a FLAC file");
        let mut blocks = Vec::new();
        let mut streaminfo = None;
        loop {
            let mut h = [0u8; 4];
            f.read_exact_at(&mut h, pos)?;
            let last = h[0] & 0x80 != 0;
            let kind = h[0] & 0x7F;
            let len = u32::from_be_bytes([0, h[1], h[2], h[3]]) as usize;
            let mut data = vec![0u8; len];
            f.read_exact_at(&mut data, pos + 4)?;
            pos += 4 + len as u64;
            if kind == BLOCK_STREAMINFO {
                streaminfo = Some(StreamInfo::parse(&data)?);
            } else if kind != BLOCK_PADDING {
                blocks.push(MetaBlock { kind, data });
            }
            if last {
                break;
            }
        }
        Ok(FlacMeta {
            streaminfo: streaminfo.context("no STREAMINFO")?,
            blocks,
            audio_start: pos,
        })
    }

    pub fn vorbis_comments(&self) -> Vec<(String, String)> {
        self.blocks
            .iter()
            .filter(|b| b.kind == BLOCK_VORBIS)
            .flat_map(|b| parse_vorbis(&b.data))
            .collect()
    }

    pub fn pictures(&self) -> Vec<&MetaBlock> {
        self.blocks.iter().filter(|b| b.kind == BLOCK_PICTURE).collect()
    }
}

pub fn parse_vorbis(b: &[u8]) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let rd = |p: usize| -> Option<usize> {
        b.get(p..p + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap()) as usize)
    };
    let Some(vlen) = rd(0) else { return out };
    let mut p = 4 + vlen;
    let Some(n) = rd(p) else { return out };
    p += 4;
    for _ in 0..n {
        let Some(l) = rd(p) else { break };
        p += 4;
        let Some(s) = b.get(p..p + l) else { break };
        p += l;
        let s = String::from_utf8_lossy(s);
        if let Some((k, v)) = s.split_once('=') {
            out.push((k.to_ascii_uppercase(), v.to_string()));
        }
    }
    out
}

pub fn build_vorbis(vendor: &str, tags: &[(String, String)]) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
    b.extend_from_slice(vendor.as_bytes());
    b.extend_from_slice(&(tags.len() as u32).to_le_bytes());
    for (k, v) in tags {
        let s = format!("{k}={v}");
        b.extend_from_slice(&(s.len() as u32).to_le_bytes());
        b.extend_from_slice(s.as_bytes());
    }
    b
}

/// Serialize a list of metadata blocks (STREAMINFO first) after the "fLaC" magic.
pub fn build_header(si: &[u8; 34], blocks: &[MetaBlock]) -> Vec<u8> {
    let mut out = b"fLaC".to_vec();
    let push = |out: &mut Vec<u8>, kind: u8, data: &[u8], last: bool| {
        out.push(kind | if last { 0x80 } else { 0 });
        out.extend_from_slice(&(data.len() as u32).to_be_bytes()[1..]);
        out.extend_from_slice(data);
    };
    push(&mut out, BLOCK_STREAMINFO, si, blocks.is_empty());
    for (i, b) in blocks.iter().enumerate() {
        push(&mut out, b.kind, &b.data, i + 1 == blocks.len());
    }
    out
}

// ---------------------------------------------------------------------------
// Whole-image frame index

/// Byte offsets of every frame of a fixed-blocksize FLAC image, built by a single
/// sequential pass that verifies each frame's header CRC-8, frame number and
/// CRC-16. A successful build therefore also proves the file is complete
/// (no zero-filled holes from an unfinished download).
#[derive(Debug, Clone)]
pub struct FrameIndex {
    pub block_size: u32,
    /// offsets[k] = start of frame k; offsets[nframes] = end of last frame
    pub offsets: Vec<u64>,
    pub bps_in_header: bool,
}

impl FrameIndex {
    pub fn build(path: &Path, meta: &FlacMeta) -> Result<FrameIndex> {
        let si = &meta.streaminfo;
        ensure!(si.total_samples > 0, "STREAMINFO has unknown total sample count");
        let f = File::open(path)?;
        let flen = f.metadata()?.len();
        let mut first = [0u8; 16];
        f.read_exact_at(&mut first, meta.audio_start)?;
        let h0 = FrameHeader::parse(&first).context("no valid frame at audio start")?;
        ensure!(!h0.variable, "variable-blocksize images are not supported");
        ensure!(h0.number == 0, "first frame number is not 0");
        let bs = h0.block_size;
        ensure!(si.min_block == si.max_block || si.max_block as u32 == bs, "inconsistent block size");
        let nframes = si.total_samples.div_ceil(bs as u64);

        let mut offsets = Vec::with_capacity(nframes as usize + 1);
        offsets.push(meta.audio_start);

        let mut w = Window { f: &f, flen, buf: Vec::new(), off: meta.audio_start, eof: false };
        w.fill()?;

        let mut cur = 0usize; // start of current frame within w.buf
        for k in 0..nframes {
            if w.buf.len() - cur < 64 && !w.eof {
                w.compact(&mut cur, None)?;
            }
            let hdr = FrameHeader::parse(&w.buf[cur..])
                .with_context(|| format!("frame {k} header invalid at byte {}", w.off + cur as u64))?;
            ensure!(hdr.number == k, "frame {k}: header says frame {}", hdr.number);
            let is_last = k + 1 == nframes;
            let mut crc = crc16_update(0, &w.buf[cur..cur + hdr.len]);
            let mut p = cur + hdr.len;
            let mut last_candidates: Vec<u64> = Vec::new();
            let end: Option<u64> = loop {
                if p >= w.buf.len() {
                    if w.eof {
                        break None;
                    }
                    w.compact(&mut cur, Some(&mut p))?;
                    continue;
                }
                crc = (crc << 8) ^ CRC16[((crc >> 8) as u8 ^ w.buf[p]) as usize];
                p += 1;
                // p - cur is invariant under compaction; need at least header + CRC-16
                if crc != 0 || p - cur < hdr.len + 2 {
                    continue;
                }
                if is_last {
                    // CRC-16 alone false-matches ~1/65536 bytes: collect every candidate
                    // up to EOF and pick the one followed only by a recognisable tag.
                    last_candidates.push(w.off + p as u64);
                    continue;
                }
                if p + 32 > w.buf.len() && !w.eof {
                    w.compact(&mut cur, Some(&mut p))?;
                }
                if let Some(nh) = FrameHeader::parse(&w.buf[p..]) {
                    // the final frame usually carries an explicit (smaller) block size
                    let next_is_last = k + 2 == nframes;
                    if nh.number == k + 1
                        && !nh.variable
                        && nh.sr_code == h0.sr_code
                        && nh.bps_code == h0.bps_code
                        && (nh.bs_code == h0.bs_code || next_is_last)
                    {
                        break Some(w.off + p as u64);
                    }
                }
            };
            let end = if is_last {
                last_candidates
                    .iter()
                    .rev()
                    .copied()
                    .find(|&e| trailing_is_tag(&f, e, flen))
                    .context("last frame: no valid end followed by EOF or a tag (incomplete file?)")?
            } else {
                end.with_context(|| format!("frame {k}: no valid end found (incomplete file?)"))?
            };
            offsets.push(end);
            cur = (end - w.off) as usize;
        }
        Ok(FrameIndex {
            block_size: bs,
            offsets,
            bps_in_header: h0.bps_code != 0,
        })
    }

    pub fn nframes(&self) -> u64 {
        self.offsets.len() as u64 - 1
    }
}

/// Sliding read window over a file used by the indexer.
struct Window<'a> {
    f: &'a File,
    flen: u64,
    buf: Vec<u8>,
    /// file offset of buf[0]
    off: u64,
    eof: bool,
}

impl Window<'_> {
    const CHUNK: usize = 4 << 20;

    fn fill(&mut self) -> Result<()> {
        let pos = self.off + self.buf.len() as u64;
        if pos >= self.flen {
            self.eof = true;
            return Ok(());
        }
        let n = Self::CHUNK.min((self.flen - pos) as usize);
        let old = self.buf.len();
        self.buf.resize(old + n, 0);
        self.f.read_exact_at(&mut self.buf[old..], pos)?;
        if pos + n as u64 >= self.flen {
            self.eof = true;
        }
        Ok(())
    }

    /// Drop everything before `cur`, rebase positions, read more.
    fn compact(&mut self, cur: &mut usize, p: Option<&mut usize>) -> Result<()> {
        let d = *cur;
        self.buf.drain(..d);
        self.off += d as u64;
        *cur = 0;
        if let Some(p) = p {
            *p -= d;
        }
        self.fill()
    }
}

/// True if the bytes from `end` to EOF are empty or a recognisable trailing tag.
fn trailing_is_tag(f: &File, end: u64, flen: u64) -> bool {
    let tail = flen - end;
    if tail == 0 {
        return true;
    }
    if tail > 1 << 20 {
        return false;
    }
    let mut head = [0u8; 8];
    let n = (tail as usize).min(8);
    if f.read_exact_at(&mut head[..n], end).is_err() {
        return false;
    }
    // No allowance for zero padding on purpose: an unfinished download also ends in zeros.
    (tail == 128 && &head[..3] == b"TAG") || head.starts_with(b"APETAGEX") || head.starts_with(b"ID3")
}

// ---------------------------------------------------------------------------
// VERBATIM frame encoder

pub struct BitWriter {
    pub buf: Vec<u8>,
    acc: u64,
    nbits: u32,
}

impl BitWriter {
    pub fn put(&mut self, value: u64, bits: u32) {
        debug_assert!(bits <= 32);
        self.acc = (self.acc << bits) | (value & ((1u64 << bits) - 1));
        self.nbits += bits;
        while self.nbits >= 8 {
            self.nbits -= 8;
            self.buf.push((self.acc >> self.nbits) as u8);
        }
        self.acc &= (1u64 << self.nbits) - 1;
    }
    pub fn align(&mut self) {
        if self.nbits > 0 {
            let pad = 8 - self.nbits;
            self.put(0, pad);
        }
    }
}

/// Size in bytes of a VERBATIM frame produced by `encode_verbatim`.
pub fn verbatim_size(samples: u32, channels: u32, bps: u32, sample_number: u64, sr_extra: usize) -> u64 {
    let bs_extra = if samples <= 256 { 1 } else { 2 };
    let header = 4 + coded_len(sample_number) + bs_extra + sr_extra + 1;
    let bits = channels as u64 * (8 + samples as u64 * bps as u64);
    header as u64 + bits.div_ceil(8) + 2
}

/// Encode interleaved-by-channel samples (`chans[c][i]`) as one VERBATIM frame
/// with a variable-blocksize header. `sr_bytes` = (sample-rate code, explicit bytes)
/// copied from the source frames so the header stays self-describing.
pub fn encode_verbatim(chans: &[&[i32]], bps: u32, sample_number: u64, sr_code: u8, sr_extra: &[u8]) -> Vec<u8> {
    let n = chans[0].len() as u32;
    assert!((1..=65535).contains(&n), "FLAC block size must be 1..=65535");
    let mut h = vec![0xFF, 0xF9];
    let bs_code = if n <= 256 { 6u8 } else { 7u8 };
    h.push((bs_code << 4) | sr_code);
    h.push((((chans.len() - 1) as u8) << 4) | (bps_code_for(bps) << 1));
    encode_number(sample_number, &mut h);
    if bs_code == 6 {
        h.push((n - 1) as u8);
    } else {
        h.extend_from_slice(&((n - 1) as u16).to_be_bytes());
    }
    h.extend_from_slice(sr_extra);
    h.push(crc8(&h));
    let mut w = BitWriter { buf: h, acc: 0, nbits: 0 };
    for ch in chans {
        w.put(0b0000_0010, 8); // zero pad bit, SUBFRAME_VERBATIM, no wasted bits
        for &s in ch.iter() {
            w.put(s as u32 as u64, bps);
        }
    }
    w.align();
    let crc = crc16(&w.buf);
    w.buf.extend_from_slice(&crc.to_be_bytes());
    w.buf
}

/// Decode one complete frame (bytes from its first header byte to its CRC-16).
/// Returns per-channel samples. Works around claxon's lack of STREAMINFO-bps
/// fallback by patching an explicit bps code into the header when needed.
pub fn decode_frame(frame: &[u8], bps: u32) -> Result<Vec<Vec<i32>>> {
    let h = FrameHeader::parse(frame).context("bad frame header")?;
    let owned;
    let data: &[u8] = if h.bps_code == 0 {
        let code = bps_code_for(bps);
        ensure!(code != 0, "unsupported bits per sample {bps}");
        let mut v = frame[..h.len].to_vec();
        v[3] = (v[3] & 0xF1) | (code << 1);
        let l = v.len();
        v[l - 1] = crc8(&v[..l - 1]);
        v.extend_from_slice(&frame[h.len..frame.len() - 2]);
        let c = crc16(&v);
        v.extend_from_slice(&c.to_be_bytes());
        owned = v;
        &owned
    } else {
        frame
    };
    let mut rd = claxon::frame::FrameReader::new(std::io::Cursor::new(data));
    let block = match rd.read_next_or_eof(Vec::new()) {
        Ok(Some(b)) => b,
        Ok(None) => bail!("empty frame"),
        Err(e) => bail!("decode error: {e}"),
    };
    Ok((0..block.channels()).map(|c| block.channel(c).to_vec()).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn number_roundtrip() {
        for &v in &[0u64, 1, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x1FFFFF, 0x200000, 0x3FFFFFF, 0x4000000, 0x7FFFFFFF, 0x80000000, 0xF_FFFF_FFFF] {
            let mut b = Vec::new();
            encode_number(v, &mut b);
            assert_eq!(b.len(), coded_len(v), "len {v:#x}");
            assert_eq!(decode_number(&b), Some((v, b.len())), "roundtrip {v:#x}");
        }
    }

    #[test]
    fn crc_known_values() {
        // CRC-16/BUYPASS ("123456789") = 0xFEE8, CRC-8/SMBUS = 0xF4
        assert_eq!(crc16(b"123456789"), 0xFEE8);
        assert_eq!(crc8(b"123456789"), 0xF4);
    }

    #[test]
    fn verbatim_roundtrip() {
        for &(bps, n) in &[(16u32, 1u32), (16, 17), (16, 300), (24, 4096), (8, 5), (24, 65535)] {
            let max = 1i64 << (bps - 1);
            let l: Vec<i32> = (0..n as i64).map(|i| ((i * 7919) % (2 * max) - max) as i32).collect();
            let r: Vec<i32> = l.iter().map(|s| (-(*s as i64) - 1) as i32).collect();
            let fr = encode_verbatim(&[&l, &r], bps, 123_456, 9, &[]);
            assert_eq!(fr.len() as u64, verbatim_size(n, 2, bps, 123_456, 0), "size bps={bps} n={n}");
            let h = FrameHeader::parse(&fr).unwrap();
            assert!(h.variable && h.number == 123_456 && h.block_size == n);
            assert_eq!(crc16(&fr), 0);
            let dec = decode_frame(&fr, bps).unwrap();
            assert_eq!(dec, vec![l, r]);
        }
    }
}
