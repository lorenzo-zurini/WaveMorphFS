# WaveMorphFS

A read-only FUSE filesystem that shows a music library the way a music server
wants to see it — **one properly tagged file per track** — while the files on disk
stay **byte-for-byte untouched**: rips keep their checksums and logs, and nothing
is duplicated.

| On disk (pristine)                         | What the mount shows                                   |
|--------------------------------------------|--------------------------------------------------------|
| `Album.cue` + `Album.flac` (image rip)     | `01 - Title.flac`, `02 - Title.flac`, …                |
| `Album.cue` + an APE, WavPack, TTA, TAK, WAV, AIFF or ALAC image | same, encoded to FLAC on the fly from the decoded image |
| `Album.cue` + 32-bit float `Album.wv`      | `01 - Title.wv`, … made of the image's own blocks       |
| `Disc.iso` (SACD, plain DSD **or DST**)    | `01 - Title.dsf`, `02 - Title.dsf`, …                  |
| `*.flac`, `*.mp3`, `*.m4a`, `*.dsf`        | the same file with sidecar tags applied                |
| `Track.ape` / `.wv` / `.tta` / `.tak`      | `Track.flac`, encoded on the fly (sidecar tags apply)  |
| 32-bit float `Track.wv`                    | the same file with sidecar tags applied                |
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
* **Non-FLAC images** (APE, WavPack, TTA, TAK, WAV, AIFF, ALAC) cannot be cut without
  re-encoding (APE frames are seconds long and must all be full length), so their
  tracks are served as compressed FLAC encoded on the fly. Nothing is stored but
  positions and sizes: the first time an image is seen it is decoded once with
  checksum verification (FFmpeg's decoders) and every track frame is encoded once
  to learn its size, so file sizes are exact. Reads then decode just the packets
  they need and encode just those frames; FLAC frames are independent, so a frame
  re-encoded later is byte-identical.
* **Floating-point WavPack** (e.g. 32-bit float vinyl rips) is beyond FLAC, so
  those images become WavPack tracks, split like FLAC images: WavPack blocks are
  self-contained, and every block inside a track is copied byte-for-byte except
  its header (track-relative sample numbers) and its block checksum. File-level
  data (the original WAV header, the whole-file MD5) stays out of the tracks.
  Only the samples before a track's first whole block and after its last are
  encoded again, with libwavpack in the image's own mode; their sizes are measured
  once. The image is verified first by decoding it with every block CRC (and its
  MD5, if it has one). Tags are written as APEv2 (`Track`, `Disc`, `Year` and
  `Album Artist` in APEv2 spelling, every other name as given). Standalone float
  `.wv` files keep all their blocks and only get a new APEv2 tag.
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

Linux only (FUSE 3). Needs GCC 13+ or Clang 17+, CMake 3.20+, pkg-config,
libfuse3 3.12+, libFLAC, libwavpack 5, FFmpeg 5.1+ (libavformat, libavcodec,
libavutil, libswscale) and nlohmann/json 3.10+. The tests, `verify` and
`compare` also use the `flac`, `metaflac` and `ffmpeg` tools. On Debian/Ubuntu:

```sh
sudo apt install g++ cmake ninja-build pkg-config libfuse3-dev libflac-dev libwavpack-dev \
    nlohmann-json3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev flac ffmpeg fuse3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/wavemorphfs-tests      # unit tests
sudo cmake --install build   # /usr/local/bin/wavemorphfs
```

To run it without Docker, enable `user_allow_other` in `/etc/fuse.conf` if other
users (e.g. a music server) should read the mount, and run `mount` as a service,
for example with systemd:

```ini
# /etc/systemd/system/wavemorphfs.service
[Unit]
Description=WaveMorphFS
After=local-fs.target

[Service]
User=music
ExecStart=/usr/local/bin/wavemorphfs --root=Music=/srv/music \
    --tags-dir=/srv/wavemorph/tags --cache-dir=/srv/wavemorph/cache \
    mount /srv/wavemorph/mnt --allow-other
ExecStopPost=-/usr/bin/fusermount3 -uz /srv/wavemorph/mnt
Restart=on-failure

[Install]
WantedBy=multi-user.target
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
│   ├── cache/      frame/packet indexes, safe to delete
│   └── status.txt  which images are ready, still being processed, or failed
├── navidrome/      Navidrome's database
└── beets/
    └── config.yaml
```

Create the folders as the user that will own them before the first start
(`mkdir -p wavemorph navidrome beets`); WaveMorphFS takes over `wavemorph/`
itself, but Navidrome runs as `user: 1000:1000` and needs `navidrome/` to be
writable.

```yaml
# docker-compose.yml
services:
  wavemorphfs:
    image: ghcr.io/lorenzo-zurini/wavemorphfs:latest   # amd64 and arm64
    container_name: wavemorphfs
    devices:
      - /dev/fuse
    # only what it needs: the FUSE mount (SYS_ADMIN), switching to PUID/PGID,
    # and handing its own folders to that user on the first start
    cap_drop:
      - ALL
    cap_add:
      - SYS_ADMIN
      - SETUID
      - SETGID
      - CHOWN
      - DAC_OVERRIDE
      - FOWNER
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
  `WAVEMORPH_LOG=debug` logs more. For `"_cover"` images, bind their folder
  read-only and point `WAVEMORPH_COVERS_DIR` at it.
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

# the other commands take the same --root/--tags-dir/--cache-dir options and a
# folder inside one of the roots
R="--root Music=/srv/music --tags-dir /srv/wavemorph/tags --cache-dir /srv/wavemorph/cache"
wavemorphfs $R ls /srv/music/Album              # what a folder looks like through the fs
wavemorphfs $R verify /srv/music/Album          # validate its virtual tracks with flac/ffmpeg
wavemorphfs $R compare /srv/music/Album <split> # PCM-compare virtual tracks with old split files
wavemorphfs $R tags-init /srv/music/Album       # write a sidecar pre-filled with current tags
```

`wavemorphfs --help` lists every option (worker and FUSE thread counts, the
prescan interval — `--prescan 0` turns the periodic re-walk off — and SACD
multichannel areas). `tags-init` reads the current tags of every taggable file
(FLAC, DSF, MP3, M4A, WavPack); with `--force` it replaces an existing sidecar,
but refuses one that holds `_` directives. `--allow-other` needs
`user_allow_other` in `/etc/fuse.conf`. Containers that read the mount should bind
the **parent** of the mountpoint with `bind-propagation=rslave` so they survive
filesystem restarts. `WAVEMORPH_LOG=debug` for more logging.

## Troubleshooting

* **An album does not appear.** Images are verified and indexed before their
  tracks are shown, and files modified in the last minute are left alone until
  they settle. `status.txt` (next to the cache directory) lists every image as
  ready, pending, settling or failed, with the error for failed ones; a failed
  image stays visible as its original files.
* **"Transport endpoint is not connected"** in another container: it bound the
  mountpoint itself instead of its parent folder; see the Docker notes above.
* **`fusermount3: option allow_other only allowed if 'user_allow_other' is set`**:
  add `user_allow_other` to `/etc/fuse.conf` (the container image has it).
* **The mount fails in a container** with "permission denied": the container needs
  `/dev/fuse`, `CAP_SYS_ADMIN`, and an AppArmor profile that allows FUSE mounts
  (`apparmor:unconfined` in the example); `dmesg` shows AppArmor denials.
* **The first scan is slow**: every image is decoded once to verify it (and APE,
  TTA, TAK images are encoded once to measure their tracks). The results are
  cached, so later starts are fast.
* `WAVEMORPH_LOG=debug` logs every listing, job and cache decision.

## Security

WaveMorphFS only reads the music; give it the music read-only. Its own data is the
tags directory (sidecars: back it up) and the cache (safe to delete). The mount is
writable only for tag edits, which become sidecar changes; files on the mount
belong to the user running WaveMorphFS, and only that user (and root) may edit
them. See [SECURITY.md](SECURITY.md) for reporting problems.

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
* Top-level keys starting with `_` are directives (`_hide`, and `_target` and
  `_cover` below); other ones such as `"_comment"` are ignored. `//` comments are
  allowed, but a sidecar in the tags tree is rewritten (without its comments)
  when tags are edited on the mount.

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
  move along beneath it unless they have a `"_target"` of their own. A root's own
  folder cannot be moved (its `"_target"` is ignored with a warning).
* In a `track` or `file` entry it moves that one file (its name stays the same).
* Paths are directories relative to the mount root; their first component is
  normally a root name. Directories are created as needed, folders emptied by
  moves disappear, several folders may share a target (equal file names are
  numbered), and moved folders' new and old parents change mtime so music
  servers rescan them.
* Targets are read from the sidecars in the tags tree (`--tags-dir`) and take
  effect within a few seconds. Tag edits of moved files still go to their own
  folder's sidecar.

### Cover art

`"_cover"` embeds an image as the front cover of tracks, replacing their own
pictures (useful when a music server groups tracks into albums that span many
folders, e.g. by work):

```json
{
  "album": {"_cover": "Sibelius/Symphony 5.jpg"},
  "track": {"7": {"_cover": "Sibelius/Finlandia.jpg"}}
}
```

* At the top level or in `"album"` it applies to every track of the folder; in a
  `track` or `file` entry it overrides that one.
* Paths are absolute or relative to `--covers-dir` (env `WAVEMORPH_COVERS_DIR`).
  JPEG or PNG; images larger than 1024 px on the long edge or 2 MB are scaled
  down to 1024 px once (a JPEG kept in the cache directory), so full-resolution
  artwork can be referenced directly. A missing or unusable image is logged and
  the track keeps its own pictures.
* FLAC (split, encoded and retagged) gets a PICTURE block, DSF and MP3 an APIC
  frame, WavPack a "Cover Art (Front)" item; M4A files are not changed. The image is read from its file whenever a
  player reads that part of a track, so it costs no memory per track. Replacing
  the image file updates the tracks (and their mtime) within seconds.

### Editing tags on the mount

FLAC, DSF, MP3, M4A and WavPack files on the mount can be edited in place with any tag
editor (Picard, Kid3, Mp3tag, mutagen...). The edit never reaches the source file:
writes go to a scratch overlay, and when the editor closes the file the tags are
read back from it, compared field by field with what the mount generated, and
only the differences are stored in the folder's sidecar — `track."N"` for split
tracks (their names change with the title), `file."<name>"` for regular files.
Names already used in the sidecar keep their spelling; removed tags become `""`.
Changes to the audio itself are discarded, and files cannot be created, renamed
or deleted. Generated files carry tag padding, so editors save in place quickly.

## Limitations

* Linux only (FUSE 3).
* Variable-blocksize FLAC images cannot be split by copying frames; their tracks
  are encoded on the fly like non-FLAC images.
* Fragmented MP4 files are not retagged (they are passed through), and `"_cover"`
  leaves M4A files unchanged.
* Sidecar tags apply to FLAC, MP3, M4A, DSF and WavPack; other files (Ogg, Opus,
  standalone WAV/AIFF) are passed through as they are.
* FUSE gives music servers no change notifications: let them rescan on a timer.

## License

GPL-3.0-or-later (see `LICENSE`). `src/dst.cpp` is derived from FFmpeg's DST
decoder (LGPL-2.1-or-later), used under the GPL as that license permits.
