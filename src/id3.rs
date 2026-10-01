//! ID3v2.4 writer: builds the tag for DSF output and retags MP3 files.
//!
//! Retagging keeps every frame of the source tag that the overlay does not name
//! (pictures, chapters, private frames...) byte for byte; only the frames for the
//! overlay's tag names are replaced. Tag names are never rewritten: a name that is
//! not a known text frame becomes a TXXX frame described by the name exactly as
//! written.

use crate::tags::Tags;
use anyhow::{Result, bail};
use std::fs::File;
use std::io::Read;
use std::path::Path;

/// Where a tag name goes in ID3v2.4.
#[derive(Debug, Clone, PartialEq)]
enum Target {
    Text(String),
    Txxx(String),
    Comment,
    Lyrics(String),
    Track,
    Disc,
}

const TRACK_NAMES: &[&str] = &["TRACKNUMBER", "TRACK", "TRCK"];
const TRACK_TOTALS: &[&str] = &["TRACKTOTAL", "TOTALTRACKS"];
const DISC_NAMES: &[&str] = &["DISCNUMBER", "DISC", "TPOS"];
const DISC_TOTALS: &[&str] = &["DISCTOTAL", "TOTALDISCS"];

/// Tag name -> ID3v2.4 frame. Accepts Vorbis names (TITLE, ALBUMARTIST), the names
/// ffprobe reports for ID3 sources (album_artist, publisher, album-sort, lyrics-eng)
/// and raw text-frame ids (TSRC). Case-insensitive.
fn target_for(key: &str) -> Target {
    let k = key.to_ascii_uppercase();
    if TRACK_NAMES.contains(&k.as_str()) || TRACK_TOTALS.contains(&k.as_str()) {
        return Target::Track;
    }
    if DISC_NAMES.contains(&k.as_str()) || DISC_TOTALS.contains(&k.as_str()) {
        return Target::Disc;
    }
    if k == "COMMENT" {
        return Target::Comment;
    }
    if k == "LYRICS" || k == "UNSYNCEDLYRICS" || k.starts_with("LYRICS-") {
        // ffprobe: "lyrics-eng" / "lyrics-<description>-eng"
        let lang = k
            .rsplit('-')
            .next()
            .filter(|l| k.contains('-') && l.len() == 3)
            .unwrap_or("XXX");
        return Target::Lyrics(lang.to_ascii_lowercase());
    }
    let id = match k.as_str() {
        "TITLE" => "TIT2",
        "ARTIST" => "TPE1",
        "ALBUMARTIST" | "ALBUM_ARTIST" | "ALBUM ARTIST" => "TPE2",
        "CONDUCTOR" => "TPE3",
        "REMIXER" | "MIXARTIST" => "TPE4",
        "ALBUM" => "TALB",
        "DATE" | "YEAR" => "TDRC",
        "ORIGINALDATE" | "ORIGINAL_DATE" => "TDOR",
        "RELEASEDATE" => "TDRL",
        "GENRE" => "TCON",
        "COMPOSER" => "TCOM",
        "LYRICIST" => "TEXT",
        "LABEL" | "PUBLISHER" | "ORGANIZATION" => "TPUB",
        "COPYRIGHT" => "TCOP",
        "ISRC" => "TSRC",
        "WORK" | "GROUPING" => "TIT1",
        "SUBTITLE" => "TIT3",
        "BPM" => "TBPM",
        "MOOD" => "TMOO",
        "ENCODEDBY" | "ENCODED_BY" => "TENC",
        "ENCODER" | "ENCODERSETTINGS" => "TSSE",
        "COMPILATION" => "TCMP",
        "ALBUMSORT" | "ALBUM_SORT" | "ALBUM-SORT" => "TSOA",
        "ARTISTSORT" | "ARTIST_SORT" | "ARTIST-SORT" => "TSOP",
        "TITLESORT" | "TITLE_SORT" | "TITLE-SORT" => "TSOT",
        "ALBUMARTISTSORT" | "ALBUM_ARTIST_SORT" | "ALBUM_ARTIST-SORT" | "ALBUM-ARTIST-SORT" => {
            "TSO2"
        }
        "COMPOSERSORT" | "COMPOSER_SORT" | "COMPOSER-SORT" => "TSOC",
        "MOVEMENTNAME" => "MVNM",
        "MEDIA" => "TMED",
        "LANGUAGE" => "TLAN",
        "DISCSUBTITLE" => "TSST",
        "CREATION_TIME" => "TDEN",
        _ => {
            // a raw text-frame id such as TSRC or TMED
            let raw = k.len() == 4
                && k.starts_with('T')
                && k != "TXXX"
                && k.bytes()
                    .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit());
            return if raw {
                Target::Text(k)
            } else {
                Target::Txxx(key.to_string())
            };
        }
    };
    Target::Text(id.to_string())
}

