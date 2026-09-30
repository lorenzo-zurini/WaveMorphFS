//! CUE sheet parsing with charset detection.
//!
//! Only what matters for splitting and tagging is kept: disc-level fields
//! (PERFORMER, TITLE, SONGWRITER, CATALOG, REM *), and per track the number,
//! INDEX 00/01 positions (in CD frames of 1/75 s) and track-level fields.

use anyhow::{Result, bail};
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Debug, Clone, Default)]
pub struct CueTrack {
    pub number: u32,
    /// INDEX 01 in CD frames (75 per second) relative to the start of the file
    pub index01: u64,
    pub index00: Option<u64>,
    /// TITLE / PERFORMER / SONGWRITER / ISRC and REM fields, upper-case keys
    pub fields: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Default)]
pub struct CueSheet {
    /// FILE entries in order (only single-FILE sheets are used for splitting)
    pub files: Vec<String>,
    pub fields: BTreeMap<String, String>,
    pub tracks: Vec<CueTrack>,
    pub encoding: &'static str,
}

impl CueSheet {
    pub fn read(path: &Path) -> Result<CueSheet> {
        let raw = std::fs::read(path)?;
        Self::parse_bytes(&raw)
    }

    pub fn parse_bytes(raw: &[u8]) -> Result<CueSheet> {
        let (text, encoding) = decode_text(raw);
        let mut sheet = Self::parse(&text)?;
        sheet.encoding = encoding;
        Ok(sheet)
    }

    pub fn parse(text: &str) -> Result<CueSheet> {
        let mut sheet = CueSheet::default();
        let mut cur: Option<CueTrack> = None;
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            let (cmd, rest) = split_word(line);
            let cmd = cmd.to_ascii_uppercase();
            match cmd.as_str() {
                "FILE" => {
                    // FILE "name with spaces.flac" WAVE  |  FILE name.flac WAVE
                    let name = if let Some(r) = rest.strip_prefix('"') {
                        r.rsplit_once('"').map(|(n, _)| n).unwrap_or(r)
                    } else {
                        rest.rsplit_once(char::is_whitespace).map(|(n, _)| n).unwrap_or(rest)
                    };
                    sheet.files.push(name.trim().to_string());
                }
                "TRACK" => {
                    if let Some(t) = cur.take() {
                        sheet.tracks.push(t);
                    }
                    let (num, kind) = split_word(rest);
                    // Only audio tracks matter (data tracks on enhanced CDs are skipped)
                    let number = num.parse().unwrap_or(sheet.tracks.len() as u32 + 1);
                    let mut t = CueTrack { number, ..Default::default() };
                    if !kind.eq_ignore_ascii_case("AUDIO") {
                        t.fields.insert("__NONAUDIO".into(), kind.to_string());
                    }
                    cur = Some(t);
                }
                "INDEX" => {
                    let (idx, pos) = split_word(rest);
                    let Some(t) = cur.as_mut() else { continue };
                    let frames = parse_msf(pos)?;
                    match idx.parse::<u32>().unwrap_or(99) {
                        0 => t.index00 = Some(frames),
                        1 => t.index01 = frames,
                        _ => {}
                    }
                }
                "REM" => {
                    let (k, v) = split_word(rest);
                    let k = k.to_ascii_uppercase();
                    if k.is_empty() {
                        continue;
                    }
                    let v = unquote(v);
                    match cur.as_mut() {
                        Some(t) => {
                            t.fields.insert(k, v);
                        }
                        None => {
                            sheet.fields.insert(k, v);
                        }
                    }
                }
                "TITLE" | "PERFORMER" | "SONGWRITER" | "ISRC" | "CATALOG" | "COMPOSER" | "ARRANGER" => {
                    let v = unquote(rest);
                    if v.is_empty() {
                        continue;
                    }
                    match cur.as_mut() {
                        Some(t) => {
                            t.fields.insert(cmd, v);
                        }
                        None => {
                            sheet.fields.insert(cmd, v);
                        }
                    }
                }
                _ => {}
            }
        }
        if let Some(t) = cur.take() {
            sheet.tracks.push(t);
        }
        sheet.tracks.retain(|t| !t.fields.contains_key("__NONAUDIO"));
        if sheet.tracks.is_empty() {
            bail!("no audio tracks");
        }
        for w in sheet.tracks.windows(2) {
            if w[1].index01 <= w[0].index01 {
                bail!("track {} INDEX 01 is not after track {}", w[1].number, w[0].number);
            }
        }
        Ok(sheet)
    }

    /// Single FILE with several tracks = an image rip that needs splitting.
    pub fn is_image(&self) -> bool {
        self.files.len() == 1 && self.tracks.len() > 1
    }
}

