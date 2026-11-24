#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Serve WaveMorphFS over NFS on port $WAVEMORPH_NFS (default 2049); other
# containers get it as an NFS volume, mounted by Docker. Needs no capabilities:
# run it with compose `user:` and `cap_drop: [ALL]`. Started as root instead,
# it runs as PUID:PGID (default 1000:1000).
#
# `entrypoint.sh health` is the image's healthcheck.
set -e

if [ "${1:-}" = health ]; then
    # listening (state 0A) on the port, IPv4 or IPv6
    port=$(printf '%04X' "${WAVEMORPH_NFS##*:}")
    exec grep -q ":$port [0-9A-F:]* 0A" /proc/net/tcp /proc/net/tcp6
fi

drop=
if [ "$(id -u)" = 0 ]; then
    PUID="${PUID:-1000}" PGID="${PGID:-1000}"
    [ "$PUID" != 0 ] || { echo "PUID=0 is not supported: run as an unprivileged user" >&2; exit 1; }
    # compose creates missing bind sources as root: hand our own directories (not
    # their contents, never the music) to the user the filesystem runs as
    for d in "${WAVEMORPH_TAGS_DIR:-}" "${WAVEMORPH_CACHE_DIR:-}"; do
        case "$d" in "" | /) continue ;; esac
        mkdir -p "$d"
        [ "$(stat -c %u "$d")" = "$PUID" ] || chown "$PUID:$PGID" "$d"
    done
    drop="setpriv --reuid=$PUID --regid=$PGID --clear-groups --"
fi

# stops cleanly on SIGTERM (pending tag edits are stored first)
exec $drop wavemorphfs "$@" nfs "$WAVEMORPH_NFS"