/// One frame of a tag, stored as it will be written (ID3v2.4 layout).
#[derive(Debug, Clone)]
struct Frame {
    id: [u8; 4],
    flags: [u8; 2],
    data: Vec<u8>,
}

impl Frame {
    fn new(id: &str, data: Vec<u8>) -> Frame {
        Frame {
            id: id.as_bytes().try_into().expect("4-char frame id"),
            flags: [0, 0],
            data,
        }
    }

    fn id(&self) -> &str {
        std::str::from_utf8(&self.id).unwrap_or("")
    }

    /// Frame body without a data length indicator; None if compressed/encrypted.
    fn body(&self) -> Option<&[u8]> {
        if self.flags[1] & 0x0C != 0 {
            return None;
        }
        if self.flags[1] & 0x01 != 0 {
            self.data.get(4..)
        } else {
            Some(&self.data)
        }
    }

    /// Description of a TXXX/COMM/USLT frame.
    fn description(&self) -> Option<String> {
        let b = self.body()?;
        let enc = *b.first()?;
        let rest = if self.id() == "TXXX" {
            b.get(1..)?
        } else {
            b.get(4..)?
        };
        Some(decode_text(enc, split_terminated(enc, rest).0))
    }

    fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.id);
        out.extend_from_slice(&synchsafe(self.data.len() as u32));
        out.extend_from_slice(&self.flags);
        out.extend_from_slice(&self.data);
    }
}

fn synchsafe(n: u32) -> [u8; 4] {
    [
        ((n >> 21) & 0x7F) as u8,
        ((n >> 14) & 0x7F) as u8,
        ((n >> 7) & 0x7F) as u8,
        (n & 0x7F) as u8,
    ]
}

fn unsynchsafe(b: &[u8]) -> u32 {
    b.iter()
        .take(4)
        .fold(0u32, |a, &x| (a << 7) | (x & 0x7F) as u32)
}

fn deunsync(b: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        out.push(b[i]);
        if b[i] == 0xFF && b.get(i + 1) == Some(&0) {
            i += 1;
        }
        i += 1;
    }
    out
}

/// Split at the encoding's string terminator.
fn split_terminated(enc: u8, b: &[u8]) -> (&[u8], &[u8]) {
    if enc == 1 || enc == 2 {
        let mut i = 0;
        while i + 1 < b.len() {
            if b[i] == 0 && b[i + 1] == 0 {
                return (&b[..i], &b[i + 2..]);
            }
            i += 2;
        }
        (b, &[])
    } else {
        match b.iter().position(|&x| x == 0) {
            Some(i) => (&b[..i], &b[i + 1..]),
            None => (b, &[]),
        }
    }
}

fn decode_text(enc: u8, b: &[u8]) -> String {
    match enc {
        0 => b.iter().map(|&c| c as char).collect(),
        1 | 2 => {
            let mut be = enc == 2;
            let mut b = b;
            if b.len() >= 2 && (b[..2] == [0xFF, 0xFE] || b[..2] == [0xFE, 0xFF]) {
                be = b[0] == 0xFE;
                b = &b[2..];
            }
            let units: Vec<u16> = b
                .chunks_exact(2)
                .map(|c| {
                    if be {
                        u16::from_be_bytes([c[0], c[1]])
                    } else {
                        u16::from_le_bytes([c[0], c[1]])
                    }
                })
                .collect();
            String::from_utf16_lossy(&units)
        }
        _ => String::from_utf8_lossy(b).into_owned(),
    }
}

fn text_frame(id: &str, values: &[String]) -> Frame {
    let mut body = vec![3u8]; // UTF-8
    body.extend_from_slice(values.join("\0").as_bytes());
    Frame::new(id, body)
}

fn txxx_frame(desc: &str, values: &[String]) -> Frame {
    let mut body = vec![3u8];
    body.extend_from_slice(desc.as_bytes());
    body.push(0);
    body.extend_from_slice(values.join("\0").as_bytes());
    Frame::new("TXXX", body)
}

