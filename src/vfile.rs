//! Virtual file abstraction: every entry the filesystem exposes is a `VFile`
//! with an exact size known up front and random-access reads.

use anyhow::Result;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::PathBuf;

pub trait VFile: Send + Sync {
    fn size(&self) -> u64;
    /// Read up to `len` bytes at `off`; shorter only at EOF.
    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>>;
    /// Human-readable description of where the bytes come from (for xattrs / debugging).
    fn describe(&self) -> String;
}

/// A source file exposed unchanged.
pub struct Passthrough {
    pub path: PathBuf,
    pub size: u64,
}

impl VFile for Passthrough {
    fn size(&self) -> u64 {
        self.size
    }
    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        if off >= self.size {
            return Ok(Vec::new());
        }
        let len = len.min((self.size - off) as usize);
        let mut buf = vec![0u8; len];
        let f = File::open(&self.path)?;
        let n = read_full_at(&f, &mut buf, off)?;
        buf.truncate(n);
        Ok(buf)
    }
    fn describe(&self) -> String {
        format!("passthrough:{}", self.path.display())
    }
}

/// pread until the buffer is full or EOF.
pub fn read_full_at(f: &File, buf: &mut [u8], mut off: u64) -> std::io::Result<usize> {
    let mut done = 0;
    while done < buf.len() {
        match f.read_at(&mut buf[done..], off) {
            Ok(0) => break,
            Ok(n) => {
                done += n;
                off += n as u64;
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(done)
}

/// Copy the part of `seg` (a segment starting at virtual offset `seg_start`) that
/// overlaps the request [off, off+len) into `out`.
pub fn copy_overlap(out: &mut Vec<u8>, seg: &[u8], seg_start: u64, off: u64, len: usize) {
    let seg_end = seg_start + seg.len() as u64;
    let req_end = off + len as u64;
    let a = off.max(seg_start);
    let b = req_end.min(seg_end);
    if a < b {
        out.extend_from_slice(&seg[(a - seg_start) as usize..(b - seg_start) as usize]);
    }
}

/// A piece of a `Spliced` file.
pub enum Seg {
    Mem(Vec<u8>),
    /// (offset, length) in the source file
    Src(u64, u64),
}

/// A source file with some byte ranges replaced (retagged MP3 / M4A).
pub struct Spliced {
    pub path: PathBuf,
    /// segments with their virtual start offsets
    segs: Vec<(u64, Seg)>,
    size: u64,
    what: &'static str,
}

impl Spliced {
    pub fn new(path: PathBuf, what: &'static str, segs: Vec<Seg>) -> Spliced {
        let mut at = 0;
        let segs: Vec<(u64, Seg)> = segs
            .into_iter()
            .map(|s| {
                let start = at;
                at += match &s {
                    Seg::Mem(b) => b.len() as u64,
                    Seg::Src(_, l) => *l,
                };
                (start, s)
            })
            .collect();
        Spliced {
            path,
            segs,
            size: at,
            what,
        }
    }
}

impl VFile for Spliced {
    fn size(&self) -> u64 {
        self.size
    }

    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        let mut out = Vec::with_capacity(len);
        if off >= self.size {
            return Ok(out);
        }
        let end = off + len.min((self.size - off) as usize) as u64;
        let mut file = None;
        for (start, seg) in &self.segs {
            match seg {
                Seg::Mem(b) => copy_overlap(&mut out, b, *start, off, (end - off) as usize),
                Seg::Src(so, sl) => {
                    let a = off.max(*start);
                    let b = end.min(start + sl);
                    if a < b {
                        if file.is_none() {
                            file = Some(File::open(&self.path)?);
                        }
                        let mut buf = vec![0u8; (b - a) as usize];
                        let n = read_full_at(file.as_ref().unwrap(), &mut buf, so + (a - start))?;
                        buf.truncate(n);
                        out.extend_from_slice(&buf);
                    }
                }
            }
        }
        Ok(out)
    }

    fn describe(&self) -> String {
        format!("{}:{}", self.what, self.path.display())
    }
}
