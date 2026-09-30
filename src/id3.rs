//! Minimal ID3v2.4 writer (UTF-8 text frames) for DSF output.

use crate::tags::Tags;

/// Vorbis key -> ID3v2.4 text frame id.
fn frame_for(key: &str) -> Option<&'static str> {
    Some(match key {
        "TITLE" => "TIT2",
        "ARTIST" => "TPE1",
        "ALBUMARTIST" => "TPE2",
        "CONDUCTOR" => "TPE3",
        "ALBUM" => "TALB",
        "DATE" | "YEAR" => "TDRC",
        "ORIGINALDATE" => "TDOR",
        "GENRE" => "TCON",
        "COMPOSER" => "TCOM",
        "LYRICIST" => "TEXT",
        "LABEL" | "PUBLISHER" | "ORGANIZATION" => "TPUB",
        "COPYRIGHT" => "TCOP",
        "ISRC" => "TSRC",
        "WORK" => "TIT1",
        "SUBTITLE" => "TIT3",
        "BPM" => "TBPM",
        "MOOD" => "TMOO",
        "ENCODEDBY" => "TENC",
        "ALBUMSORT" => "TSOA",
        "ARTISTSORT" => "TSOP",
        "TITLESORT" => "TSOT",
        "ALBUMARTISTSORT" => "TSO2",
        "MOVEMENTNAME" => "MVNM",
        "MEDIA" => "TMED",
        _ => return None,
    })
}

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
    let mut frames = Vec::new();
    let num = |a: Option<&str>, b: Option<&str>| match (a, b) {
        (Some(a), Some(b)) => Some(format!("{}/{}", a.split('/').next().unwrap_or(a), b)),
        (Some(a), None) => Some(a.to_string()),
        _ => None,
    };
    if let Some(t) = num(tags.get("TRACKNUMBER"), tags.get("TRACKTOTAL").or(tags.get("TOTALTRACKS"))) {
        frames.extend(text_frame("TRCK", &[t]));
    }
    if let Some(d) = num(tags.get("DISCNUMBER"), tags.get("DISCTOTAL").or(tags.get("TOTALDISCS"))) {
        frames.extend(text_frame("TPOS", &[d]));
    }
    for (k, vs) in &tags.0 {
        if vs.is_empty() || matches!(k.as_str(), "TRACKNUMBER" | "TRACKTOTAL" | "TOTALTRACKS" | "DISCNUMBER" | "DISCTOTAL" | "TOTALDISCS") {
            continue;
        }
        match frame_for(k) {
            Some(id) => frames.extend(text_frame(id, vs)),
            None => frames.extend(txxx_frame(k, vs)),
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
}