/// COMM / USLT: encoding, language, empty description, text.
fn lang_frame(id: &str, lang: &str, text: &str) -> Frame {
    let mut body = vec![3u8];
    let l = lang.as_bytes();
    body.extend_from_slice(&[
        *l.first().unwrap_or(&b'X'),
        *l.get(1).unwrap_or(&b'X'),
        *l.get(2).unwrap_or(&b'X'),
    ]);
    body.push(0);
    body.extend_from_slice(text.as_bytes());
    Frame::new(id, body)
}

/// A source tag read from the start of a file.
struct SourceTag {
    /// tag-wide flags to keep (v2.4 unsynchronisation)
    flags: u8,
    frames: Vec<Frame>,
}

/// Parse an ID3v2.3/2.4 tag. Returns the tag and its total length in the file.
fn parse_tag(b: &[u8]) -> Result<(SourceTag, usize)> {
    let major = b[3];
    let hflags = b[5];
    let size = unsynchsafe(&b[6..10]) as usize;
    let total = 10
        + size
        + if major == 4 && hflags & 0x10 != 0 {
            10
        } else {
            0
        };
    if !(major == 3 || major == 4) {
        bail!("ID3v2.{major} tag");
    }
    if b.len() < 10 + size {
        bail!("truncated ID3 tag");
    }
    let mut body = b[10..10 + size].to_vec();
    if major == 3 && hflags & 0x80 != 0 {
        body = deunsync(&body);
    }
    let mut pos = 0;
    if hflags & 0x40 != 0 {
        pos = if major == 3 {
            4 + u32::from_be_bytes(body[0..4].try_into()?) as usize
        } else {
            unsynchsafe(&body[0..4]) as usize
        };
    }
    let mut frames = Vec::new();
    while pos + 10 <= body.len() && body[pos] != 0 {
        let id: [u8; 4] = body[pos..pos + 4].try_into()?;
        let fs = if major == 4 {
            unsynchsafe(&body[pos + 4..pos + 8])
        } else {
            u32::from_be_bytes(body[pos + 4..pos + 8].try_into()?)
        } as usize;
        let flags = [body[pos + 8], body[pos + 9]];
        let data = body
            .get(pos + 10..pos + 10 + fs)
            .ok_or_else(|| anyhow::anyhow!("frame overruns tag"))?
            .to_vec();
        pos += 10 + fs;
        if !id
            .iter()
            .all(|c| c.is_ascii_uppercase() || c.is_ascii_digit())
        {
            break;
        }
        if major == 4 {
            frames.push(Frame { id, flags, data });
            continue;
        }
        // ID3v2.3: compressed/encrypted/grouped frames have a different layout in 2.4
        if flags[1] & 0xE0 != 0 {
            continue;
        }
        frames.push(Frame {
            id,
            flags: [0, 0],
            data,
        });
    }
    if major == 3 {
        frames = upgrade_v23(frames);
    }
    Ok((
        SourceTag {
            flags: if major == 4 { hflags & 0x80 } else { 0 },
            frames,
        },
        total,
    ))
}

/// Replace the ID3v2.3-only date frames with their 2.4 counterparts.
fn upgrade_v23(frames: Vec<Frame>) -> Vec<Frame> {
    let text = |id: &str| {
        frames.iter().find(|f| f.id() == id).and_then(|f| {
            f.data
                .split_first()
                .map(|(&e, t)| decode_text(e, t).trim_end_matches('\0').trim().to_string())
        })
    };
    let year = text("TYER");
    let ddmm = text("TDAT");
    let tory = text("TORY");
    let mut out: Vec<Frame> = frames
        .iter()
        .filter(|f| !matches!(f.id(), "TYER" | "TDAT" | "TIME" | "TRDA" | "TORY" | "TSIZ"))
        .cloned()
        .collect();
    if let Some(y) = year.filter(|y| !y.is_empty()) {
        let date = match ddmm.filter(|d| d.len() == 4 && d.bytes().all(|c| c.is_ascii_digit())) {
            Some(d) => format!("{y}-{}-{}", &d[2..4], &d[0..2]),
            None => y,
        };
        if !out.iter().any(|f| f.id() == "TDRC") {
            out.push(text_frame("TDRC", &[date]));
        }
    }
    if let Some(y) = tory.filter(|y| !y.is_empty()) {
        if !out.iter().any(|f| f.id() == "TDOR") {
            out.push(text_frame("TDOR", &[y]));
        }
    }
    out
}

