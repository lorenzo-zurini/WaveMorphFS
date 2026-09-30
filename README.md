# WaveMorphFS

A read-only FUSE filesystem that shows a music library the way a music server
wants to see it — **one properly tagged file per track** — while the files on disk
stay **exactly as downloaded**, so torrents keep seeding.

| On disk (pristine)                       | What the mount shows                                  |
|------------------------------------------|-------------------------------------------------------|
| `Album.cue` + `Album.flac` (image rip)   | `01 - Title.flac`, `02 - Title.flac`, …               |
| `Album.cue` + `Album.ape` / `.wv` / `.wav` | same, via a verified lossless FLAC conversion in cache |
| `Disc.iso` (SACD, plain DSD **or DST**)  | `01 - Title.dsf`, `02 - Title.dsf`, …                 |
| any other file                           | passed through untouched (FLACs retagged if a sidecar applies) |
| `*.!qB`, `*.parts`, dotfiles             | hidden                                                |

Tags come from the image itself, the CUE sheet / SACD text, and **sidecar files**
you control — never written into the downloads.

## How it works

* **CUE + FLAC images are split sample-exactly without re-encoding.** Every source
  frame that lies inside a track is copied byte-for-byte; only its frame header is
  rewritten (variable-blocksize, track-relative sample number, new CRC-8/CRC-16).
  The partial frames at the two track boundaries are decoded and re-emitted as
  VERBATIM frames. Output sizes are computable up front, so `stat` is exact and
  random access is cheap.
* **Images are fully verified before they appear.** The first time an image is
  seen, one sequential pass checks every frame's CRC and numbering and records its
  offset (cached on disk). BitTorrent downloads pieces in random order, so this is
  also what keeps half-finished downloads out of the library.
* **Non-FLAC images** (APE, WavPack, TTA, TAK, WAV, ALAC) are converted once to FLAC
  in the cache; the conversion is accepted only if the decoded PCM MD5 matches the
  source. The source file is never modified.
* **SACD ISOs** (Scarlet Book) are parsed for track lists and text; the stereo area
  is exposed as DSF. With `--sacd-multichannel` the multichannel area is exposed too,
  as `MC NN - Title.dsf` tagged `<album> (Multichannel)` (5.0/5.1 channel layouts). Plain DSD is only re-ordered (byte de-interleave + bit
  reversal); DST-compressed areas are decoded by a port of FFmpeg's DST decoder.
* Each split track's STREAMINFO carries the real audio MD5, computed in the
  background after the image is indexed (one decode pass per image, cached), so
  `flac -t` and players can verify every track end to end.
* Slow work (indexing, conversion, SACD scans) runs in background workers; a
  folder's contents appear when ready and its mtime changes so music servers rescan.

## Verification

Everything that produces audio is checked against independent references:

* split tracks pass `flac -t` and decode PCM-identical to cutting the image with the
  reference `flac --skip/--until`; 48 synthetic edge cases (1- and 15-sample
  boundary frames, tracks inside a single frame, 8/16/24-bit, 1–6 channels, block
  sizes 576–4608) are unit tests
* split tracks are PCM-identical to splits made earlier by other tools
  (`wavemorphfs compare`)
* SACD DSF output decodes PCM-identical to a DFF built from the raw disc bytes,
  and DSD-bit-identical to `sacd_extract`-style DSF extractions
* DST decoding is PCM-identical to FFmpeg's decoder

## Usage

```sh
cargo build --release

# mount (roots default to ~/Storage/Music and ~/Storage/Classical Music)
wavemorphfs --root "Music=~/Storage/Music" --root "Classical Music=~/Storage/Classical Music" \
    mount ~/Storage/WaveMorph/mnt --allow-other

wavemorphfs ls <source dir>                 # what a folder looks like through the fs
wavemorphfs verify <source dir>             # validate its virtual tracks with flac/ffmpeg
wavemorphfs compare <source dir> <split dir> # PCM-compare virtual tracks with old split files
wavemorphfs tags-init <source dir>          # write a sidecar pre-filled with current tags
```

`--allow-other` needs `user_allow_other` in `/etc/fuse.conf`. A systemd user unit
lives in the operator notes; containers should bind the **parent** of the mountpoint
with `bind-propagation=rslave` so they survive filesystem restarts.

## Sidecar tags

`wavemorph.toml`, either next to the files or (to keep downloads untouched) in a
separate tree: `<tags-dir>/<root name>/<path relative to the root>/wavemorph.toml`.
Both are merged, the separate tree winning.

```toml
[album]                        # every track in the folder
ALBUM = "Symphonie Nr. 5"
ALBUMARTIST = ["Bayerisches Staatsorchester", "Wolfgang Sawallisch"]
COMMENT = ""                   # empty string removes a tag

[track.3]                      # by track number (split images, SACDs)
TITLE = "III. Scherzo"

[track."2-05"]                 # disc 2, track 5 (folders with several discs)
TITLE = "…"

[file."01 - Allegro.flac"]     # by file name
TITLE = "…"
```

Edits are picked up within seconds. A `cover.jpg`/`cover.png` in the sidecar folder
is shown in the album folder if it has no cover of its own.

## Limitations

* Variable-blocksize FLAC images are not split (they are shown as-is).

## License

MIT, except `src/dst.rs` (a port of FFmpeg's DST decoder), which is
LGPL-2.1-or-later like its origin.
