# Changelog

All notable changes to WaveMorphFS. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/). Releases are published as container
images `ghcr.io/lorenzo-zurini/wavemorphfs:<version>`.

Generated files carry an *output epoch*: when a release changes what generated
files contain, their modification times move forward, so music servers re-read
them once on their next scan.

## [0.2.3] - 2026-10-20

### Added
- `"_drop"` and `"_keep"` also cover ID3 frames that are not tags in MP3 and DSF
  files (`PRIV`, `GEOB`, `UFID`, `POPM`, URL frames, ...): by frame ID, or as
  `ID:<owner or description>` (`PRIV:mailto:uits-info@umusic.com`), so
  watermark frames such as UITS can be removed.

## [0.2.2] - 2026-10-20

### Added
- `"_keep"` (whitelist) and `"_drop"` (blacklist) decide which tags are served,
  at every level: the whole library (`<tags-dir>/wavemorph.json`), a root, any
  folder of the tags tree and everything below it, a folder's `"album"`, a track,
  a file. Names match case-insensitively and across synonyms, with `*` wildcards.
  The most specific level decides; a value set in a sidecar beats a less specific
  drop. Tags these lists remove are not stored when a tagger writes them through
  the mount, so sidecars stay clean.

## [0.2.1] - 2026-10-19

### Added
- Cover art can be set on the mount: a front cover embedded with a tag editor
  is stored in the tags tree (`.covers/`, named by its content) and becomes the
  track's `_cover`; removing the picture removes it. FLAC, DSF, MP3 and WavPack.
- `_cover` paths starting with `./` are relative to the folder's sidecar in the
  tags tree.

## [0.2.0] - 2026-10-19

A release-readiness pass: every part of the code was reviewed, and the bugs,
duplication and rough edges found were fixed. Generated files change for the
cases listed below, so their modification times move forward once.

### Added
- 32-bit floating-point WavPack images (which FLAC cannot hold) are split into
  WavPack tracks made of the image's own blocks; only each track's first block
  and a partial last block are encoded again. Standalone float `.wv` files get
  sidecar tags as APEv2. Tags on WavPack files can be edited on the mount.
- `wavemorphfs --version`; the mount logs its version when it starts.
- `--prescan 0` turns the periodic re-walk off. `tags-init` reads DSF, MP3, M4A
  and WavPack files too.
- Build options `WAVEMORPH_WERROR` and `WAVEMORPH_SANITIZE`; CI runs the test
  suite under AddressSanitizer and UndefinedBehaviorSanitizer, and tags create
  GitHub releases with notes from this changelog.
- Tests for SACD parsing, DSF output and the DST decoder (on synthetic discs,
  checked against FFmpeg), the FUSE layer, listings, sidecar edits and the tag
  parsers (including malformed input): 40 tests became more than 100.

### Changed
- The mount enforces file permissions (`default_permissions`): only the user
  running WaveMorphFS (and root) can edit tags through it.
- The container image ships only the libraries the filesystem links, not the
  `ffmpeg` and `flac` tools (540 MB → 246 MB); `verify` and `compare` need them,
  so run those from a source build. The documented compose setup drops every
  capability the container does not need.
- The entrypoint gives the mount, tags and cache folders to `PUID`/`PGID` when
  Docker Compose created them as root, and stops the filesystem cleanly.
- One table drives tag-name synonyms and the ID3/MP4 frame mapping. Names from
  other conventions (`aART`, `DISK`, `trkn`, `SORT_*`, ...) reach the right
  frame or atom, and fields that were written twice under two names are written
  once. Names keep the spelling given.
- Lyrics in several languages are kept side by side in MP3s.
- Background jobs that fail are remembered per file version instead of being
  retried on every listing, and the image stays visible as it is; failures are
  listed in `status.txt`.
- Edits on the mount are visible immediately instead of after about 2 s.
- SACD DST decoding is faster, and readers no longer cancel each other's
  read-ahead.

