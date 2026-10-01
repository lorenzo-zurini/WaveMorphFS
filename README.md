# WaveMorphFS

A read-only FUSE filesystem that shows a music library the way a music server
wants to see it — **one properly tagged file per track** — while the files on disk
stay **exactly as downloaded**, so torrents keep seeding.

| On disk (pristine)                         | What the mount shows                                   |
|--------------------------------------------|--------------------------------------------------------|
| `Album.cue` + `Album.flac` (image rip)     | `01 - Title.flac`, `02 - Title.flac`, …                |
| `Album.cue` + `Album.ape` / `.wv` / `.wav` | same, encoded to FLAC on the fly from the decoded image |
| `Disc.iso` (SACD, plain DSD **or DST**)    | `01 - Title.dsf`, `02 - Title.dsf`, …                  |
| `*.flac`, `*.mp3`, `*.m4a`                 | the same file with sidecar tags applied                |
| any other file                             | passed through untouched                               |
| `*.!qB`, `*.parts`, dotfiles               | hidden                                                 |

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
* **Non-FLAC images** (APE, WavPack, TTA, TAK, WAV, ALAC) cannot be cut without
  re-encoding (APE frames are seconds long and must all be full length), so their
  tracks are served as compressed FLAC encoded on the fly. Nothing is stored but
  positions and sizes: the first time an image is seen it is decoded once with
  checksum verification (FFmpeg's decoders) and every track frame is encoded once
  to learn its size, so file sizes are exact. Reads then decode just the packets
  they need and encode just those frames; FLAC frames are independent, so a frame
  re-encoded later is byte-identical.
* **SACD ISOs** (Scarlet Book) are parsed for track lists and text; the stereo area
  is exposed as DSF. With `--sacd-multichannel` the multichannel area is exposed too,
  as `MC NN - Title.dsf` tagged `<album> (Multichannel)`. Plain DSD is only
  re-ordered (byte de-interleave + bit reversal); DST-compressed areas are decoded
  by a port of FFmpeg's DST decoder.
* **Regular FLAC, MP3 and M4A files** get sidecar tags without touching the audio:
  only the tag area is rebuilt (VORBIS_COMMENT; ID3v2.4; the MP4 `ilst`, with chunk
  offsets adjusted). Tags and pictures the sidecar does not name are kept as is.
* Each split track's STREAMINFO carries the real audio MD5, computed in the
  background after the image is indexed (one decode pass per image, cached), so
  `flac -t` and players can verify every track end to end.
* Slow work (indexing, conversion, SACD scans) runs in background workers; a
  folder's contents appear when ready and its mtime changes so music servers rescan.

## Verification

Everything that produces audio is checked against independent references:

* split tracks pass `flac -t` and decode PCM-identical to the source samples;
  48 synthetic edge cases (1- and 15-sample boundary frames, tracks inside a single
  frame, 8/16/24-bit, 1–6 channels, block sizes 576–4608) are unit tests
* split tracks are PCM-identical to splits made earlier by other tools
  (`wavemorphfs compare`)
* SACD DSF output decodes PCM-identical to a DFF built from the raw disc bytes,
  and DSD-bit-identical to `sacd_extract`-style DSF extractions
* DST decoding is PCM-identical to FFmpeg's decoder
* retagged MP3/M4A files carry the identical audio packets (`wavemorphfs verify`)

## Building

Needs a C++20 compiler, CMake, libfuse3, libFLAC, FFmpeg's libavformat/libavcodec
and nlohmann/json; `flac` and `ffmpeg` are used by `verify`, `compare` and the tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/wavemorphfs-tests      # unit tests (need the flac tool)
```

The `Dockerfile` builds and tests it on Debian and runs the mount in a container
(needs `/dev/fuse`, `CAP_SYS_ADMIN`, and the mountpoint's parent bound with
`rshared` propagation; see `docker/entrypoint.sh`).

## Usage

```sh
# mount (roots default to ~/Storage/Music and ~/Storage/Classical Music)
wavemorphfs --root "Music=~/Storage/Music" --root "Classical Music=~/Storage/Classical Music" \
    mount ~/Storage/WaveMorph/mnt --allow-other

wavemorphfs ls <source dir>                  # what a folder looks like through the fs
wavemorphfs verify <source dir>              # validate its virtual tracks with flac/ffmpeg
wavemorphfs compare <source dir> <split dir> # PCM-compare virtual tracks with old split files
wavemorphfs tags-init <source dir>           # write a sidecar pre-filled with current tags
```

`--allow-other` needs `user_allow_other` in `/etc/fuse.conf`. Containers that read
the mount should bind the **parent** of the mountpoint with `bind-propagation=rslave`
so they survive filesystem restarts. `WAVEMORPH_LOG=debug` for more logging.

## Sidecar tags

`wavemorph.json`, either next to the files or (to keep downloads untouched) in a
separate tree: `<tags-dir>/<root name>/<path relative to the root>/wavemorph.json`.
Both are merged, the separate tree winning.

```json
{
  "album": {
    "ALBUM": "Symphonie Nr. 5",
    "ALBUMARTIST": ["Bayerisches Staatsorchester", "Wolfgang Sawallisch"],
    "COMMENT": ""
  },
  "track": {
    "3": {"TITLE": "III. Scherzo"},
    "2-05": {"TITLE": "…"}
  },
  "file": {
    "01 - Allegro.flac": {"TITLE": "…", "MusicBrainz Album Id": "…"}
  }
}
```

* `album` applies to every track in the folder, `track` by track number (`"2-05"`:
  disc 2, track 5, for folders with several discs), `file` by file name.
* Values are strings or lists of strings; an empty string or list removes the tag.
* **Tag names are used exactly as written** — Picard's `MusicBrainz Album Id`,
  ffprobe's `album_artist` and plain Vorbis names all work. Names match
  case-insensitively, and a name replaces its synonyms from other conventions
  (`album_artist` replaces the image's `ALBUMARTIST`), keeping the sidecar's
  spelling. In ID3/MP4 output, names without a standard frame or atom become
  `TXXX` / freeform items under their exact name.
* Other top-level keys (e.g. `"_comment"`) are ignored; `//` comments are allowed.

Edits are picked up within seconds. A `cover.jpg`/`cover.png` in the sidecar folder
is shown in the album folder if it has no cover of its own.

## Limitations

* Variable-blocksize FLAC images are not split (they are shown as-is).
* Fragmented MP4 files are not retagged (they are passed through).

## License

GPL-3.0-or-later (see `LICENSE`). `src/dst.cpp` is derived from FFmpeg's DST
decoder (LGPL-2.1-or-later), used under the GPL as that license permits.
