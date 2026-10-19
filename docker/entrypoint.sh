#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Mount WaveMorphFS at $WAVEMORPH_MOUNT. The parent of the mountpoint must be
# bind-mounted with rshared propagation so the mount shows up on the host.
#
# Started as root, it runs the filesystem as PUID:PGID (default 1000:1000);
# started as another user (compose `user:`), it runs as that user, which then
# needs a passwd entry (fusermount3 refuses users without one).
set -e
MNT="${WAVEMORPH_MOUNT:?set WAVEMORPH_MOUNT}"
# clear a stale mount left by a previous instance first: stat() on it fails
fusermount3 -uz "$MNT" 2>/dev/null || true

if [ "$(id -u)" = 0 ]; then
    PUID="${PUID:-1000}" PGID="${PGID:-1000}"
    [ "$PUID" != 0 ] || { echo "PUID=0 is not supported: run as an unprivileged user" >&2; exit 1; }
    groupmod -o -g "$PGID" wavemorph
    usermod -o -u "$PUID" -g "$PGID" wavemorph 2>/dev/null || true  # "no changes" when they already match
    as_user() { setpriv --reuid="$PUID" --regid="$PGID" --init-groups -- "$@"; }
    # compose creates missing bind sources as root: hand our own directories (not
    # their contents, never the music) to the user the filesystem runs as
    for d in "$(dirname "$MNT")" "$MNT" "${WAVEMORPH_TAGS_DIR:-}" "${WAVEMORPH_CACHE_DIR:-}"; do
        case "$d" in "" | /) continue ;; esac
        mkdir -p "$d"
        [ "$(stat -c %u "$d")" = "$PUID" ] || chown "$PUID:$PGID" "$d"
    done
else
    getent passwd "$(id -u)" >/dev/null || {
        echo "uid $(id -u) has no passwd entry: run the container as root with PUID/PGID instead of user:" >&2
        exit 1
    }
    as_user() { "$@"; }
fi

# fusermount3 wants the mountpoint writable by the user that mounts it
as_user mkdir -p "$MNT"
# lazy unmount on shutdown: a plain unmount fails while readers (Navidrome,
# beets) have files open, which would leave a dead mount behind
trap 'fusermount3 -uz "$MNT" 2>/dev/null; kill -TERM "$pid" 2>/dev/null' TERM INT
as_user wavemorphfs "$@" mount "$MNT" --allow-other &
pid=$!
# a trapped signal interrupts wait: then wait again for the filesystem to exit
set +e
wait "$pid"
status=$?
if [ "$status" -gt 128 ] && kill -0 "$pid" 2>/dev/null; then
    wait "$pid"
    status=$?
fi
exit "$status"
