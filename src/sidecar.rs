//! Sidecar tag files.
//!
//! A sidecar is a TOML file named `wavemorph.toml`, looked up (and merged, later
//! wins) at:
//!   1. `<source dir>/wavemorph.toml`                 (optional, inside the download)
//!   2. `<tags dir>/<root name>/<relative dir>/wavemorph.toml`  (keeps downloads pristine)
//!
//! ```toml
//! [album]                       # applies to every track in the directory
//! ALBUM = "Symphonie Nr. 5"
//! ALBUMARTIST = ["Bayerisches Staatsorchester", "Wolfgang Sawallisch"]
//!
//! [track.3]                     # by track number (split images / SACD)
//! TITLE = "III. Scherzo"
//! [track."2-05"]                # disc 2, track 5 (directories with several discs)
//! TITLE = "..."
//!
//! [file."01 - Allegro.flac"]    # by file name (regular files or virtual names)
//! TITLE = "..."
//! ```
//! Values are strings or arrays of strings; an empty string or empty array
//! removes the tag.

use crate::tags::Tags;
use anyhow::{Context, Result};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

pub const FILE_NAME: &str = "wavemorph.toml";

#[derive(Debug, Clone, Default)]
pub struct Sidecar {
    pub album: Tags,
    pub tracks: BTreeMap<String, Tags>,
    pub files: BTreeMap<String, Tags>,
    /// newest mtime among the sidecar files that contributed
    pub mtime: Option<SystemTime>,
    pub sources: Vec<PathBuf>,
}

fn table_to_tags(t: &toml::Table) -> Tags {
    let mut tags = Tags::new();
    for (k, v) in t {
        let vals: Vec<String> = match v {
            toml::Value::String(s) => vec![s.clone()],
            toml::Value::Array(a) => a.iter().map(value_str).collect(),
            other => vec![value_str(other)],
        };
        // names are kept exactly as written; explicit removals stay as an empty Vec so overlay() deletes the key
        tags.0.insert(
            crate::tags::Key(k.clone()),
            vals.into_iter().filter(|s| !s.is_empty()).collect(),
        );
    }
    tags
}

fn value_str(v: &toml::Value) -> String {
    match v {
        toml::Value::String(s) => s.clone(),
        toml::Value::Integer(i) => i.to_string(),
        toml::Value::Float(f) => f.to_string(),
        toml::Value::Boolean(b) => b.to_string(),
        toml::Value::Datetime(d) => d.to_string(),
        other => other.to_string(),
    }
}

impl Sidecar {
    pub fn parse(text: &str) -> Result<Sidecar> {
        let doc: toml::Table = text.parse()?;
        let mut sc = Sidecar::default();
        if let Some(toml::Value::Table(a)) = doc.get("album") {
            sc.album = table_to_tags(a);
        }
        for (section, target) in [("track", &mut sc.tracks), ("file", &mut sc.files)] {
            if let Some(toml::Value::Table(t)) = doc.get(section) {
                for (k, v) in t {
                    if let toml::Value::Table(tt) = v {
                        target.insert(k.clone(), table_to_tags(tt));
                    }
                }
            }
        }
        Ok(sc)
    }

    /// Merge the sidecars that apply to `src_dir`.
    pub fn load(src_dir: &Path, overlay_dir: Option<&Path>) -> Result<Option<Sidecar>> {
        let mut out: Option<Sidecar> = None;
        let candidates = [
            Some(src_dir.join(FILE_NAME)),
            overlay_dir.map(|d| d.join(FILE_NAME)),
        ];
        for p in candidates.into_iter().flatten() {
            let Ok(md) = std::fs::metadata(&p) else {
                continue;
            };
            let text =
                std::fs::read_to_string(&p).with_context(|| format!("read {}", p.display()))?;
            let sc = Sidecar::parse(&text).with_context(|| format!("parse {}", p.display()))?;
            let acc = out.get_or_insert_with(Sidecar::default);
            acc.album.overlay(&sc.album);
            for (k, v) in sc.tracks {
                acc.tracks.entry(k).or_default().overlay(&v);
            }
            for (k, v) in sc.files {
                acc.files.entry(k).or_default().overlay(&v);
            }
            let m = md.modified().ok();
            acc.mtime = acc.mtime.max(m);
            acc.sources.push(p);
        }
        Ok(out)
    }

