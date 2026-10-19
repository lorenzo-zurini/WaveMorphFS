# WaveMorphFS

WaveMorphFS mounts a music archive as the library a music server expects:
one file per track, properly tagged, in the folders you want. The files on
disk are never written to. A CUE sheet with a FLAC image stays exactly as you
ripped it, with its log and checksums still matching, while your server sees
twelve ordinary tracks.

It is a FUSE filesystem for Linux, written in C++. Nothing is converted ahead
of time and no audio is copied. Every track is assembled from the source file
at the moment it is read.

```
On disk                                     In the mount
───────                                     ────────────
Sawallisch - Bruckner 5 (1991)/             Sawallisch - Bruckner 5 (1991)/
  Bruckner 5.cue                              01 - I. Introduktion. Adagio - Allegro.flac
  Bruckner 5.flac        (one 300 MB image)   02 - II. Adagio. Sehr langsam.flac
  Bruckner 5.log                              03 - III. Scherzo. Molto vivace.flac
                                              04 - IV. Finale. Adagio - Allegro moderato.flac
                                              Bruckner 5.log

Karajan - Planets [SACD]/                   Karajan - Planets [SACD]/
  Planets.iso            (DST-compressed)     01 - Mars, the Bringer of War.dsf
                                              02 - Venus, the Bringer of Peace.dsf
                                              ...
```

## Who it is for

- **People who rip to image + CUE.** EAC and XLD rips are best kept whole, but
  Navidrome and most players want separate tracks. WaveMorphFS splits them on
  the fly, sample-exact, without re-encoding a FLAC image.
- **Anyone keeping an archive bit-for-bit.** If you verify rips with
  AccurateRip logs, keep checksums for backups, or share files that must stay
  identical, you can still tag everything with Picard or beets. The tags land in
  small JSON sidecar files instead of the audio files.
- **SACD collectors.** ISO images, including DST-compressed ones, appear as DSF
  tracks a player can stream. No extraction step, no second copy of the disc.
- **Collections in awkward formats.** APE, WavPack, TTA and TAK files are
  served as FLAC, encoded as they are read. 32-bit float WavPack, which FLAC
  cannot hold, is split into WavPack tracks.
- **Classical libraries.** Recordings can be regrouped into folders by work,
  given a cover per work, and retagged freely, all without moving or renaming a
  single source file.

## What it does

| On disk (untouched)                                   | In the mount                                         |
|-------------------------------------------------------|------------------------------------------------------|
| `Album.cue` + `Album.flac`                            | `01 - Title.flac`, `02 - Title.flac`, ...            |
| `Album.cue` + APE, WavPack, TTA, TAK, WAV, AIFF or ALAC image | the same tracks, as FLAC encoded while you listen |
| `Album.cue` + 32-bit float `Album.wv`                 | `01 - Title.wv`, ... built from the image's own blocks |
| `Disc.iso` (SACD, plain DSD or DST)                   | `01 - Title.dsf`, `02 - Title.dsf`, ...              |
| `*.flac`, `*.mp3`, `*.m4a`, `*.dsf`, float `*.wv`     | the same file, with your sidecar tags applied        |
| `Track.ape`, `.wv`, `.tta`, `.tak`                    | `Track.flac`, encoded on the fly                     |
| anything else                                         | passed through as it is                              |
| partly copied files (`*.parts`), dotfiles             | hidden until they are complete                       |

Beyond splitting and converting:

- **Tags from three places.** The image's own tags, the CUE sheet or SACD text,
  and your sidecars, in that order. Tag names are kept exactly as you write
  them, so Picard's `MusicBrainz Album Id`, ffprobe's `album_artist` and plain
  Vorbis names all work and are never renamed.
- **Edit tags on the mount.** Open a track in Picard, Kid3, beets or any other
  tag editor and save. The change is stored in the folder's sidecar and shows up
  at once; the source file is not touched.
- **Reorganize without moving anything.** A sidecar can place a folder, or a
  single track, anywhere in the mount.
- **Per-work cover art.** A sidecar can name the front cover for a folder, a
  track or a file. Large images are scaled to 1024 px once and cached.
- **Several discs in one folder.** Two images in one folder become discs 1 and
  2 (`1-01 - Title.flac`, `2-01 - ...`).
- **Exact, verifiable output.** File sizes are known before any byte is read,
  so seeking and `stat` are exact. Every split FLAC carries the real MD5 of its
  audio, so `flac -t` checks each track end to end.
- **Nothing half-done appears.** Every image is decoded once with all its
  checksums before its tracks are shown, so damaged files and files still being
  copied stay out of the library.

## Quick start with Docker