/// Apply `overlay` to `frames`: every frame a named tag maps to is replaced.
fn apply(frames: &mut Vec<Frame>, overlay: &Tags) {
    let get = |names: &[&str]| -> Option<&str> { names.iter().find_map(|n| overlay.get(n)) };
    let named = |names: &[&str]| names.iter().any(|n| overlay.get_all(n).is_some());
    let num = |a: Option<&str>, b: Option<&str>| match (a, b) {
        (Some(a), Some(b)) if !a.contains('/') => Some(format!("{a}/{b}")),
        (Some(a), _) => Some(a.to_string()),
        _ => None,
    };
    // track / disc combine two names into one frame
    for (id, nums, totals) in [
        ("TRCK", TRACK_NAMES, TRACK_TOTALS),
        ("TPOS", DISC_NAMES, DISC_TOTALS),
    ] {
        if !named(nums) && !named(totals) {
            continue;
        }
        let old_n = frames.iter().find(|f| f.id() == id).and_then(|f| {
            f.body()
                .and_then(|b| b.split_first().map(|(&e, t)| decode_text(e, t)))
        });
        let n = get(nums).map(str::to_string).or_else(|| {
            if named(nums) {
                None
            } else {
                old_n.map(|s| {
                    s.split('/')
                        .next()
                        .unwrap_or("")
                        .trim_end_matches('\0')
                        .to_string()
                })
            }
        });
        frames.retain(|f| f.id() != id);
        if let Some(v) = num(n.as_deref(), get(totals)) {
            frames.push(text_frame(id, &[v]));
        }
    }
    let mut used: Vec<Target> = Vec::new();
    for (k, vs) in &overlay.0 {
        let t = target_for(&k.0);
        if matches!(t, Target::Track | Target::Disc) {
            continue;
        }
        // two spellings mapping to the same frame: the first one wins
        if used.contains(&t) {
            continue;
        }
        let same = |f: &Frame| match &t {
            Target::Text(id) => {
                f.id() == id
                    || (id == "TDRC" && matches!(f.id(), "TYER" | "TDAT" | "TIME" | "TRDA"))
            }
            Target::Txxx(d) => {
                f.id() == "TXXX" && f.description().is_some_and(|x| x.eq_ignore_ascii_case(d))
            }
            Target::Comment => f.id() == "COMM" && f.description().is_some_and(|x| x.is_empty()),
            Target::Lyrics(_) => f.id() == "USLT",
            Target::Track | Target::Disc => false,
        };
        frames.retain(|f| !same(f));
        if !vs.is_empty() {
            match &t {
                Target::Text(id) => frames.push(text_frame(id, vs)),
                Target::Txxx(d) => frames.push(txxx_frame(d, vs)),
                Target::Comment => frames.push(lang_frame("COMM", "eng", &vs.join("\n"))),
                Target::Lyrics(l) => frames.push(lang_frame("USLT", l, &vs.join("\n"))),
                Target::Track | Target::Disc => {}
            }
        }
        used.push(t);
    }
}

fn serialize(flags: u8, frames: &[Frame]) -> Vec<u8> {
    let mut body = Vec::new();
    for f in frames {
        f.write(&mut body);
    }
    let mut out = vec![b'I', b'D', b'3', 4, 0, flags];
    out.extend_from_slice(&synchsafe(body.len() as u32));
    out.extend(body);
    out
}

/// Build a complete ID3v2.4 tag from scratch (DSF output).
pub fn build(tags: &Tags) -> Vec<u8> {
    let mut frames = Vec::new();
    apply(&mut frames, tags);
    serialize(0, &frames)
}

