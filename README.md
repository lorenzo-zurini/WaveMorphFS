# WaveMorphFS

A read-only FUSE filesystem that shows a music library the way a music server
wants to see it — **one properly tagged file per track** — while the files on disk
stay **byte-for-byte untouched**: rips keep their checksums and logs, and nothing
is duplicated.

| On disk (pristine)                         | What the mount shows                                   |
|--------------------------------------------|--------------------------------------------------------|
| `Album.cue` + `Album.flac` (image rip)     | `01 - Title.flac`, `02 - Title.flac`, …                |
| `Album.cue` + `Album.ape` / `.wv` / `.wav` | same, encoded to FLAC on the fly from the decoded image |
| `Disc.iso` (SACD, plain DSD **or DST**)    | `01 - Title.dsf`, `02 - Title.dsf`, …                  |
| `*.flac`, `*.mp3`, `*.m4a`, `*.dsf`        | the same file with sidecar tags applied                |
| any other file                             | passed through untouched                               |
| incomplete files (`*.parts`), dotfiles     | hidden                                                 |

Tags come from the image itself, the CUE sheet / SACD text, and **sidecar files**
you control — never written into the source files.

## How it works

* **CUE + FLAC images are split sample-exactly without re-encoding.** Every source
  frame that lies inside a track is copied byte-for-byte; only its frame header is
  rewritten (variable-blocksize, track-relative sample number, new CRC-8/CRC-16).
  The partial frames at the two track boundaries are decoded and re-emitted as
  VERBATIM frames. Output sizes are computable up front, so `stat` is exact and
  random access is cheap.
* **Several images in one folder** become discs (`1-01 - Title.flac`, …). Several
  CUE sheets describing the same image with the same tracks (e.g. `X.flac.cue` and
  `X.wav.cue`) count once: the sheet whose FILE line names the image is used, else
  the first by name.
