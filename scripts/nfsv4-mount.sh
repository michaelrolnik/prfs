#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# Smoke-test the nfsv4 plugin over a real Linux NFSv4.0 mount: build a small tree
# (bigtree), serve it with nfsv4.so, `mount -t nfs -o vers=4.0`, and exercise the
# browse/read path (ls, stat, cat via O_DIRECT to bypass the client cache).
#
# NFSv4 is a browse/read first cut, so this is a diagnostic: it prints what works
# and leaves the per-COMPOUND server log for triage. Needs root (mount):
#
#   sudo scripts/nfsv4-mount.sh
#   sudo KEEP=1 scripts/nfsv4-mount.sh   # leave server + mount up afterward
#
# Config via env: PORT, MNT, STORE, PRFS_HOST, NFSV4_SO, BIGTREE_SO, VERS, KEEP.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-20490}"
MNT="${MNT:-/mnt/prfs}"
STORE="${STORE:-/tmp/prfs-v4}"
PRFS_HOST="${PRFS_HOST:-$REPO/build/prfs-host}"
NFSV4_SO="${NFSV4_SO:-$REPO/build/nfsv4.so}"
BIGTREE_SO="${BIGTREE_SO:-$REPO/build/bigtree.so}"
VERS="${VERS:-4.0}"
KEEP="${KEEP:-0}"
HOST_LOG="${HOST_LOG:-/tmp/prfs-v4-host.log}"

die() {
    echo "nfsv4-mount.sh: $*" >&2
    exit 1
}

[ "$(id -u)" -eq 0 ] || die "must run as root (mount needs it): sudo $0"
[ -x "$PRFS_HOST" ] || die "no prfs-host at $PRFS_HOST (meson compile -C build)"
[ -f "$NFSV4_SO" ] || die "no nfsv4.so at $NFSV4_SO"

HOST_PID=
MOUNTED=0
cleanup() {
    if [ "$KEEP" = 1 ]; then
        echo "==> KEEP=1: server (pid ${HOST_PID:-?}) + mount $MNT left up"
        return
    fi
    [ "$MOUNTED" = 1 ] && umount "$MNT" 2>/dev/null
    [ -n "$HOST_PID" ] && kill -INT "$HOST_PID" 2>/dev/null
}
trap cleanup EXIT

# 1. Serve a small tree (bigtree builds it on start, listed before nfsv4).
echo "==> serving nfsv4 on :$PORT (store $STORE)"
plugins=(--plugin "$NFSV4_SO")
sets=()
if [ -f "$BIGTREE_SO" ]; then
    plugins=(--plugin "$BIGTREE_SO" --plugin "$NFSV4_SO")
    sets=(--set bigtree.total=64M --set bigtree.files=4 --set bigtree.dirs=2 --set bigtree.depth=2)
fi
"$PRFS_HOST" --store "$STORE" --clean --port "$PORT" "${plugins[@]}" "${sets[@]}" \
    >"$HOST_LOG" 2>&1 &
HOST_PID=$!
sleep 2
kill -0 "$HOST_PID" 2>/dev/null || {
    echo "prfs-host failed:"
    cat "$HOST_LOG"
    exit 1
}

# 2. Mount NFSv4 (no MOUNT program, no nolock — v4 has integrated locking).
#    Force-clear any stale mount first (a dead prior server leaves the mountpoint
#    unstattable → the new mount fails before it reaches us).
umount -f -l "$MNT" 2>/dev/null || true
mkdir -p "$MNT"
echo "==> mount -t nfs -o vers=$VERS,port=$PORT 127.0.0.1:/ $MNT"
if ! timeout 30 mount -t nfs -o "vers=$VERS,proto=tcp,port=$PORT" 127.0.0.1:/ "$MNT"; then
    echo "!! mount failed. Recent server COMPOUND log:"
    grep -a "nfsv4:" "$HOST_LOG" | tail -30
    exit 1
fi
MOUNTED=1
echo "   mounted OK"

# 3. Exercise the browse/read path.
echo "==> ls -la $MNT"
ls -la "$MNT" 2>&1 | head -20
echo "==> find (tree walk)"
find "$MNT" -maxdepth 3 2>&1 | head -20
echo "==> stat + read the first regular file (O_DIRECT bypasses the page cache)"
f="$(find "$MNT" -type f 2>/dev/null | head -1)"
if [ -n "$f" ]; then
    stat -c '   %n  size=%s  blocks=%b  mtime=%y' "$f"
    echo "   first 32 bytes (generated content):"
    dd if="$f" iflag=direct bs=4096 count=1 2>/dev/null | head -c 32 | xxd | head -2 ||
        dd if="$f" bs=4096 count=1 2>/dev/null | head -c 32 | xxd | head -2
else
    echo "   (no regular file found)"
fi

echo
echo "==> server COMPOUND trace (last 40 lines of $HOST_LOG):"
grep -a "nfsv4:" "$HOST_LOG" | tail -40
echo
echo "==> done. $([ "$KEEP" = 1 ] && echo 'left mounted' || echo 'unmounting + stopping server')"
