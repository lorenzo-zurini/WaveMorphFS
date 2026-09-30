mod cue;
mod flac;
mod tags;
mod track;
mod vfile;

use anyhow::{Context, Result};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Arc;
use vfile::VFile;

fn md5_cmd(args: &[&str]) -> Result<String> {
    let out = std::process::Command::new("sh").arg("-c").arg(format!("{} | md5sum", args.join(" "))).output()?;
    Ok(String::from_utf8_lossy(&out.stdout).split_whitespace().next().unwrap_or("").to_string())
}

fn q(p: &std::path::Path) -> String { format!("'{}'", p.display().to_string().replace('\'', "'\\''")) }

fn main() -> Result<()> {
    let cue_path = PathBuf::from(std::env::args().nth(1).context("usage: split-test <cue>")?);
    let dir = cue_path.parent().unwrap().to_path_buf();
    let cue = cue::CueSheet::read(&cue_path)?;
    let img_path = dir.join(&cue.files[0]);
    println!("cue: {} tracks, encoding {}, image {}", cue.tracks.len(), cue.encoding, img_path.display());
    let meta = flac::FlacMeta::read(&img_path)?;
    let t = std::time::Instant::now();
    let idx = flac::FrameIndex::build(&img_path, &meta)?;
    println!("indexed {} frames in {:?}", idx.nframes(), t.elapsed());
    let img = Arc::new(track::FlacImage::open(img_path.clone(), meta, idx)?);
    let spf = img.si().sample_rate as u64 / 75;
    let total = img.si().total_samples;
    let tmp = PathBuf::from("/tmp/claude-1000/wm-test"); std::fs::create_dir_all(&tmp)?;
    let mut ok = 0;
    for (i, t) in cue.tracks.iter().enumerate() {
        let s = t.index01 * spf;
        let e = cue.tracks.get(i + 1).map(|n| n.index01 * spf).unwrap_or(total);
        let mut tags = tags::Tags::new();
        tags.set("TITLE", t.fields.get("TITLE").cloned().unwrap_or_default());
        tags.set("TRACKNUMBER", t.number.to_string());
        let tr = track::FlacTrack::new(img.clone(), s, e, &tags, &[])?;
        // sequential read in 128 KiB chunks (like the kernel would)
        let mut full = Vec::with_capacity(tr.size() as usize);
        let mut off = 0u64;
        while off < tr.size() { let b = tr.read_at(off, 131072)?; off += b.len() as u64; full.extend(b); }
        assert_eq!(full.len() as u64, tr.size());
        // random-access consistency
        let mut seed = 0x9E3779B97F4A7C15u64 ^ i as u64;
        for _ in 0..200 {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            let o = seed % tr.size(); let l = 1 + ((seed >> 20) % 70000) as usize;
            let b = tr.read_at(o, l)?; let l2 = l.min((tr.size() - o) as usize);
            assert_eq!(&b[..], &full[o as usize..o as usize + l2], "random read mismatch at {o}+{l}");
        }
        let out = tmp.join(format!("{:02}.flac", t.number));
        std::fs::File::create(&out)?.write_all(&full)?;
        let test = std::process::Command::new("flac").args(["-t", "-s"]).arg(&out).output()?;
        let raw = "--force-raw-format --endian=little --sign=signed -d -c -s";
        let ref_md5 = md5_cmd(&["flac", raw, &format!("--skip={s} --until={e}"), &q(&img_path)])?;
        let got_md5 = md5_cmd(&["flac", raw, &q(&out)])?;
        let pass = test.status.success() && ref_md5 == got_md5;
        if pass { ok += 1; }
        println!("  track {:2}: samples {:>10}..{:<10} size {:>10}  flac -t {}  pcm {}",
            t.number, s, e, tr.size(), if test.status.success() {"OK "} else {"FAIL"},
            if ref_md5 == got_md5 {"IDENTICAL".to_string()} else {format!("DIFF {ref_md5} vs {got_md5}")});
        if !test.status.success() { println!("{}", String::from_utf8_lossy(&test.stderr)); }
        std::fs::remove_file(&out)?;
    }
    println!("{ok}/{} tracks pass", cue.tracks.len());
    Ok(())
}
