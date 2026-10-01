//! A regular FLAC file with its tag block replaced (source tags overlaid with
//! sidecar tags). Audio frames are served straight from the source file.

use crate::flac::{self, BLOCK_VORBIS, FlacMeta, MetaBlock};
use crate::tags::Tags;
use crate::vfile::{VFile, copy_overlap, read_full_at};
use anyhow::Result;
use std::fs::File;
use std::path::PathBuf;

pub struct RetagFlac {
    path: PathBuf,
    header: Vec<u8>,
    audio_start: u64,
    size: u64,
}

impl RetagFlac {
    pub fn new(path: PathBuf, meta: &FlacMeta, overlay: &Tags) -> Result<RetagFlac> {
        let mut tags = Tags::from_pairs(meta.vorbis_comments());
        tags.overlay(overlay);
        let mut blocks = vec![MetaBlock {
            kind: BLOCK_VORBIS,
            data: flac::build_vorbis("WaveMorphFS", &tags.to_pairs()),
        }];
        // keep every other block (SEEKTABLE offsets are relative to the first frame, so still valid)
        blocks.extend(
            meta.blocks
                .iter()
                .filter(|b| b.kind != BLOCK_VORBIS)
                .cloned(),
        );
        let si = meta.streaminfo;
        // min/max frame size fields are informational; keep them unknown rather than re-derive
        let header = flac::build_header(&si.encode(0, 0), &blocks);
        let flen = std::fs::metadata(&path)?.len();
        let size = header.len() as u64 + (flen - meta.audio_start);
        Ok(RetagFlac {
            path,
            header,
            audio_start: meta.audio_start,
            size,
        })
    }
}

impl VFile for RetagFlac {
    fn size(&self) -> u64 {
        self.size
    }

    fn read_at(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        let mut out = Vec::with_capacity(len);
        if off >= self.size {
            return Ok(out);
        }
        let len = len.min((self.size - off) as usize);
        copy_overlap(&mut out, &self.header, 0, off, len);
        let h = self.header.len() as u64;
        let end = off + len as u64;
        if end > h {
            let a = off.max(h);
            let mut buf = vec![0u8; (end - a) as usize];
            let f = File::open(&self.path)?;
            let n = read_full_at(&f, &mut buf, self.audio_start + (a - h))?;
            buf.truncate(n);
            out.extend_from_slice(&buf);
        }
        Ok(out)
    }

    fn describe(&self) -> String {
        format!("retagged-flac:{}", self.path.display())
    }
}