fn split_word(s: &str) -> (&str, &str) {
    let s = s.trim_start();
    match s.find(char::is_whitespace) {
        Some(i) => (&s[..i], s[i..].trim()),
        None => (s, ""),
    }
}

fn unquote(s: &str) -> String {
    let s = s.trim();
    let s = s.strip_prefix('"').and_then(|r| r.strip_suffix('"')).unwrap_or(s);
    s.trim().to_string()
}

/// "MM:SS:FF" -> CD frames (1/75 s)
pub fn parse_msf(s: &str) -> Result<u64> {
    let p: Vec<&str> = s.trim().split(':').collect();
    if p.len() != 3 {
        bail!("bad timestamp {s:?}");
    }
    let m: u64 = p[0].parse()?;
    let sec: u64 = p[1].parse()?;
    let f: u64 = p[2].parse()?;
    if sec >= 60 || f >= 75 {
        bail!("bad timestamp {s:?}");
    }
    Ok((m * 60 + sec) * 75 + f)
}

/// Decode a text file of unknown charset. UTF-8 (with or without BOM) and
/// UTF-16 BOMs are honoured; anything else goes through chardetng, which handles
/// the Windows-125x code pages common in cue sheets from Eastern European rips.
pub fn decode_text(raw: &[u8]) -> (String, &'static str) {
    if let Some((enc, bom_len)) = encoding_rs::Encoding::for_bom(raw) {
        let (s, _) = enc.decode_without_bom_handling(&raw[bom_len..]);
        return (s.into_owned(), enc.name());
    }
    if let Ok(s) = std::str::from_utf8(raw) {
        return (s.to_string(), "UTF-8");
    }
    let mut det = chardetng::EncodingDetector::new(chardetng::Iso2022JpDetection::Deny);
    det.feed(raw, true);
    let enc = det.guess(None, chardetng::Utf8Detection::Deny);
    let (s, _, _) = enc.decode(raw);
    (s.into_owned(), enc.name())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"REM GENRE Classical
REM DATE 1991
REM COMMENT "ExactAudioCopy v1.0"
PERFORMER "Bayerisches Staatsorchester, Wolfgang Sawallisch"
TITLE "Bruckner: Symphonie Nr. 5"
CATALOG 0012345678905
FILE "image.wav" WAVE
  TRACK 01 AUDIO
    TITLE "I. Introduktion"
    PERFORMER "BSO"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "II. Adagio"
    INDEX 00 20:11:40
    INDEX 01 20:13:02
  TRACK 03 AUDIO
    TITLE "III. Scherzo"
    SONGWRITER "Anton Bruckner"
    INDEX 01 38:00:74
"#;

    #[test]
    fn parses_sample() {
        let c = CueSheet::parse(SAMPLE).unwrap();
        assert!(c.is_image());
        assert_eq!(c.files, vec!["image.wav"]);
        assert_eq!(c.fields["GENRE"], "Classical");
        assert_eq!(c.fields["COMMENT"], "ExactAudioCopy v1.0");
        assert_eq!(c.fields["CATALOG"], "0012345678905");
        assert_eq!(c.tracks.len(), 3);
        assert_eq!(c.tracks[1].index00, Some((20 * 60 + 11) * 75 + 40));
        assert_eq!(c.tracks[1].index01, (20 * 60 + 13) * 75 + 2);
        assert_eq!(c.tracks[2].fields["SONGWRITER"], "Anton Bruckner");
        assert_eq!(c.tracks[0].fields["PERFORMER"], "BSO");
    }

    #[test]
    fn windows_1250_romanian() {
        // "Mugur de fluier" performer with Romanian diacritics in CP1250: ş=0xBA ţ=0xFE
        let mut raw = b"PERFORMER \"Phoenix \xBA\xFE\"\r\nFILE \"a.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 01:00:00\r\n".to_vec();
        raw.extend_from_slice(b"  TRACK 03 AUDIO\r\n    TITLE \"C\xE2ntec \xEEn \xBAir\"\r\n    INDEX 01 02:00:00\r\n");
        let c = CueSheet::parse_bytes(&raw).unwrap();
        assert!(c.encoding.starts_with("windows-125"), "{}", c.encoding);
        assert_eq!(c.tracks.len(), 3);
    }

    #[test]
    fn rejects_bad_order() {
        let t = "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 01:00:00\nTRACK 02 AUDIO\nINDEX 01 00:30:00\n";
        assert!(CueSheet::parse(t).is_err());
    }
}