* **Images are fully verified before they appear.** The first time an image is
  seen, one sequential pass checks every frame's CRC and numbering and records its
  offset (cached on disk). This also keeps files that are still being copied or
  are damaged out of the library.
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
* **Regular FLAC, MP3, M4A and DSF files** get sidecar tags without touching the
  audio: only the tag area is rebuilt (VORBIS_COMMENT; ID3v2.4; the MP4 `ilst`, with
  chunk offsets adjusted; DSF's trailing ID3v2.4 tag). Tags and pictures the sidecar
  does not name are kept as is.
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

The `Dockerfile` builds and tests it on Debian and runs the mount in a container;
see [Running with Docker](#running-with-docker). Images for amd64 and arm64 are
published as `ghcr.io/lorenzo-zurini/wavemorphfs`: `:latest` and `:X.Y.Z` for
releases, `:edge` for the main branch. To build your own, replace `image:` in
the compose file with `build: <path to this repository>`.

## Running with Docker

A complete setup as it runs in production: WaveMorphFS mounts the music folders,
[Navidrome](https://www.navidrome.org/) serves the mount, and
[beets](https://beets.io/) tags it from MusicBrainz — every tag beets writes lands
in a sidecar, so the source files are never touched.

```
music-stack/
├── docker-compose.yml
├── wavemorph/
│   ├── mnt/        the mount (created by the container)
│   ├── tags/       sidecars — the only precious data, back it up
│   └── cache/      frame/packet indexes, safe to delete
├── navidrome/      Navidrome's database
└── beets/
    └── config.yaml
```

```yaml
# docker-compose.yml
services:
  wavemorphfs:
    image: ghcr.io/lorenzo-zurini/wavemorphfs:latest   # amd64 and arm64
    container_name: wavemorphfs
    devices:
      - /dev/fuse
    cap_add:
      - SYS_ADMIN
    security_opt:
      - apparmor:unconfined       # AppArmor blocks FUSE mounts in containers
    environment:
      # the user the filesystem runs as: owner of wavemorph/, able to read the music
      - PUID=1000
      - PGID=1000
      # also expose SACD multichannel areas as "(Multichannel)" albums
      - WAVEMORPH_SACD_MULTICHANNEL=false
    # one --root per library: NAME=PATH shows PATH as folder NAME of the mount
    command:
      - --root=Music=/music
      - --root=Classical Music=/classical
    volumes:
      # the music: read-only, WaveMorphFS never writes to it
      - /path/to/music:/music:ro
      - /path/to/classical:/classical:ro
      # mount (mnt/), sidecars (tags/) and indexes (cache/); rshared: the FUSE
      # mount made in here propagates back to the host
      - type: bind
        source: ./wavemorph
        target: /wavemorph
        bind:
          propagation: rshared
    restart: unless-stopped

  navidrome:
    image: deluan/navidrome:latest
    user: 1000:1000
    environment:
      # the image sets ND_MUSICFOLDER=/music, which overrides navidrome.toml
      - ND_MUSICFOLDER=/wm/mnt/Music
      # FUSE gives no change notifications: rescan on a timer
      - ND_SCANNER_SCHEDULE=@every 1h
    ports:
      - 4533:4533
    depends_on:
      wavemorphfs:
        condition: service_healthy
    volumes:
      - ./navidrome:/data
      # bind the PARENT of the mountpoint with rslave, not the mountpoint itself:
      # that way the mount reappears after wavemorphfs restarts
      - type: bind
        source: ./wavemorph
        target: /wm
        read_only: true
        bind:
          propagation: rslave
    restart: unless-stopped

  beets:
    image: lscr.io/linuxserver/beets:latest
    environment:
      - PUID=1000
      - PGID=1000
      - TZ=Etc/UTC
    depends_on:
      wavemorphfs:
        condition: service_healthy
    volumes:
      - ./beets:/config
      # writable: tag writes on the mount are stored as sidecars
      - type: bind
        source: ./wavemorph
        target: /wm
        bind:
          propagation: rslave
    restart: unless-stopped
```

```yaml
# beets/config.yaml — tag files in place on the mount; never move or copy them
# (their names are fixed by the mount)
directory: /wm/mnt
library: /config/library.db
plugins: musicbrainz
import:
  copy: no
  move: no
  write: yes
  incremental: yes
  log: /config/import.log
```

Then `docker compose up -d`, import with
`docker compose exec beets beet import /wm/mnt/Music`, and add further libraries
(e.g. `/wm/mnt/Classical Music`) in Navidrome's web UI under *Libraries*.

Notes:

* The host directory holding `wavemorph/` must be on a shared mount for `rshared`
  to work (the default on systemd hosts; otherwise `mount --make-rshared /`).
* The container starts as root only to switch to `PUID`/`PGID`. Running it with
  compose's `user:` instead works for uid 1000 only (`fusermount3` needs a passwd
  entry for the user it runs as).
* The image is healthy once the mount is up, which is what `depends_on:
  condition: service_healthy` waits for. Its paths can be changed with
  `WAVEMORPH_MOUNT`, `WAVEMORPH_TAGS_DIR` and `WAVEMORPH_CACHE_DIR`;
  `WAVEMORPH_LOG=debug` logs more.
* Only the folder of the mountpoint is bound into the other containers, never the
  mountpoint itself — a bind of the mountpoint goes stale
  ("Transport endpoint is not connected") when wavemorphfs restarts.
* After upgrading to a version that changes generated files, their mtimes change,
  so Navidrome's regular scan picks the new contents up by itself.
* Don't run `navidrome scan` in a second container or next to the running server;
  let the server's own scan do it.

## Usage

```sh
# mount: one --root per library, shown as a top-level folder of the mount
wavemorphfs --root "Music=/srv/music" --root "Classical Music=/srv/classical" \
    --tags-dir /srv/wavemorph/tags --cache-dir /srv/wavemorph/cache \
    mount /srv/wavemorph/mnt --allow-other

wavemorphfs ls <source dir>                  # what a folder looks like through the fs
wavemorphfs verify <source dir>              # validate its virtual tracks with flac/ffmpeg
wavemorphfs compare <source dir> <split dir> # PCM-compare virtual tracks with old split files
wavemorphfs tags-init <source dir>           # write a sidecar pre-filled with current tags
```

`--allow-other` needs `user_allow_other` in `/etc/fuse.conf`. Containers that read
the mount should bind the **parent** of the mountpoint with `bind-propagation=rslave`
so they survive filesystem restarts. `WAVEMORPH_LOG=debug` for more logging.

## Sidecar tags

`wavemorph.json`, either next to the files or (to keep the source folders untouched) in a
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
* `"_hide"` hides files: at the top level a name or list of names of the folder —
  a CUE sheet, image or SACD ISO (with all its tracks), or any file of the mount —
  and `"_hide": true` inside a `track` or `file` entry hides that one track or file.
  Hidden sheets and ISOs do not count as discs, so the rest is numbered as if they
  were not there.
* Other top-level keys (e.g. `"_comment"`) are ignored; `//` comments are allowed.

Edits are picked up within seconds. A `cover.jpg`/`cover.png` in the sidecar folder
is shown in the album folder if it has no cover of its own.

### Organizing the mount

By default every folder appears at its own path. `"_target"` places files somewhere
else in the mount, so the library can be organized without moving a single source
file:

```json
{
  "_target": "Classical Music/Gustav Mahler/Symphony No. 5 (Bernstein, 1987)",
  "track": {
    "7": {"_target": "Classical Music/Gustav Mahler/Encores"}
  },
  "file": {
    "booklet.pdf": {"_target": "Classical Music/Gustav Mahler/Booklets"}
  }
}
```

* At the top level it moves the folder's files; its subfolders (`CD1`, `CD2`, ...)
  move along beneath it unless they have a `"_target"` of their own.
* In a `track` or `file` entry it moves that one file (its name stays the same).
* Paths are directories relative to the mount root; their first component is
  normally a root name. Directories are created as needed, folders emptied by
  moves disappear, several folders may share a target (equal file names are
  numbered), and moved folders' new and old parents change mtime so music
  servers rescan them.
* Targets are read from the sidecars in the tags tree (`--tags-dir`) and take
  effect within a few seconds. Tag edits of moved files still go to their own
  folder's sidecar.

### Editing tags on the mount

FLAC, DSF, MP3 and M4A files on the mount can be edited in place with any tag
editor (Picard, Kid3, Mp3tag, mutagen...). The edit never reaches the source file:
writes go to a scratch overlay, and when the editor closes the file the tags are
read back from it, compared field by field with what the mount generated, and
only the differences are stored in the folder's sidecar — `track."N"` for split
tracks (their names change with the title), `file."<name>"` for regular files.
Names already used in the sidecar keep their spelling; removed tags become `""`.
Changes to the audio itself are discarded, and files cannot be created, renamed
or deleted. Generated files carry tag padding, so editors save in place quickly.

## Limitations

* Variable-blocksize FLAC images cannot be split by copying frames; their tracks
  are encoded on the fly like non-FLAC images.
* Fragmented MP4 files are not retagged (they are passed through).

## License

GPL-3.0-or-later (see `LICENSE`). `src/dst.cpp` is derived from FFmpeg's DST
decoder (LGPL-2.1-or-later), used under the GPL as that license permits.
