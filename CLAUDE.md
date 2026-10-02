# WaveMorphFS — notes for Claude sessions

Read-only-source FUSE filesystem (C++20, GPL-3.0-or-later) that presents pristine
music files (CUE images, APE, SACD ISOs, which must stay byte-identical) as one
tagged file per track for music servers. See README.md for the user-facing picture; this file is the
developer briefing.

## Non-negotiable invariants

1. **Never write to source files.** Source files stay byte-identical (the owner
   verifies them by checksum).
   Everything the user changes goes into sidecars (`wavemorph.json`).
2. **Never cache audio.** Only positions/sizes/MD5s are cached. Audio is always
   read (or decoded/encoded) from the source on the fly. A previous version kept
   16 GB of converted APE copies; the owner rejected that.
3. **Never rename or normalise tag names.** Names are kept exactly as found in
   source files and sidecars (`MusicBrainz Album Id`, `album_artist`, ...). Match
   case-insensitively and treat synonyms as one field (`canonical_field()` in
   tags.cpp), but always emit the spelling that was given. The owner was emphatic:
   "dont change the tag names, that will break them — fix the engine".
4. **Output must be exact and verifiable.** Sizes are known before any byte is
   produced (stat must be exact); audio must be sample/bit-identical to the
   source. Every change touching audio gets checked against reference tools.
5. **Generated output changes ⇒ bump `OUTPUT_EPOCH_NS`** in library.cpp so music
   servers (which only rescan files whose mtime changed) re-read the files.

## Layout

```
src/
  main.cpp       CLI: mount, ls, verify, compare, tags-init (hand-rolled parser)
  fs.cpp         libfuse3 high-level API; path -> Library listing; writable mount (edits -> sidecars)
  library.cpp    per-directory listing rules, background jobs, status.txt, apply_edit(), OUTPUT_EPOCH_NS
  flac.cpp       FLAC primitives: headers/CRCs, frame index, VERBATIM + libFLAC frame encode, libFLAC decode
  track.cpp      FlacTrack: split track of a fixed-blocksize FLAC image (frames copied, headers rewritten)
  avimage.cpp    AvImage: non-FLAC images (APE/WV/TTA/...) decoded with libav; packet index; parallel read-ahead
  encoded.cpp    EncodedTrack: tracks of an AvImage as compressed FLAC encoded per frame on the fly (layout index)
  sacd.cpp       SACD ISO parsing, DSF tracks, chunked parallel DST decode
  dst.cpp        DST decoder (port of FFmpeg dstdec.c, LGPL-2.1+ used under GPL)
  id3.cpp mp4.cpp  ID3v2.4 writer/reader (DSF + MP3 retag), MP4 ilst rewrite/reader (stco/co64 shift)
  retag.cpp      regular FLAC/MP3/M4A/DSF with sidecar tags (Spliced VFile)
  writeback.cpp  tag edits on the mount: WriteSession overlay, read_file_tags(), tag_changes() diff
  sidecar.cpp    wavemorph.json (nlohmann ordered_json, // comments allowed, "_name" pins track file names)
  tags.cpp       Tags model (case-insensitive keys keeping spelling), synonyms, cue mapping, sanitize_name
  cue.cpp charset.cpp  CUE parsing; charset detection via iconv + script/language scoring (no chardetng)
  cache.cpp      on-disk index cache names = md5(path\nsize\nmtime\nextra)[:16]
  util.cpp       errors (wm::Error, WM_ENSURE, with_context), File/pread, atomic_write, logging, run(), worker pools
tests/           tiny in-tree framework (TEST/CHECK); test_flac (splitting vs the flac tool), formats, tags
docker/entrypoint.sh, Dockerfile (Debian trixie, builds + runs tests; PUID/PGID, /wavemorph defaults)
.github/workflows/docker.yml  multi-arch (native amd64+arm64) image -> ghcr.io/<owner>/wavemorphfs
```

Key types: `VFile` (size + read_at + describe), `Entry` in a `Listing` (name,
file, mtime, tag_section/tag_key/tag_ext for edit routing).

## How things work (short)

- **FLAC images** (fixed blocksize): one CRC-verified pass builds a frame index
  (also proves the file is complete). Tracks copy inner frames with headers
  rewritten to variable-blocksize sample numbers; partial boundary frames are
  decoded and re-emitted as VERBATIM. Per-track STREAMINFO MD5 computed by a
  background job (md5/ cache).
