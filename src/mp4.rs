//! M4A/MP4 retagging: the `moov/udta/meta/ilst` item list is rewritten in memory
//! and the file is served as [bytes before moov] + new moov + [bytes after moov].
//! When moov sits before the media data its size change is compensated in every
//! chunk offset table (stco/co64).
//!
//! Items the overlay does not name are kept byte for byte (cover art included).
//! Tag names are never rewritten: names that are not a known iTunes atom become
//! freeform `----:com.apple.iTunes:<name>` items with the name exactly as written.

use crate::tags::Tags;
use anyhow::{Context, Result, bail};
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::Path;

#[derive(Debug, Clone)]
enum Node {
    Leaf {
        typ: [u8; 4],
        data: Vec<u8>,
    },
    /// `prefix` holds a full box's version/flags (meta)
    Container {
        typ: [u8; 4],
        prefix: Vec<u8>,
        children: Vec<Node>,
    },
}

impl Node {
    fn typ(&self) -> [u8; 4] {
        match self {
            Node::Leaf { typ, .. } | Node::Container { typ, .. } => *typ,
        }
    }

    fn len(&self) -> u64 {
        let payload = match self {
            Node::Leaf { data, .. } => data.len() as u64,
            Node::Container {
                prefix, children, ..
            } => prefix.len() as u64 + children.iter().map(Node::len).sum::<u64>(),
        };
        if payload + 8 > u32::MAX as u64 {
            payload + 16
        } else {
            payload + 8
        }
    }

    fn write(&self, out: &mut Vec<u8>) {
        let len = self.len();
        if len > u32::MAX as u64 {
            out.extend_from_slice(&1u32.to_be_bytes());
            out.extend_from_slice(&self.typ());
            out.extend_from_slice(&len.to_be_bytes());
        } else {
            out.extend_from_slice(&(len as u32).to_be_bytes());
            out.extend_from_slice(&self.typ());
        }
        match self {
            Node::Leaf { data, .. } => out.extend_from_slice(data),
            Node::Container {
                prefix, children, ..
            } => {
                out.extend_from_slice(prefix);
                for c in children {
                    c.write(out);
                }
            }
        }
    }

    fn child_mut(&mut self, typ: &[u8; 4]) -> Option<&mut Node> {
        match self {
            Node::Container { children, .. } => children.iter_mut().find(|c| &c.typ() == typ),
            Node::Leaf { .. } => None,
        }
    }

    fn children_mut(&mut self) -> &mut Vec<Node> {
        match self {
            Node::Container { children, .. } => children,
            Node::Leaf { .. } => unreachable!("leaf has no children"),
        }
    }
}

const CONTAINERS: &[&[u8; 4]] = &[
    b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta", b"meta",
];

/// Parse the boxes in `b` (no box crosses its end).
fn parse(b: &[u8]) -> Result<Vec<Node>> {
    let mut out = Vec::new();
    let mut pos = 0usize;
    while pos + 8 <= b.len() {
        let mut size = u32::from_be_bytes(b[pos..pos + 4].try_into()?) as u64;
        let typ: [u8; 4] = b[pos + 4..pos + 8].try_into()?;
        let mut hdr = 8;
        if size == 1 {
            size = u64::from_be_bytes(b.get(pos + 8..pos + 16).context("short box")?.try_into()?);
            hdr = 16;
        } else if size == 0 {
            size = (b.len() - pos) as u64;
        }
        let end = pos
            .checked_add(size as usize)
            .filter(|&e| e <= b.len() && size as usize >= hdr)
            .context("box overruns parent")?;
        let payload = &b[pos + hdr..end];
        if CONTAINERS.contains(&&typ) {
            // meta is a full box in ISO files but a plain container in some QuickTime files
            let prefix_len = if &typ == b"meta" && payload.len() >= 8 && &payload[4..8] != b"hdlr" {
                4
            } else {
                0
            };
            out.push(Node::Container {
                typ,
                prefix: payload[..prefix_len].to_vec(),
                children: parse(&payload[prefix_len..])?,
            });
        } else {
            out.push(Node::Leaf {
                typ,
                data: payload.to_vec(),
            });
        }
        pos = end;
    }
    Ok(out)
}

