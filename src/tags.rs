//! Tag model: upper-case Vorbis-style keys with multiple values, merged from
//! several sources in increasing priority (file < cue/SACD text < sidecar).

use std::collections::BTreeMap;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Tags(pub BTreeMap<String, Vec<String>>);

/// Keys that describe a single track; they must not leak from an image file's
/// own tags (which describe the whole disc) into the split tracks.
pub const TRACK_SPECIFIC: &[&str] = &[
    "TITLE", "TRACKNUMBER", "TRACKTOTAL", "TOTALTRACKS", "ISRC", "CUESHEET", "LYRICS", "UNSYNCEDLYRICS",
    "MUSICBRAINZ_TRACKID", "MUSICBRAINZ_RELEASETRACKID", "MUSICBRAINZ_WORKID", "ACOUSTID_ID", "ACOUSTID_FINGERPRINT",
    "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "LENGTH", "PART", "MOVEMENTNAME", "MOVEMENT", "WORK",
];

impl Tags {
    pub fn new() -> Self {
        Tags::default()
    }

    pub fn from_pairs<I: IntoIterator<Item = (String, String)>>(pairs: I) -> Self {
        let mut t = Tags::new();
        for (k, v) in pairs {
            t.add(&k, v);
        }
        t
    }

    pub fn add(&mut self, key: &str, value: impl Into<String>) {
        let v = value.into();
        if v.trim().is_empty() {
            return;
        }
        self.0.entry(key.to_ascii_uppercase()).or_default().push(v);
    }

    /// Replace all values of `key`.
    pub fn set(&mut self, key: &str, value: impl Into<String>) {
        let v = value.into();
        let k = key.to_ascii_uppercase();
        if v.trim().is_empty() {
            self.0.remove(&k);
        } else {
            self.0.insert(k, vec![v]);
        }
    }

    pub fn set_many(&mut self, key: &str, values: Vec<String>) {
        let k = key.to_ascii_uppercase();
        let values: Vec<String> = values.into_iter().filter(|v| !v.trim().is_empty()).collect();
        if values.is_empty() {
            self.0.remove(&k);
        } else {
            self.0.insert(k, values);
        }
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.0.get(&key.to_ascii_uppercase()).and_then(|v| v.first()).map(|s| s.as_str())
    }


    /// Overlay `other` on top of self: every key present in `other` replaces ours.
    pub fn overlay(&mut self, other: &Tags) {
        for (k, v) in &other.0 {
            if v.is_empty() {
                self.0.remove(k);
            } else {
                self.0.insert(k.clone(), v.clone());
            }
        }
    }

    pub fn without_track_specific(&self) -> Tags {
        let mut t = self.clone();
        for k in TRACK_SPECIFIC {
            t.0.remove(*k);
        }
        t
    }

    pub fn to_pairs(&self) -> Vec<(String, String)> {
        self.0.iter().flat_map(|(k, vs)| vs.iter().map(move |v| (k.clone(), v.clone()))).collect()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

/// Map cue-sheet disc-level fields to tags.
pub fn from_cue_disc(fields: &BTreeMap<String, String>) -> Tags {
    let mut t = Tags::new();
    for (k, v) in fields {
        match k.as_str() {
            "TITLE" => t.set("ALBUM", v),
            "PERFORMER" => {
                t.set("ALBUMARTIST", v);
                t.set("ARTIST", v);
            }
            "SONGWRITER" | "COMPOSER" => t.set("COMPOSER", v),
            "CATALOG" => t.set("BARCODE", v),
            "DATE" | "YEAR" => t.set("DATE", v),
            "GENRE" => t.set("GENRE", v),
            "DISCID" => t.set("DISCID", v),
            "DISCNUMBER" => t.set("DISCNUMBER", v),
            "TOTALDISCS" | "DISCTOTAL" => t.set("DISCTOTAL", v),
            "LABEL" | "PUBLISHER" => t.set("LABEL", v),
            "CONDUCTOR" | "ORCHESTRA" | "ARRANGER" => t.set(k, v),
            k if k.starts_with("REPLAYGAIN_ALBUM") || k.starts_with("MUSICBRAINZ_") => t.set(k, v),
            _ => {}
        }
    }
    t
}

/// Map cue-sheet track-level fields to tags.
pub fn from_cue_track(fields: &BTreeMap<String, String>) -> Tags {
    let mut t = Tags::new();
    for (k, v) in fields {
        match k.as_str() {
            "TITLE" => t.set("TITLE", v),
            "PERFORMER" => t.set("ARTIST", v),
            "SONGWRITER" | "COMPOSER" => t.set("COMPOSER", v),
            "ISRC" => t.set("ISRC", v),
            "CONDUCTOR" | "ORCHESTRA" | "ARRANGER" | "LYRICIST" => t.set(k, v),
            k if k.starts_with("REPLAYGAIN_TRACK") => t.set(k, v),
            _ => {}
        }
    }
    t
}

/// A filesystem-safe version of a tag value for use in a file name.
pub fn sanitize_name(s: &str) -> String {
    let mut out: String = s
        .chars()
        .map(|c| match c {
            '/' => '∕',
            '\0'..='\x1f' => ' ',
            c => c,
        })
        .collect();
    out = out.trim().trim_end_matches('.').to_string();
    // keep names well under the 255-byte limit (room for prefix + extension)
    while out.len() > 200 {
        out.pop();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn overlay_replaces_whole_key() {
        let mut a = Tags::from_pairs([("ARTIST".into(), "A".into()), ("ARTIST".into(), "B".into()), ("DATE".into(), "1990".into())]);
        let b = Tags::from_pairs([("artist".into(), "C".into())]);
        a.overlay(&b);
        assert_eq!(a.0["ARTIST"], vec!["C"]);
        assert_eq!(a.get("date"), Some("1990"));
    }

    #[test]
    fn sanitize() {
        assert_eq!(sanitize_name("AC/DC: Back in Black."), "AC∕DC: Back in Black");
    }
}
