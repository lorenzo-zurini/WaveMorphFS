# Changelog

All notable changes to WaveMorphFS. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/). Releases are published as container
images `ghcr.io/lorenzo-zurini/wavemorphfs:<version>`.

Generated files carry an *output epoch*: when a release changes what generated
files contain, their modification times move forward, so music servers re-read
them once on their next scan.

## [0.2.0] - unreleased

### Added
- 32-bit floating-point WavPack images (which FLAC cannot hold) are split into
  WavPack tracks made of the image's own blocks; only each track's first block
  and a partial last block are encoded again. Standalone float `.wv` files get
  sidecar tags as APEv2. Tags on WavPack files can be edited on the mount.
- `wavemorphfs --version`; the mount logs its version when it starts.
- Build options `WAVEMORPH_WERROR` and `WAVEMORPH_SANITIZE`; CI runs the test
  suite under AddressSanitizer and UndefinedBehaviorSanitizer.
- GitHub releases with notes from this changelog.

### Changed
- The container image no longer ships the `ffmpeg` and `flac` tools (only the
  libraries the filesystem links), which more than halves its size.
  `verify` and `compare` need those tools; run them from a source build.
- The entrypoint gives the mount, tags and cache directories to `PUID`/`PGID`
  when Docker Compose created them as root, and stops the filesystem cleanly.

### Fixed
- A race between path lookups and directory listing rebuilds could read freed
  memory (seen as occasional `std::bad_alloc` errors on `getattr`).
- MP3 files whose ID3v2.4 tag is flagged as unsynchronised now get sidecar
  covers, and their UTF-16 text is read correctly.

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

[0.2.0]: https://github.com/lorenzo-zurini/WaveMorphFS/compare/v0.1.9...HEAD
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