/// What an ilst item is keyed by.
#[derive(Debug, Clone, PartialEq)]
enum Item {
    Atom([u8; 4], Kind),
    Freeform(String),
    /// container fields ffprobe reports next to the tags; not ilst items
    Ignore,
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Kind {
    Text,
    Pair, // trkn / disk
    U8,
    U16,
}

fn atom(s: &str) -> [u8; 4] {
    // "©nam": © is 0xA9 in the atom name
    let mut out = [0u8; 4];
    let mut i = 0;
    for c in s.chars() {
        out[i] = if c == '©' { 0xA9 } else { c as u8 };
        i += 1;
    }
    out
}

const TRACK_NAMES: &[&str] = &["TRACKNUMBER", "TRACK", "TRKN"];
const TRACK_TOTALS: &[&str] = &["TRACKTOTAL", "TOTALTRACKS"];
const DISC_NAMES: &[&str] = &["DISCNUMBER", "DISC", "DISK"];
const DISC_TOTALS: &[&str] = &["DISCTOTAL", "TOTALDISCS"];

/// Tag name -> ilst item. Accepts Vorbis names (TITLE, ALBUMARTIST), the names
/// ffprobe reports for MP4 sources (album_artist, sort_album, ...) and raw atom
/// names (©nam, aART). Case-insensitive except for raw atom names.
fn item_for(key: &str) -> Item {
    let k = key.to_ascii_uppercase();
    let a = |s: &str| Item::Atom(atom(s), Kind::Text);
    if TRACK_NAMES.contains(&k.as_str()) || TRACK_TOTALS.contains(&k.as_str()) {
        return Item::Atom(*b"trkn", Kind::Pair);
    }
    if DISC_NAMES.contains(&k.as_str()) || DISC_TOTALS.contains(&k.as_str()) {
        return Item::Atom(*b"disk", Kind::Pair);
    }
    match k.as_str() {
        "MAJOR_BRAND" | "MINOR_VERSION" | "COMPATIBLE_BRANDS" | "CREATION_TIME" => Item::Ignore,
        "TITLE" => a("©nam"),
        "ARTIST" => a("©ART"),
        "ALBUMARTIST" | "ALBUM_ARTIST" | "ALBUM ARTIST" => a("aART"),
        "ALBUM" => a("©alb"),
        "DATE" | "YEAR" => a("©day"),
        "GENRE" => a("©gen"),
        "COMPOSER" => a("©wrt"),
        "COMMENT" => a("©cmt"),
        "DESCRIPTION" => a("desc"),
        "SYNOPSIS" => a("ldes"),
        "ENCODER" => a("©too"),
        "COPYRIGHT" => a("cprt"),
        "GROUPING" => a("©grp"),
        "LYRICS" | "UNSYNCEDLYRICS" => a("©lyr"),
        "WORK" => a("©wrk"),
        "MOVEMENTNAME" => a("©mvn"),
        "TITLESORT" | "TITLE_SORT" | "SORT_NAME" => a("sonm"),
        "ALBUMSORT" | "ALBUM_SORT" | "SORT_ALBUM" => a("soal"),
        "ARTISTSORT" | "ARTIST_SORT" | "SORT_ARTIST" => a("soar"),
        "ALBUMARTISTSORT" | "ALBUM_ARTIST_SORT" | "SORT_ALBUM_ARTIST" => a("soaa"),
        "COMPOSERSORT" | "COMPOSER_SORT" | "SORT_COMPOSER" => a("socm"),
        "SHOW" => a("tvsh"),
        "NETWORK" => a("tvnn"),
        "EPISODE_ID" => a("tven"),
        "PURCHASE_DATE" => a("purd"),
        "COMPILATION" => Item::Atom(*b"cpil", Kind::U8),
        "GAPLESS_PLAYBACK" => Item::Atom(*b"pgap", Kind::U8),
        "MEDIA_TYPE" => Item::Atom(*b"stik", Kind::U8),
        "RATING" => Item::Atom(*b"rtng", Kind::U8),
        "BPM" | "TMPO" => Item::Atom(*b"tmpo", Kind::U16),
        _ if key.chars().count() == 4 && key.starts_with('©') => a(key),
        _ if matches!(
            key,
            "aART"
                | "cprt"
                | "desc"
                | "ldes"
                | "sonm"
                | "soal"
                | "soar"
                | "soaa"
                | "socm"
                | "tvsh"
                | "tvnn"
                | "tven"
                | "purd"
        ) =>
        {
            a(key)
        }
        _ => Item::Freeform(key.to_string()),
    }
}

fn data_atom(typ: u32, payload: &[u8]) -> Node {
    let mut d = typ.to_be_bytes().to_vec();
    d.extend_from_slice(&[0, 0, 0, 0]); // locale
    d.extend_from_slice(payload);
    Node::Leaf {
        typ: *b"data",
        data: d,
    }
}

fn freeform_name(n: &Node) -> Option<String> {
    let Node::Container { children, .. } = n else {
        return None;
    };
    children.iter().find_map(|c| match c {
        Node::Leaf { typ, data } if typ == b"name" && data.len() >= 4 => {
            Some(String::from_utf8_lossy(&data[4..]).into_owned())
        }
        _ => None,
    })
}

/// Number pair (trkn/disk) stored in an existing item.
fn old_pair(items: &[Node], typ: &[u8; 4]) -> Option<(u16, u16)> {
    items
        .iter()
        .find(|n| &n.typ() == typ)
        .and_then(|n| match n {
            Node::Container { children, .. } => children.iter().find_map(|c| match c {
                Node::Leaf { typ, data } if typ == b"data" && data.len() >= 14 => Some((
                    u16::from_be_bytes([data[10], data[11]]),
                    u16::from_be_bytes([data[12], data[13]]),
                )),
                _ => None,
            }),
            _ => None,
        })
}

fn parse_pair(s: &str) -> (Option<u16>, Option<u16>) {
    let mut it = s.split('/');
    let a = it.next().and_then(|x| x.trim().parse().ok());
    let b = it.next().and_then(|x| x.trim().parse().ok());
    (a, b)
}

/// Apply `overlay` to an ilst's items.
fn apply(items: &mut Vec<Node>, overlay: &Tags) {
    let get = |names: &[&str]| -> Option<&str> { names.iter().find_map(|n| overlay.get(n)) };
    let named = |names: &[&str]| names.iter().any(|n| overlay.get_all(n).is_some());
    for (typ, nums, totals) in [
        (b"trkn", TRACK_NAMES, TRACK_TOTALS),
        (b"disk", DISC_NAMES, DISC_TOTALS),
    ] {
        if !named(nums) && !named(totals) {
            continue;
        }
        let (old_n, old_t) = old_pair(items, typ)
            .map(|(a, b)| (Some(a), Some(b)))
            .unwrap_or((None, None));
        let (n, t_in_n) = get(nums).map(parse_pair).unwrap_or((None, None));
        let n = if named(nums) { n } else { old_n };
        let t = get(totals)
            .and_then(|x| x.trim().parse().ok())
            .or(t_in_n)
            .or(if named(totals) { None } else { old_t });
        items.retain(|i| &i.typ() != typ);
        if let Some(n) = n {
            let mut p = vec![0, 0];
            p.extend_from_slice(&n.to_be_bytes());
            p.extend_from_slice(&t.unwrap_or(0).to_be_bytes());
            if typ == b"trkn" {
                p.extend_from_slice(&[0, 0]);
            }
            items.push(Node::Container {
                typ: *typ,
                prefix: vec![],
                children: vec![data_atom(0, &p)],
            });
        }
    }
    let mut used: Vec<Item> = Vec::new();
    for (k, vs) in &overlay.0 {
        let it = item_for(&k.0);
        if matches!(it, Item::Ignore | Item::Atom(_, Kind::Pair)) || used.contains(&it) {
            continue;
        }
        match &it {
            Item::Atom(t, _) => {
                items.retain(|i| &i.typ() != t && !(t == &atom("©gen") && &i.typ() == b"gnre"))
            }
            Item::Freeform(name) => items.retain(|i| {
                !(&i.typ() == b"----"
                    && freeform_name(i).is_some_and(|n| n.eq_ignore_ascii_case(name)))
            }),
            Item::Ignore => {}
        }
        if !vs.is_empty() {
            let children: Vec<Node> = match &it {
                Item::Atom(_, Kind::Text) => {
                    vs.iter().map(|v| data_atom(1, v.as_bytes())).collect()
                }
                Item::Atom(_, Kind::U8) => vs
                    .first()
                    .and_then(|v| {
                        v.trim().parse::<u8>().ok().or(match v.trim() {
                            "true" => Some(1),
                            "false" => Some(0),
                            _ => None,
                        })
                    })
                    .map(|n| vec![data_atom(21, &[n])])
                    .unwrap_or_default(),
                Item::Atom(_, Kind::U16) => vs
                    .first()
                    .and_then(|v| v.trim().parse::<f64>().ok())
                    .map(|n| vec![data_atom(21, &(n.round() as u16).to_be_bytes())])
                    .unwrap_or_default(),
                Item::Freeform(name) => {
                    let mut c = vec![
                        Node::Leaf {
                            typ: *b"mean",
                            data: [&[0u8, 0, 0, 0][..], b"com.apple.iTunes"].concat(),
                        },
                        Node::Leaf {
                            typ: *b"name",
                            data: [&[0u8, 0, 0, 0][..], name.as_bytes()].concat(),
                        },
                    ];
                    c.extend(vs.iter().map(|v| data_atom(1, v.as_bytes())));
                    c
                }
                _ => vec![],
            };
            if !children.is_empty() {
                let typ = match &it {
                    Item::Atom(t, _) => *t,
                    _ => *b"----",
                };
                items.push(Node::Container {
                    typ,
                    prefix: vec![],
                    children,
                });
            }
        }
        used.push(it);
    }
}

/// Parse ilst items: each item is a container of data/mean/name atoms.
fn parse_ilst(data: &[u8]) -> Result<Vec<Node>> {
    Ok(parse(data)?
        .into_iter()
        .map(|n| match n {
            Node::Leaf { typ, data } => match parse(&data) {
                Ok(children) => Node::Container {
                    typ,
                    prefix: vec![],
                    children,
                },
                Err(_) => Node::Leaf { typ, data },
            },
            c => c,
        })
        .collect())
}

/// Add `delta` to every chunk offset at or beyond `from`.
fn shift_offsets(n: &mut Node, from: u64, delta: i64) -> Result<()> {
    match n {
        Node::Container { children, .. } => {
            for c in children {
                shift_offsets(c, from, delta)?;
            }
        }
        Node::Leaf { typ, data } if typ == b"stco" || typ == b"co64" => {
            let wide = typ == b"co64";
            let count =
                u32::from_be_bytes(data.get(4..8).context("short stco")?.try_into()?) as usize;
            let w = if wide { 8 } else { 4 };
            if data.len() < 8 + count * w {
                bail!("short chunk offset table");
            }
            for i in 0..count {
                let p = 8 + i * w;
                let v = if wide {
                    u64::from_be_bytes(data[p..p + 8].try_into()?)
                } else {
                    u32::from_be_bytes(data[p..p + 4].try_into()?) as u64
                };
                if v >= from {
                    let nv = (v as i64 + delta) as u64;
                    if wide {
                        data[p..p + 8].copy_from_slice(&nv.to_be_bytes());
                    } else {
                        data[p..p + 4].copy_from_slice(
                            &u32::try_from(nv)
                                .context("chunk offset overflow")?
                                .to_be_bytes(),
                        );
                    }
                }
            }
        }
        _ => {}
    }
    Ok(())
}

/// Retag an MP4 file: returns (moov offset, original moov length, new moov).
pub fn retag_m4a(path: &Path, overlay: &Tags) -> Result<(u64, u64, Vec<u8>)> {
    let f = File::open(path)?;
    let flen = f.metadata()?.len();
    let mut pos = 0u64;
    let mut moov = None;
    while pos + 8 <= flen {
        let mut h = [0u8; 16];
        f.read_exact_at(&mut h[..8], pos)?;
        let mut size = u32::from_be_bytes(h[0..4].try_into()?) as u64;
        let typ: [u8; 4] = h[4..8].try_into()?;
        if size == 1 {
            f.read_exact_at(&mut h[8..16], pos + 8)?;
            size = u64::from_be_bytes(h[8..16].try_into()?);
        } else if size == 0 {
            size = flen - pos;
        }
        if size < 8 {
            bail!("bad box at {pos}");
        }
        if &typ == b"moof" {
            bail!("fragmented MP4");
        }
        if &typ == b"moov" {
            moov = Some((pos, size));
        }
        pos += size;
    }
    let (mpos, mlen) = moov.context("no moov box")?;
    let mut raw = vec![0u8; mlen as usize];
    f.read_exact_at(&mut raw, mpos)?;
    let mut root = parse(&raw)?.pop().context("moov parse")?;

    // moov/udta/meta(hdlr mdir)/ilst, created when missing
    if root.child_mut(b"udta").is_none() {
        root.children_mut().push(Node::Container {
            typ: *b"udta",
            prefix: vec![],
            children: vec![],
        });
    }
    let udta = root.child_mut(b"udta").expect("udta");
    if udta.child_mut(b"meta").is_none() {
        let mut hdlr = vec![0u8; 8];
        hdlr.extend_from_slice(b"mdirappl");
        hdlr.extend_from_slice(&[0u8; 9]);
        udta.children_mut().push(Node::Container {
            typ: *b"meta",
            prefix: vec![0, 0, 0, 0],
            children: vec![Node::Leaf {
                typ: *b"hdlr",
                data: hdlr,
            }],
        });
    }
    let meta = udta.child_mut(b"meta").expect("meta");
    let ilst_data = match meta.child_mut(b"ilst") {
        Some(Node::Leaf { data, .. }) => std::mem::take(data),
        _ => Vec::new(),
    };
    let mut items = parse_ilst(&ilst_data)?;
    apply(&mut items, overlay);
    let mut body = Vec::new();
    for i in &items {
        i.write(&mut body);
    }
    match meta.child_mut(b"ilst") {
        Some(Node::Leaf { data, .. }) => *data = body,
        _ => meta.children_mut().push(Node::Leaf {
            typ: *b"ilst",
            data: body,
        }),
    }

    let delta = root.len() as i64 - mlen as i64;
    if delta != 0 {
        shift_offsets(&mut root, mpos + mlen, delta)?;
    }
    let mut out = Vec::with_capacity(root.len() as usize);
    root.write(&mut out);
    Ok((mpos, mlen, out))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bx(typ: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut v = ((payload.len() + 8) as u32).to_be_bytes().to_vec();
        v.extend_from_slice(typ);
        v.extend_from_slice(payload);
        v
    }

    #[test]
    fn rewrites_ilst_and_shifts_offsets() {
        // ftyp, moov(trak/mdia/minf/stbl/stco -> mdat payload, udta/meta/ilst ©nam + covr), mdat
        let item = |t: &[u8; 4], typ: u32, p: &[u8]| {
            bx(
                t,
                &bx(
                    b"data",
                    &[&typ.to_be_bytes()[..], &[0, 0, 0, 0], p].concat(),
                ),
            )
        };
        let ilst = bx(
            b"ilst",
            &[item(&atom("©nam"), 1, b"Old"), item(b"covr", 13, b"JPEG")].concat(),
        );
        let meta = bx(
            b"meta",
            &[&[0u8, 0, 0, 0][..], &bx(b"hdlr", &[0u8; 25]), &ilst].concat(),
        );
        let ftyp = bx(b"ftyp", b"M4A \0\0\0\0");
        let build = |off: u32| {
            let stco = bx(
                b"stco",
                &[&[0u8; 4][..], &1u32.to_be_bytes(), &off.to_be_bytes()].concat(),
            );
            let trak = bx(b"trak", &bx(b"mdia", &bx(b"minf", &bx(b"stbl", &stco))));
            bx(b"moov", &[trak, bx(b"udta", &meta)].concat())
        };
        let moov_len = build(0).len() as u32;
        let audio_off = ftyp.len() as u32 + moov_len + 8;
        let file = [ftyp.clone(), build(audio_off), bx(b"mdat", b"AUDIO")].concat();
        let dir = std::env::temp_dir().join(format!("wm-mp4-{}", std::process::id()));
        std::fs::write(&dir, &file).unwrap();
        let ov = Tags::from_pairs([
            ("title".into(), "A much longer new title".into()),
            ("MusicBrainz Album Id".into(), "mbid".into()),
            ("track".into(), "3/12".into()),
        ]);
        let (mpos, mlen, new) = retag_m4a(&dir, &ov).unwrap();
        std::fs::remove_file(&dir).ok();
        assert_eq!((mpos, mlen), (ftyp.len() as u64, moov_len as u64));
        let out = [
            &file[..mpos as usize],
            &new,
            &file[(mpos + mlen) as usize..],
        ]
        .concat();
        // chunk offset still points at the audio
        let root = parse(&new).unwrap().pop().unwrap();
        let mut stco_off = None;
        fn find(n: &Node, f: &mut Option<u32>) {
            match n {
                Node::Container { children, .. } => children.iter().for_each(|c| find(c, f)),
                Node::Leaf { typ, data } if typ == b"stco" => {
                    *f = Some(u32::from_be_bytes(data[8..12].try_into().unwrap()))
                }
                _ => {}
            }
        }
        find(&root, &mut stco_off);
        let o = stco_off.unwrap() as usize;
        assert_eq!(&out[o..o + 5], b"AUDIO");
        let s = String::from_utf8_lossy(&new);
        assert!(s.contains("A much longer new title") && !s.contains("Old"));
        assert!(s.contains("JPEG"), "cover kept");
        assert!(s.contains("com.apple.iTunes") && s.contains("MusicBrainz Album Id"));
        assert!(
            new.windows(8).any(|w| w == [0, 0, 0, 3, 0, 12, 0, 0]),
            "trkn 3/12"
        );
    }
}