/// Retag an MP3: returns the new tag and the offset where the source's audio
/// (everything after its ID3v2 tag) starts.
pub fn retag_mp3(path: &Path, overlay: &Tags) -> Result<(Vec<u8>, u64)> {
    let mut f = File::open(path)?;
    let mut head = [0u8; 10];
    let n = f.read(&mut head)?;
    let (mut src, start) = if n == 10 && &head[..3] == b"ID3" {
        let total = 10 + unsynchsafe(&head[6..10]) as usize;
        let mut b = head.to_vec();
        b.resize(total, 0);
        f.read_exact(&mut b[10..])?;
        let (tag, len) = parse_tag(&b)?;
        (tag, len as u64)
    } else {
        (
            SourceTag {
                flags: 0,
                frames: Vec::new(),
            },
            0,
        )
    };
    apply(&mut src.frames, overlay);
    Ok((serialize(src.flags, &src.frames), start))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frames_of(tag: &[u8]) -> Vec<Frame> {
        parse_tag(tag).unwrap().0.frames
    }

    #[test]
    fn builds_valid_header() {
        let mut t = Tags::new();
        t.set("TITLE", "Promenade");
        t.set("TRACKNUMBER", "1");
        t.set("TRACKTOTAL", "15");
        t.set("CATALOGNUMBER", "CLSC2201 SA");
        let b = build(&t);
        assert_eq!(&b[..5], b"ID3\x04\x00");
        let size = b[6..10].iter().fold(0u32, |a, &x| (a << 7) | x as u32) as usize;
        assert_eq!(size + 10, b.len());
        let s = String::from_utf8_lossy(&b);
        assert!(
            s.contains("TIT2")
                && s.contains("Promenade")
                && s.contains("1/15")
                && s.contains("CATALOGNUMBER")
        );
    }

    #[test]
    fn ffprobe_and_picard_names() {
        let t = Tags::from_pairs([
            ("album_artist".into(), "AA".into()),
            ("track".into(), "3/12".into()),
            ("MusicBrainz Album Id".into(), "mbid".into()),
            ("TSRC".into(), "USRC17607839".into()),
        ]);
        let s = String::from_utf8_lossy(&build(&t)).to_string();
        assert!(s.contains("TPE2") && s.contains("AA"));
        assert!(s.contains("TRCK") && s.contains("3/12"));
        assert!(
            s.contains("TXXX") && s.contains("MusicBrainz Album Id"),
            "spelling kept in TXXX"
        );
        assert!(s.contains("TSRC") && s.contains("USRC17607839"));
    }

    #[test]
    fn retag_keeps_unnamed_frames_and_replaces_named() {
        // a v2.3 tag: TIT2 (UTF-16), TYER, APIC, TXXX "MusicBrainz Album Id", TRCK
        let mut body = Vec::new();
        let mut v23 = |id: &str, data: &[u8]| {
            body.extend_from_slice(id.as_bytes());
            body.extend_from_slice(&(data.len() as u32).to_be_bytes());
            body.extend_from_slice(&[0, 0]);
            body.extend_from_slice(data);
        };
        v23("TIT2", &[1, 0xFF, 0xFE, b'O', 0, b'l', 0, b'd', 0]);
        v23("TYER", b"\x001999");
        v23("APIC", b"\x00image/jpeg\x00\x03\x00JPEGDATA");
        v23("TXXX", b"\x00MusicBrainz Album Id\x00old");
        v23("TRCK", b"\x004/10");
        let mut tag = vec![b'I', b'D', b'3', 3, 0, 0];
        tag.extend_from_slice(&synchsafe(body.len() as u32 + 16));
        tag.extend(body);
        tag.extend([0u8; 16]); // padding
        let (mut src, len) = parse_tag(&tag).unwrap();
        assert_eq!(len, tag.len());
        let ov = Tags::from_pairs([
            ("title".into(), "New".into()),
            ("MUSICBRAINZ ALBUM ID".into(), "new".into()),
            ("TRACKTOTAL".into(), "12".into()),
        ]);
        apply(&mut src.frames, &ov);
        let out = serialize(src.flags, &src.frames);
        let fr = frames_of(&out);
        let ids: Vec<&str> = fr.iter().map(|f| f.id()).collect();
        assert!(ids.contains(&"APIC"), "picture kept");
        assert!(
            !ids.contains(&"TYER") && ids.contains(&"TDRC"),
            "v2.3 date upgraded"
        );
        assert_eq!(fr.iter().filter(|f| f.id() == "TIT2").count(), 1);
        let s = String::from_utf8_lossy(&out);
        assert!(s.contains("New") && !s.contains("O\0l\0d"));
        assert!(
            s.contains("MusicBrainz Album Id") == false
                && s.contains("MUSICBRAINZ ALBUM ID")
                && s.contains("new"),
            "TXXX replaced case-insensitively"
        );
        assert!(s.contains("4/12"), "track number kept, total set");
        assert!(s.contains("1999"));
    }
}
