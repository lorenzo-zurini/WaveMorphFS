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
  ISOs, CUE sheets, images). Malformed input must never crash the server or make
  it read out of bounds; please report any file that does.
- The NFS server parses requests from the network. Malformed or oversized
  requests must never crash it; it closes such connections.
- It needs no privileges: the container runs as an ordinary user with every
  capability dropped. The kernel's NFS client on the host mounts the share.
- The share is writable for tag edits only: writes are captured and stored as
  sidecar changes. Everyone who can connect can read; only the user running
  WaveMorphFS (and root) can edit. NFS version 3 trusts the user a client
  claims, so the server accepts only connections from reserved ports (which
  only the kernel and root can use), and the documented setup puts it on a
  Docker network of its own. `--insecure` lifts the port check.