This is a complete stack: WaveMorphFS mounts your music,
[Navidrome](https://www.navidrome.org/) serves it, and
[beets](https://beets.io/) tags it from MusicBrainz, with every tag beets
writes going into a sidecar. Images for amd64 and arm64 are published as
`ghcr.io/lorenzo-zurini/wavemorphfs` (`:latest` and `:X.Y.Z` for releases,
`:edge` for the main branch).

```
music-stack/
├── docker-compose.yml
├── wavemorph/
│   ├── mnt/        the mount (created by the container)
│   ├── tags/       sidecars: the only data worth backing up
│   ├── cache/      indexes, safe to delete
│   └── status.txt  which images are ready, being processed, or failed
├── navidrome/      Navidrome's database
└── beets/
    └── config.yaml
```

Create the folders first, as the user who will own them:

```sh
mkdir -p music-stack/wavemorph music-stack/navidrome music-stack/beets
```

WaveMorphFS takes care of `wavemorph/` itself, but Navidrome runs as
`user: 1000:1000` and needs `navidrome/` to be writable.

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
# beets/config.yaml: tag files in place on the mount; never move or copy them
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

Then:

```sh
docker compose up -d
docker compose exec beets beet import /wm/mnt/Music
```

Add further libraries (for example `/wm/mnt/Classical Music`) in Navidrome's
web UI under *Libraries*.

The first start takes a while on a large collection: every image is decoded
once to check it, and APE-style images are encoded once to measure their
tracks. The results are cached, so later starts take seconds.

Things worth knowing:

- The host folder holding `wavemorph/` must be on a shared mount for `rshared`
  to work. That is the default on systemd hosts; otherwise run
  `mount --make-rshared /`.
- Bind the folder that contains the mountpoint into other containers, never the
  mountpoint itself. A bind of the mountpoint goes stale ("Transport endpoint is
  not connected") whenever wavemorphfs restarts.
- The container starts as root only to switch to `PUID`/`PGID`. Compose's
  `user:` works too, but only for uid 1000, because `fusermount3` needs a passwd
  entry for the user it runs as.
- The paths inside the container can be changed with `WAVEMORPH_MOUNT`,
  `WAVEMORPH_TAGS_DIR` and `WAVEMORPH_CACHE_DIR`. For `_cover` images, bind
  their folder read-only and point `WAVEMORPH_COVERS_DIR` at it.
  `WAVEMORPH_LOG=debug` logs more.
- When an upgrade changes what generated files contain, their modification
  times move forward, so Navidrome's next regular scan picks the change up.
- Let Navidrome's own scheduled scan do the scanning; don't run `navidrome scan`
  in a second container next to the running server.

## Installing from source

Linux only. You need GCC 13+ or Clang 17+, CMake 3.20+, pkg-config, libfuse3
3.12+, libFLAC, libwavpack 5, FFmpeg 5.1+ (libavformat, libavcodec, libavutil,
libswscale) and nlohmann/json 3.10+. The tests, `verify` and `compare` also use
the `flac`, `metaflac` and `ffmpeg` command-line tools. On Debian or Ubuntu:

```sh
sudo apt install g++ cmake ninja-build pkg-config libfuse3-dev libflac-dev libwavpack-dev \
    nlohmann-json3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev flac ffmpeg fuse3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/wavemorphfs-tests      # unit tests
sudo cmake --install build   # /usr/local/bin/wavemorphfs
```

If other users, such as a music server, should read the mount, enable
`user_allow_other` in `/etc/fuse.conf`. Then run the mount as a service, for
example with systemd:

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

To build the container image yourself, replace `image:` in the compose file
with `build: <path to this repository>`.

## Command line

```sh
# mount: one --root per library, shown as a top-level folder of the mount
wavemorphfs --root "Music=/srv/music" --root "Classical Music=/srv/classical" \
    --tags-dir /srv/wavemorph/tags --cache-dir /srv/wavemorph/cache \
    mount /srv/wavemorph/mnt --allow-other

# the other commands take the same --root/--tags-dir/--cache-dir options and a
# folder inside one of the roots
R="--root Music=/srv/music --tags-dir /srv/wavemorph/tags --cache-dir /srv/wavemorph/cache"
wavemorphfs $R ls /srv/music/Album              # what a folder looks like through the mount
wavemorphfs $R verify /srv/music/Album          # check its tracks with flac/ffmpeg
wavemorphfs $R compare /srv/music/Album <split> # compare its tracks with files split earlier
wavemorphfs $R tags-init /srv/music/Album       # write a sidecar filled with the current tags
```

`wavemorphfs --help` lists every option: worker and FUSE thread counts, the
prescan interval (`--prescan 0` turns the periodic re-walk off) and SACD
multichannel areas. `tags-init` reads the tags of every taggable file (FLAC,
DSF, MP3, M4A, WavPack); with `--force` it replaces an existing sidecar, but it
refuses one that holds `_` settings. `wavemorphfs --version` prints the version.

## Sidecar files

A sidecar is a `wavemorph.json` file. It can sit next to the music, or, to keep
the source folders untouched, in a separate tree:
`<tags-dir>/<root name>/<path relative to the root>/wavemorph.json`. When both
exist they are merged, and the separate tree wins.

```json
{
  "album": {
    "ALBUM": "Symphonie Nr. 5",
    "ALBUMARTIST": ["Bayerisches Staatsorchester", "Wolfgang Sawallisch"],
    "COMMENT": ""
  },
  "track": {
    "3": {"TITLE": "III. Scherzo"},
    "2-05": {"TITLE": "..."}
  },
  "file": {
    "01 - Allegro.flac": {"TITLE": "...", "MusicBrainz Album Id": "..."}
  }
}
```

- `album` applies to every track in the folder, `track` to one track by number
  (`"2-05"` is disc 2, track 5 in a folder with several discs), and `file` to one
  file by name.
- Values are strings or lists of strings. An empty string or list removes the
  tag.
- Names are used exactly as written. They match case-insensitively, and a name
  replaces its synonyms from other conventions: `album_artist` in a sidecar
  replaces the image's `ALBUMARTIST`, and the sidecar's spelling is what gets
  written. In MP3 and M4A output, a name without a standard frame or atom
  becomes a `TXXX` or freeform item under its exact name.
- `"_hide"` hides files. At the top level it takes a name or a list of names in
  the folder: a CUE sheet, an image or an SACD ISO (with all its tracks), or any
  file of the mount. Inside a `track` or `file` entry, `"_hide": true` hides that
  one track or file. Hidden sheets and ISOs don't count as discs.
- Top-level keys that start with `_` are settings (`_hide`, `_target`,
  `_cover`); others, such as `"_comment"`, are ignored. `//` comments are
  allowed, but a sidecar in the tags tree is rewritten without them when tags are
  edited on the mount.
- Changes are picked up within seconds. A `cover.jpg` or `cover.png` in the
  sidecar folder appears in the album folder if the album has no cover of its
  own.

### Organizing the mount

Every folder appears at its own path unless a sidecar says otherwise.
`"_target"` places files elsewhere in the mount:

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

- At the top level it moves the folder's files. Subfolders (`CD1`, `CD2`, ...)
  move with it unless they have a `"_target"` of their own. A root's own folder
  can't be moved; its `"_target"` is ignored with a warning.
- In a `track` or `file` entry it moves that one file, under the same name.
- Paths are folders relative to the mount root, normally starting with a root
  name. Folders are created as needed, folders left empty disappear, and several
  folders may share a target (equal file names get a number). The old and new
  parent folders change their modification time so music servers rescan them.
- Targets are read from the sidecars in the tags tree (`--tags-dir`) and take
  effect within a few seconds. Tag edits on a moved file still go to its own
  folder's sidecar.

### Cover art

`"_cover"` embeds an image as the front cover, replacing a track's own
pictures. It is useful when a server groups tracks from many folders into one
album, for example by work:

```json
{
  "album": {"_cover": "Sibelius/Symphony 5.jpg"},
  "track": {"7": {"_cover": "Sibelius/Finlandia.jpg"}}
}
```

- At the top level or in `album` it covers every track of the folder; in a
  `track` or `file` entry it applies to that one.
- Paths are absolute, or relative to `--covers-dir` (`WAVEMORPH_COVERS_DIR`).
- JPEG or PNG. Images larger than 1024 px on the long edge or 2 MB are scaled to
  1024 px once and kept in the cache, so you can point at full-resolution
  artwork. Smaller images are never enlarged. A missing or unreadable image is
  logged and the track keeps its own pictures.
- FLAC gets a PICTURE block, DSF and MP3 an APIC frame, and WavPack a "Cover Art
  (Front)" item; M4A files are left as they are. The image is read from its file
  when a player reads that part of the track, so covers cost no memory. Replacing
  the image file updates the tracks within seconds.

## Editing tags on the mount

FLAC, DSF, MP3, M4A and WavPack files on the mount can be edited in place with
any tag editor. Writes go to a scratch copy, never to the source. When the
editor closes the file, its tags are read back, compared field by field with
what the mount generated, and only the differences are saved in the folder's
sidecar: under `track."N"` for split tracks (which also pins the file's name,
so retitling a track doesn't rename it), and under `file."<name>"` for regular
files. Names already used in the sidecar keep their spelling, and removed tags
are stored as `""`.

Changes to the audio itself are discarded. Files can't be created, renamed or
deleted. Only the user running WaveMorphFS (and root) may edit; everyone else
gets read access.

## How it works

- **FLAC images are cut without re-encoding.** Every source frame that lies
  inside a track is copied as it is, with only its frame header rewritten
  (track-relative sample number, new CRCs). The partial frames at each track
  boundary are decoded and written out as uncompressed FLAC frames. Because all
  sizes follow from the frame index, `stat` is exact before a byte is read and
  any part of a track can be read directly.
- **Every image is checked before it appears.** The first time an image is
  seen, one pass verifies every frame's checksum and numbering and records where
  each frame starts. The index is cached; the audio never is.
- **APE and similar images can't be cut**, because their frames are seconds
  long and must be whole. Their tracks are served as compressed FLAC: one pass
  decodes the image with FFmpeg (checksums verified) and encodes every frame once
  to learn its size. After that, a read decodes only the packets it needs and
  encodes only those frames. Each frame is encoded independently, so encoding it
  again always gives the same bytes.
- **32-bit float WavPack** is split like FLAC. WavPack blocks are
  self-contained, so blocks inside a track are copied as they are, apart from
  their header and block checksum. Only the samples before a track's first whole
  block and after its last are encoded again, with libwavpack in the image's own
  mode. Tags are written as APEv2.
- **SACD ISOs** are read for their track lists and text, and the stereo area is
  served as DSF (with `--sacd-multichannel`, the multichannel area too, as a
  separate "(Multichannel)" album). Plain DSD only needs reordering;
  DST-compressed areas are decoded in parallel chunks by a port of FFmpeg's DST
  decoder.
- **Regular files keep their audio.** Only the tag area is rebuilt: the
  VORBIS_COMMENT block, the ID3v2.4 tag, the MP4 `ilst` (with chunk offsets
  adjusted), DSF's trailing ID3 tag or WavPack's APEv2 tag. Tags and pictures a
  sidecar doesn't mention are kept.
- **Slow work runs in the background.** Indexing, measuring and SACD scans run
  in worker threads. A folder's tracks appear when they are ready, and its
  modification time changes so music servers notice.

## How it is tested

Everything that produces audio is checked against independent tools:

- Split tracks pass `flac -t` and decode to exactly the source samples. The unit
  tests cover tracks starting 1 or 15 samples into a frame, tracks inside a
  single frame, 8 to 32-bit audio, 1 to 6 channels and block sizes from 576 to
  4608.
- Split tracks are sample-identical to splits made earlier by other tools
  (`wavemorphfs compare`).
- SACD output decodes to the same PCM as a DFF built from the raw disc bytes,
  and is bit-identical to `sacd_extract`-style DSF extractions. The DST decoder
  matches FFmpeg's on synthetic discs.
- WavPack tracks pass `wvunpack -v` and decode to the image's samples.
- Retagged MP3 and M4A files carry the identical audio packets
  (`wavemorphfs verify`).

The test suite also runs under AddressSanitizer and UndefinedBehaviorSanitizer
on every change.

## Troubleshooting

- **An album doesn't appear.** Images are checked before their tracks are shown,
  and files changed in the last minute are left alone until they settle.
  `status.txt` (next to the cache directory) lists every image as ready,
  pending, settling or failed, with the error for failed ones. A failed image
  stays visible as its original files.
- **"Transport endpoint is not connected" in another container.** It bound the
  mountpoint itself instead of the folder containing it.
- **`option allow_other only allowed if 'user_allow_other' is set`.** Add
  `user_allow_other` to `/etc/fuse.conf` (the container image already has it).
- **The mount fails inside a container with "permission denied".** The container
  needs `/dev/fuse`, `CAP_SYS_ADMIN` and an AppArmor setting that allows FUSE
  mounts (`apparmor:unconfined` in the example). `dmesg` shows AppArmor denials.
- **More detail.** `WAVEMORPH_LOG=debug` logs every listing, job and cache
  decision.

## Limitations

- Linux only.
- Variable-blocksize FLAC images can't be cut by copying frames; their tracks
  are encoded on the fly like APE.
- Fragmented MP4 files aren't retagged, and `_cover` leaves M4A files alone.
- Sidecar tags apply to FLAC, MP3, M4A, DSF and WavPack. Ogg, Opus and
  standalone WAV or AIFF files are passed through as they are.
- FUSE can't notify music servers of changes, so they need to rescan on a timer.

## Contributing, security, license

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, testing and the rules the
code keeps, [SECURITY.md](SECURITY.md) for reporting problems privately, and
[CHANGELOG.md](CHANGELOG.md) for what changed in each release.

WaveMorphFS is free software under the GPL-3.0-or-later (see `LICENSE`).
`src/dst.cpp` is derived from FFmpeg's DST decoder (LGPL-2.1-or-later), used
under the GPL as that license permits.