- **Non-FLAC images** (APE, ...) and variable-blocksize FLAC: AvImage verifies by
  a full decode with checksums (`AV_EF_CRCCHECK|EXPLODE`), records a packet index;
  `build_layout` encodes every track frame once (libFLAC level 5, loose mid-side
  OFF so frames are independent/deterministic) to record sizes + track MD5.
  Reads re-encode only the frames needed; each must match its recorded size.
- **SACD**: master/area TOC, one scan records where each 1/75 s frame starts
  (cache .sacdidx). DSF output = byte de-interleave + bit reversal; DST decoded in
  8-frame chunks in parallel. Multichannel area optional (`--sacd-multichannel`).
  A final frame cut short by a broken last sector is padded with 0x69.
- **Sidecars**: `<tags-dir>/<root name>/<relative dir>/wavemorph.json` (+ optional
  in-source one). Sections `album`, `track` ("N" or "D-NN"), `file` (by name).
- **Writable mount**: writes go to a sparse anonymous scratch file; on
  flush/release tags are read back (Vorbis/ID3/MP4 → names), diffed against the
  generated file (`tag_changes`: synonyms one field, placeholder zeros and a YEAR
  echoing DATE ignored, track numbers compared numerically) and stored by
  `Library::apply_edit` (track."N" for split tracks + `_name` pin, file."<name>"
  for regular files). Create/rename/delete → EROFS.
- **Concurrency**: two worker lanes (`submit(task, Lane::Decode|Encode)`). Decode
  jobs never wait on other jobs; encode jobs may wait only on decode jobs. The
  item a reader needs right now is produced in the reader's thread; pools only do
  read-ahead, and read-ahead starts only for sequential readers. Keep this rule or
  you get deadlocks. Pools are never destroyed (destroying a condvar with waiters
  blocks exit).
- **FUSE**: 1 MiB max_write/readahead, `auto_cache`, attr/entry TTL 1 s,
  direct_io for write sessions. Path resolution: intermediate components are real
  dirs; only the last needs the parent's listing.

## Build / test

```sh
cmake -S . -B build -G Ninja && cmake --build build -j
build/wavemorphfs-tests            # all; or a substring filter: build/wavemorphfs-tests decoded
```
Deps: libfuse3, libFLAC, libavformat/libavcodec/libavutil, nlohmann-json; `flac`
and `ffmpeg` binaries for tests/verify. Warnings must stay at zero.

## Verifying changes (do this, not just unit tests)

- `wavemorphfs ls <dir>` / `verify <dir>` (flac -t / ffmpeg decode / stream MD5)
  / `compare <dir> <split dir>` (PCM MD5 vs old split files).
- For anything touching output bytes: mount the new build on a scratch mountpoint
  (with `--tags-dir`/`--cache-dir` pointing at the real ones, or copies if the
  test edits) and byte-compare whole files **and** random reads against the
  production mount. Mutagen (python) is available for tag-editor tests.
- Performance: benchmark sequential MB/s, first-byte and fresh-open random 64K
  latency per file kind (passthrough, retagged, flac-image, encoded APE, DSF plain,
  DSF DST) on a scratch mount; compare before/after. Baseline after the perf
  pass: APE ~100 MB/s, DST ~70 MB/s (CPU-bound, all cores), plain DSF ~1.6 GB/s.

## Releases

The image is published by CI only: pushes to main -> `:edge`, tags `vX.Y.Z` ->
`:X.Y.Z`, `:X.Y`, `:latest`. The README's compose example is the documented
setup; keep it in sync with the entrypoint (PUID/PGID, WAVEMORPH_* env) and
free of host-specific names or paths.

## Conventions

- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Match surrounding style; comments explain why. Errors via `fail(...)`,
  `WM_ENSURE(...)`, `with_context("...", [&]{...})`.
- Commit messages: summary line + explanatory body; end with the attribution lines
  your harness gives you. Remote: GitHub (public). Deployment is separate from the
  repo (see your private memory notes).
- Never `pkill -f <pattern>` / `pgrep -f` loops whose pattern appears in your own
  shell command line — it kills your own shell (exit 144). Use PIDs or `[x]yz`
  bracket patterns.
