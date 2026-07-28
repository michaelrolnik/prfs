#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# CONTROL experiment for the nfsv4 ENOLCK investigation: can THIS Linux client do
# NFSv4.0 byte-range locking at all — against the in-kernel server (knfsd)? Runs
# the exact same fcntl F_SETLK probe as scripts/nfsv4-lock-debug.sh, but over a
# knfsd export instead of the prfs plugin. This partitions the problem:
#
#   knfsd LOCK acquired  ->  the prfs nfsv4 server is missing something.
#   knfsd LOCK ENOLCK    ->  the blocker is client/box-side (services/callback),
#                            NOT the prfs plugin.
#
# Non-persistent: exports via `exportfs -o` (in-memory table only, never edits
# /etc/exports) and unexports on exit. If it has to start nfs-server, it stops it
# again. Needs root and nfs-kernel-server (exportfs/rpc.nfsd):
#
#   sudo scripts/nfsv4-lock-control.sh
set -uo pipefail

EXPORT="${EXPORT:-/tmp/knfsd-root}"
MNT2="${MNT2:-/mnt/knfsd}"

die() {
    echo "nfsv4-lock-control.sh: $*" >&2
    exit 1
}
[ "$(id -u)" -eq 0 ] || die "must run as root: sudo $0"
command -v exportfs >/dev/null || die "exportfs not found — install nfs-kernel-server / nfs-utils"
command -v rpc.nfsd >/dev/null || die "rpc.nfsd not found — install nfs-kernel-server / nfs-utils"

STARTED_NFS=0
EXPORTED=0
MOUNTED=0
cleanup() {
    [ "$MOUNTED" = 1 ] && umount "$MNT2" 2>/dev/null
    [ "$EXPORTED" = 1 ] && exportfs -u "127.0.0.1:$EXPORT" 2>/dev/null
    if [ "$STARTED_NFS" = 1 ]; then
        echo "==> stopping knfsd (we started it)"
        systemctl stop nfs-server 2>/dev/null || rpc.nfsd 0 2>/dev/null || true
    fi
}
trap cleanup EXIT

mkdir -p "$EXPORT"
echo "control content" >"$EXPORT/lockme"

#  Make sure knfsd is running (start it only if it isn't; remember so we can stop).
if command -v systemctl >/dev/null && systemctl is-active --quiet nfs-server; then
    echo "==> nfs-server already active (leaving it running)"
else
    echo "==> starting knfsd"
    modprobe nfsd 2>/dev/null || true
    mountpoint -q /proc/fs/nfsd || mount -t nfsd nfsd /proc/fs/nfsd 2>/dev/null || true
    if command -v systemctl >/dev/null && systemctl start nfs-server 2>/dev/null; then
        STARTED_NFS=1
    else
        rpc.nfsd 8 2>/dev/null && STARTED_NFS=1 || true
    fi
fi
if [ -r /proc/fs/nfsd/versions ]; then
    echo "   nfsd versions: $(cat /proc/fs/nfsd/versions)"
    grep -q '+4' /proc/fs/nfsd/versions || echo "   !! NFSv4 appears disabled in nfsd (-4); mount may fail"
fi

echo "==> exporting $EXPORT as the NFSv4 pseudo-root (fsid=0)"
exportfs -o rw,sync,no_subtree_check,insecure,fsid=0,no_root_squash "127.0.0.1:$EXPORT" ||
    die "exportfs failed (does an existing export already claim fsid=0? check 'exportfs -v')"
EXPORTED=1
exportfs -v 2>/dev/null | sed 's/^/   /'

umount -f -l "$MNT2" 2>/dev/null || true
mkdir -p "$MNT2"
echo "==> mount -t nfs -o vers=4.0 127.0.0.1:/ $MNT2"
timeout 30 mount -t nfs -o vers=4.0,proto=tcp 127.0.0.1:/ "$MNT2" ||
    die "mount failed (is nfsd listening on :2049 with v4 enabled?)"
MOUNTED=1
grep -a "$MNT2 " /proc/mounts | sed 's/^/   effective: /'

echo "==> lock probe against knfsd ($MNT2/lockme) — identical to the prfs probe"
python3 - "$MNT2/lockme" <<'PY' || echo "   (probe returned non-zero)"
import fcntl, sys
p = sys.argv[1]

f = open(p, "r+")
try:
    fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0, 0)
    print("   [knfsd bare]      LOCK acquired"); fcntl.lockf(f, fcntl.LOCK_UN, 10, 0, 0)
except OSError as e:
    print("   [knfsd bare]      LOCK failed:", e)
f.close()

g = open(p, "r+")
try:
    g.read(1)
    fcntl.lockf(g, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0, 0)
    print("   [knfsd read+lock] LOCK acquired"); fcntl.lockf(g, fcntl.LOCK_UN, 10, 0, 0)
except OSError as e:
    print("   [knfsd read+lock] LOCK failed:", e)
g.close()
PY

echo
echo "==== VERDICT ===="
echo "  LOCK acquired  -> this client CAN lock NFSv4; the prfs nfsv4 server is missing something."
echo "  LOCK ENOLCK    -> this client can't lock ANY NFSv4 server; blocker is client/box-side."