### Fixed
- Reads of freed memory in path lookups and write sessions (seen as occasional
  `std::bad_alloc` errors), and exceptions that could end the mount.
- Sidecar track keys spelled differently (`"05"` / `"5"`, `"2-5"` / `"2-05"`)
  were separate tables, so edits on the mount could be lost or hidden.
- Editing a multichannel SACD track renamed its stereo twin.
- A corrupt M4A box size could hang a folder listing; malformed ID3, MP4, APEv2,
  DSF, SACD and CUE data is bounds-checked throughout.
- A sidecar with a syntax error no longer drops the folder's other sidecar and
  pinned names (the previous version is used until it is fixed).
- Files copied into the library are re-listed once they stop changing, so a
  half-copied file's size is not kept.
- Sidecar writes are durable (synced before they replace the old file).
- MP3s with unsynchronised ID3v2.4 tags get sidecar covers; ID3v2.2 tags and
  iTunes-style frame sizes are read; MP3 `TXXX` and M4A freeform items are
  replaced under any spelling of their name.
- 24 SACDs carried an empty `CATALOGNUMBER` tag; it is now left out. A disc with
  only a multichannel area is listed once.
- FLAC images whose trailing tag happened to look like a frame ending lost their
  last track; very short tracks could get a block shorter than FLAC allows.
- Covers are never enlarged, and JPEG sizes are found past large EXIF/ICC data.
- UTF-16 CUE sheets with a damaged character, CR-only CUE sheets, and invalid
  UTF-8 in tags are handled.
- Disc numbers, names that collide, emptied folders on the mount, root-folder
  targets and mtimes that could go backwards are handled consistently.

## [0.1.9] - 2026-10-07

### Added
- Sidecar `"_cover"`: embed a chosen image as the front cover of a folder's
  tracks, of one track or of one file; large images are scaled to 1024 px once
  and kept in the cache. `--covers-dir` resolves relative paths.

## [0.1.8] - 2026-10-07

### Added
- Standalone APE, WavPack, TTA and TAK tracks are served as FLAC encoded on the
  fly, tagged through `file."<name>.flac"`.

## [0.1.7] - 2026-10-03

### Fixed
- Placements keep only the targets of tags-tree sidecars, and mount listings
  expire, bringing idle memory back to its previous level.

## [0.1.6] - 2026-10-03

### Added
- Sidecar `"_target"`: place folders or single files anywhere in the mount
  without moving source files.

## [0.1.5] - 2026-10-02

### Fixed
- Encoded tracks take the frames a read needs before read-ahead can evict them.

## [0.1.4] - 2026-10-02

### Added
- Sidecar `"_hide"`.

### Fixed
- An image described by several CUE sheets with the same tracks is listed once.

## [0.1.3] - 2026-10-02

### Added
- Regular DSF files get sidecar tags.

### Changed
- Cover art and tag padding are referenced instead of held in memory, and
  indexes are mapped from the cache, lowering idle memory.

## [0.1.2] - 2026-10-02

### Changed
- Decoded audio of files nobody is reading is released.

## [0.1.1] - 2026-10-02

### Fixed
- Explicit removals in sidecars (`"KEY": ""`) survive layering and apply to
  regular files.

## [0.1.0] - 2026-10-01

First release: FLAC images split sample-exactly without re-encoding, other
lossless images (APE, WavPack, TTA, TAK, WAV, ALAC) encoded to FLAC on the fly,
SACD ISOs (plain DSD and DST) as DSF, sidecar tags for every file kind, tag edits
on the mount stored in sidecars, and a container image for amd64 and arm64.

[0.2.3]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.9...v0.2.0
[0.1.9]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.8...v0.1.9
[0.1.8]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.7...v0.1.8
[0.1.7]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.6...v0.1.7
[0.1.6]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.5...v0.1.6
[0.1.5]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/lorenzo-zurini/WaveMorphFS/releases/tag/v0.1.0
