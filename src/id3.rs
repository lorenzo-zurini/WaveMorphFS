//! Minimal ID3v2.4 writer (UTF-8 text frames) for DSF output.

use crate::tags::Tags;

/// Tag name -> ID3v2.4 text frame id. Accepts Vorbis names (TITLE, ALBUMARTIST),
/// the names ffprobe reports for ID3/MP4 sources (title, album_artist, publisher)
/// and raw frame ids (TSRC). Case-insensitive. Anything else becomes a TXXX frame
/// whose description is the tag name exactly as written.
fn frame_for(key: &str) -> Option<String> {
    let k = key.to_ascii_uppercase();
    let id = match k.as_str() {
        "TITLE" => "TIT2",
        "ARTIST" => "TPE1",
        "ALBUMARTIST" | "ALBUM_ARTIST" | "ALBUM ARTIST" => "TPE2",
        "CONDUCTOR" => "TPE3",
        "REMIXER" | "MIXARTIST" => "TPE4",
        "ALBUM" => "TALB",
        "DATE" | "YEAR" => "TDRC",
        "ORIGINALDATE" | "ORIGINAL_DATE" => "TDOR",
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
        "ALBUMSORT" | "ALBUM_SORT" => "TSOA",
        "ARTISTSORT" | "ARTIST_SORT" => "TSOP",
        "TITLESORT" | "TITLE_SORT" => "TSOT",
        "ALBUMARTISTSORT" | "ALBUM_ARTIST_SORT" => "TSO2",
        "COMPOSERSORT" | "COMPOSER_SORT" => "TSOC",
        "MOVEMENTNAME" => "MVNM",
        "MEDIA" => "TMED",
        "LANGUAGE" => "TLAN",
        "DISCSUBTITLE" => "TSST",
        _ => {
            // a raw text-frame id such as TSRC or TMED (TXXX is handled separately)
            let raw = k.len() == 4 && k.starts_with('T') && k != "TXXX" && k.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit());
            return raw.then_some(k);
        }
    };
    Some(id.to_string())
}

const TRACK_NAMES: &[&str] = &["TRACKNUMBER", "TRACK", "TRCK"];
const TRACK_TOTALS: &[&str] = &["TRACKTOTAL", "TOTALTRACKS"];
const DISC_NAMES: &[&str] = &["DISCNUMBER", "DISC", "TPOS"];
const DISC_TOTALS: &[&str] = &["DISCTOTAL", "TOTALDISCS"];

fn synchsafe(n: u32) -> [u8; 4] {
    [((n >> 21) & 0x7F) as u8, ((n >> 14) & 0x7F) as u8, ((n >> 7) & 0x7F) as u8, (n & 0x7F) as u8]
}

fn text_frame(id: &str, values: &[String]) -> Vec<u8> {
    let mut body = vec![3u8]; // UTF-8
    body.extend_from_slice(values.join("\0").as_bytes());
    let mut f = id.as_bytes().to_vec();
    f.extend_from_slice(&synchsafe(body.len() as u32));
    f.extend_from_slice(&[0, 0]);
    f.extend_from_slice(&body);
    f
}

fn txxx_frame(desc: &str, values: &[String]) -> Vec<u8> {
    let mut body = vec![3u8];
    body.extend_from_slice(desc.as_bytes());
    body.push(0);
    body.extend_from_slice(values.join("\0").as_bytes());
    let mut f = b"TXXX".to_vec();
    f.extend_from_slice(&synchsafe(body.len() as u32));
    f.extend_from_slice(&[0, 0]);
    f.extend_from_slice(&body);
    f
}

/// Build a complete ID3v2.4 tag.
pub fn build(tags: &Tags) -> Vec<u8> {
    let first = |names: &[&str]| names.iter().find_map(|n| tags.get(n));
    let num = |a: Option<&str>, b: Option<&str>| match (a, b) {
        (Some(a), Some(b)) if !a.contains('/') => Some(format!("{a}/{b}")),
        (Some(a), _) => Some(a.to_string()),
        _ => None,
    };
    let mut frames = Vec::new();
    if let Some(t) = num(first(TRACK_NAMES), first(TRACK_TOTALS)) {
        frames.extend(text_frame("TRCK", &[t]));
    }
    if let Some(d) = num(first(DISC_NAMES), first(DISC_TOTALS)) {
        frames.extend(text_frame("TPOS", &[d]));
    }
    let numeric: Vec<&str> = TRACK_NAMES.iter().chain(TRACK_TOTALS).chain(DISC_NAMES).chain(DISC_TOTALS).copied().collect();
    let mut used: Vec<String> = Vec::new();
    for (k, vs) in &tags.0 {
        if vs.is_empty() || numeric.iter().any(|n| k.is(n)) {
            continue;
        }
        match frame_for(&k.0) {
            // two spellings mapping to the same frame: first one wins
            Some(id) if used.contains(&id) => {}
            Some(id) => {
                frames.extend(text_frame(&id, vs));
                used.push(id);
            }
            None => frames.extend(txxx_frame(&k.0, vs)),
        }
    }
    let mut out = b"ID3\x04\x00\x00".to_vec();
    out.extend_from_slice(&synchsafe(frames.len() as u32));
    out.extend(frames);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(s.contains("TIT2") && s.contains("Promenade") && s.contains("1/15") && s.contains("CATALOGNUMBER"));
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
        assert!(s.contains("TXXX") && s.contains("MusicBrainz Album Id"), "spelling kept in TXXX");
        assert!(s.contains("TSRC") && s.contains("USRC17607839"));
    }
}
