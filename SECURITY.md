# Security

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](https://github.com/lorenzo-zurini/WaveMorphFS/security/advisories/new)
rather than in a public issue. Include the version (`wavemorphfs --version`),
how the filesystem is run, and the steps or input that trigger the problem.

## Trust model

- WaveMorphFS only ever *reads* the music it serves; give it the music read-only
  (the documented Docker setup binds it `:ro`).
- Its own data is the tags directory (sidecars, the only data worth backing up)
  and the cache directory (indexes, safe to delete).
- Files are parsed from the library (FLAC, APE, WavPack, MP3, M4A, DSF, SACD
  ISOs, CUE sheets, images). Malformed input must never crash the mount or make
  it read out of bounds; please report any file that does.
- The mount is writable for tag edits only: writes are captured and stored as
  sidecar changes. Access follows the file modes shown on the mount (owner =
  the user running WaveMorphFS); with `--allow-other`, other users can read but
  only that owner (and root) can edit.
- The container needs `/dev/fuse` and `CAP_SYS_ADMIN` to mount; it drops to
  `PUID`/`PGID` before starting the filesystem.
