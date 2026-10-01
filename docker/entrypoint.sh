#!/bin/sh
# Mount WaveMorphFS at $WAVEMORPH_MOUNT. The parent of the mountpoint must be
# bind-mounted with rshared propagation so the mount shows up on the host.
set -e
MNT="${WAVEMORPH_MOUNT:?set WAVEMORPH_MOUNT}"
# clear a stale mount left by a previous instance first: stat() on it fails
fusermount3 -uz "$MNT" 2>/dev/null || true
mkdir -p "$MNT"
# lazy unmount on shutdown: a plain unmount fails while readers (Navidrome,
# beets) have files open, which would leave a dead mount behind
trap 'fusermount3 -uz "$MNT" 2>/dev/null' TERM INT
wavemorphfs "$@" mount "$MNT" --allow-other &
pid=$!
wait "$pid"