    /// Tags for track `n` of disc `disc` (disc only matters in multi-disc dirs).
    pub fn track(&self, disc: Option<u32>, n: u32) -> Option<&Tags> {
        let mut keys = Vec::new();
        if let Some(d) = disc {
            keys.push(format!("{d}-{n}"));
            keys.push(format!("{d}-{n:02}"));
            keys.push(format!("{d}.{n}"));
        } else {
            keys.push(n.to_string());
            keys.push(format!("{n:02}"));
        }
        keys.iter().find_map(|k| self.tracks.get(k))
    }

    pub fn file(&self, name: &str) -> Option<&Tags> {
        self.files.get(name)
    }

    /// Serialize effective tags back to TOML (used by `tags init`).
    pub fn render(album: &Tags, tracks: &[(String, Tags)], files: &[(String, Tags)]) -> String {
        fn table(t: &Tags) -> toml::Table {
            let mut tab = toml::Table::new();
            for (k, v) in &t.0 {
                let k = &k.0;
                let val = if v.len() == 1 {
                    toml::Value::String(v[0].clone())
                } else {
                    toml::Value::Array(v.iter().cloned().map(toml::Value::String).collect())
                };
                tab.insert(k.clone(), val);
            }
            tab
        }
        let mut doc = toml::Table::new();
        doc.insert("album".into(), toml::Value::Table(table(album)));
        if !tracks.is_empty() {
            let mut t = toml::Table::new();
            for (k, v) in tracks {
                t.insert(k.clone(), toml::Value::Table(table(v)));
            }
            doc.insert("track".into(), toml::Value::Table(t));
        }
        if !files.is_empty() {
            let mut t = toml::Table::new();
            for (k, v) in files {
                t.insert(k.clone(), toml::Value::Table(table(v)));
            }
            doc.insert("file".into(), toml::Value::Table(t));
        }
        format!(
            "# WaveMorphFS sidecar tags — edit freely; the filesystem picks up changes within seconds.\n{}",
            toml::to_string_pretty(&doc).unwrap_or_default()
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quoted_names_kept_verbatim() {
        let sc = Sidecar::parse(
            r#"
[file."01 - Song.m4a"]
"MusicBrainz Album Id" = "abc"
album_artist = "Someone"
"TXXX:Custom/Tag" = "v"
"#,
        )
        .unwrap();
        let f = sc.file("01 - Song.m4a").unwrap();
        let names: Vec<String> = f.to_pairs().into_iter().map(|(k, _)| k).collect();
        assert!(names.contains(&"MusicBrainz Album Id".to_string()));
        assert!(names.contains(&"album_artist".to_string()));
        assert!(names.contains(&"TXXX:Custom/Tag".to_string()));
        assert_eq!(f.get("MUSICBRAINZ ALBUM ID"), Some("abc"));
    }

    #[test]
    fn parse_and_lookup() {
        let sc = Sidecar::parse(
            r#"
[album]
ALBUM = "X"
artist = ["A", "B"]
COMMENT = ""
[track.3]
TITLE = "Three"
[track."2-05"]
TITLE = "Disc two five"
[file."a b.flac"]
TITLE = "File"
"#,
        )
        .unwrap();
        assert_eq!(
            sc.album.get_all("ARTIST").unwrap(),
            &vec!["A".to_string(), "B".to_string()]
        );
        assert!(
            sc.album.get_all("COMMENT").unwrap().is_empty(),
            "explicit removal kept as empty"
        );
        assert_eq!(sc.track(None, 3).unwrap().get("TITLE"), Some("Three"));
        assert_eq!(
            sc.track(Some(2), 5).unwrap().get("TITLE"),
            Some("Disc two five")
        );
        assert_eq!(sc.file("a b.flac").unwrap().get("TITLE"), Some("File"));
        let mut base = Tags::from_pairs([("COMMENT".into(), "rip info".into())]);
        base.overlay(&sc.album);
        assert!(base.get("COMMENT").is_none(), "removal applied");
    }
}
