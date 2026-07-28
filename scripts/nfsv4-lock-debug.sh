#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# Diagnose why the Linux NFSv4.0 client won't drive byte-range locks against the
# nfsv4 plugin. Serves prfs, mounts vers=4.0, turns on the NFS-client rpcdebug
# trace, clears the kernel ring buffer, attempts one fcntl F_SETLK, then dumps
# the kernel log — which shows what the client's state manager did and the exact
# reason it returns ENOLCK (e.g. callback path, clientid, capability). Needs root:
#
#   sudo scripts/nfsv4-lock-debug.sh
#
# Config via env: PORT, MNT, STORE, PRFS_HOST, NFSV4_SO, KEEP.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-20490}"
MNT="${MNT:-/mnt/prfs}"
STORE="${STORE:-/tmp/prfs-v4}"
PRFS_HOST="${PRFS_HOST:-$REPO/build/prfs-host}"
NFSV4_SO="${NFSV4_SO:-$REPO/build/nfsv4.so}"
KEEP="${KEEP:-0}"
HOST_LOG="${HOST_LOG:-/tmp/prfs-v4-host.log}"

die() {
    echo "nfsv4-lock-debug.sh: $*" >&2
    exit 1
}
[ "$(id -u)" -eq 0 ] || die "must run as root: sudo $0"
[ -x "$PRFS_HOST" ] || die "no prfs-host at $PRFS_HOST (meson compile -C build)"
[ -f "$NFSV4_SO" ] || die "no nfsv4.so at $NFSV4_SO"
command -v rpcdebug >/dev/null || die "rpcdebug not found (install nfs-utils)"

HOST_PID=
MOUNTED=0
cleanup() {
    rpcdebug -m nfs -c >/dev/null 2>&1 || true
    rpcdebug -m nfs4 -c >/dev/null 2>&1 || true
    if [ "$KEEP" = 1 ]; then
        echo "==> KEEP=1: server (pid ${HOST_PID:-?}) + mount $MNT left up"
        return
    fi
    [ "$MOUNTED" = 1 ] && umount "$MNT" 2>/dev/null
    [ -n "$HOST_PID" ] && kill -INT "$HOST_PID" 2>/dev/null
}
trap cleanup EXIT

echo "==> serving nfsv4 on :$PORT (store $STORE)  [SPDLOG_LEVEL=debug]"
SPDLOG_LEVEL="${SPDLOG_LEVEL:-debug}" \
    "$PRFS_HOST" --store "$STORE" --clean --port "$PORT" --plugin "$NFSV4_SO" \
    >"$HOST_LOG" 2>&1 &
HOST_PID=$!
sleep 2
kill -0 "$HOST_PID" 2>/dev/null || {
    cat "$HOST_LOG"
    die "prfs-host failed to start"
}

umount -f -l "$MNT" 2>/dev/null || true
mkdir -p "$MNT"
echo "==> mount -t nfs -o vers=4.0,port=$PORT 127.0.0.1:/ $MNT"
timeout 30 mount -t nfs -o "vers=4.0,proto=tcp,port=$PORT" 127.0.0.1:/ "$MNT" ||
    die "mount failed"
MOUNTED=1
echo "   mounted OK"

# Pre-create the target so open() itself succeeds; the lock is the interesting bit.
echo "content" >"$MNT/lockme"

echo "==> enabling NFS client rpcdebug (state + callback + proc)"
rpcdebug -m nfs -s all >/dev/null 2>&1 || true
rpcdebug -m nfs4 -s all >/dev/null 2>&1 || true
dmesg -C # clear the ring buffer so we capture only the lock attempt

echo "==> attempting fcntl F_SETLK on $MNT/lockme (bare, then read-then-lock)"
python3 - "$MNT/lockme" <<'PY' || echo "   (lock probe returned non-zero)"
import fcntl, sys
p = sys.argv[1]

# Probe A: lock immediately after open (no prior I/O).
f = open(p, "r+")
try:
    fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0, 0)
    print("   [A bare]        LOCK acquired"); fcntl.lockf(f, fcntl.LOCK_UN, 10, 0, 0)
except OSError as e:
    print("   [A bare]        LOCK failed:", e)
f.close()

# Probe B: force a real open state via a read, THEN lock.
g = open(p, "r+")
try:
    g.read(1)
    fcntl.lockf(g, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0, 0)
    print("   [B read+lock]   LOCK acquired"); fcntl.lockf(g, fcntl.LOCK_UN, 10, 0, 0)
except OSError as e:
    print("   [B read+lock]   LOCK failed:", e)
g.close()
PY

rpcdebug -m nfs -c >/dev/null 2>&1 || true
rpcdebug -m nfs4 -c >/dev/null 2>&1 || true
rm -f "$MNT/lockme"

echo
echo "==== kernel NFS-client trace during the lock attempt (dmesg) ===="
dmesg
echo "==== end kernel trace ===="
echo
echo "==== effective mount options (watch for local_lock=) ===="
grep -a "$MNT " /proc/mounts || true
echo
echo "==== server-side handshake + a summary of every COMPOUND op-array ===="
grep -aE "SETCLIENTID|confirmed|serving on port" "$HOST_LOG" || echo "(no SETCLIENTID seen server-side)"
echo "-- COMPOUND op-array histogram (opnums: 12=LOCK 18=OPEN 35/36=SETCLIENTID) --"
grep -aoE "COMPOUND \[[^]]*\]" "$HOST_LOG" | sort | uniq -c
echo "==== end (full server log at $HOST_LOG) ===="
