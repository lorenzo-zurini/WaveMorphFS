#!/bin/sh
# Mount WaveMorphFS at $WAVEMORPH_MOUNT. The parent of the mountpoint must be
# bind-mounted with rshared propagation so the mount shows up on the host.
set -e
MNT="${WAVEMORPH_MOUNT:?set WAVEMORPH_MOUNT}"
mkdir -p "$MNT"
fusermount3 -uz "$MNT" 2>/dev/null || true   # clear a stale mount from a crash
trap 'fusermount3 -u "$MNT" 2>/dev/null' TERM INT
wavemorphfs "$@" mount "$MNT" --allow-other &
pid=$!
wait "$pid"
