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
