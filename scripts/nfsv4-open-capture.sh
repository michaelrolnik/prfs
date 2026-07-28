#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# Capture and decode the NFSv4.0 OPEN reply from BOTH the prfs plugin and the
# in-kernel server (knfsd), so they can be diffed field-by-field. The client locks
# knfsd but not prfs (ENOLCK), so the difference in the OPEN result — rflags
# (CONFIRM / LOCKTYPE_POSIX), delegation, attrset — is the lead. Needs root and
# tcpdump; knfsd bits need nfs-kernel-server.
#
#   sudo scripts/nfsv4-open-capture.sh
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-20490}"
PMNT="${PMNT:-/mnt/prfs}"
KMNT="${KMNT:-/mnt/knfsd}"
KEXPORT="${KEXPORT:-/tmp/knfsd-root}"
STORE="${STORE:-/tmp/prfs-v4}"
PRFS_HOST="${PRFS_HOST:-$REPO/build/prfs-host}"
NFSV4_SO="${NFSV4_SO:-$REPO/build/nfsv4.so}"
DECODE="$REPO/scripts/nfsv4-decode-open.py"

die() {
    echo "nfsv4-open-capture.sh: $*" >&2
    exit 1
}
[ "$(id -u)" -eq 0 ] || die "must run as root: sudo $0"
command -v tcpdump >/dev/null || die "tcpdump not found"
[ -x "$PRFS_HOST" ] || die "no prfs-host at $PRFS_HOST"

HOST_PID= TDPID= PMOUNTED=0 KMOUNTED=0 KEXPORTED=0
cleanup() {
    [ -n "$TDPID" ] && kill "$TDPID" 2>/dev/null
    [ "$PMOUNTED" = 1 ] && umount "$PMNT" 2>/dev/null
    [ "$KMOUNTED" = 1 ] && umount "$KMNT" 2>/dev/null
    [ "$KEXPORTED" = 1 ] && exportfs -u "127.0.0.1:$KEXPORT" 2>/dev/null
    [ -n "$HOST_PID" ] && kill -INT "$HOST_PID" 2>/dev/null
}
trap cleanup EXIT

capture() { # <pcap> <port> — start tcpdump, return once it's ready
    tcpdump -i lo -s0 -U -w "$1" "tcp port $2" >/dev/null 2>&1 &
    TDPID=$!
    sleep 1
}
stopcap() {
    sleep 1
    kill "$TDPID" 2>/dev/null
    wait "$TDPID" 2>/dev/null
    TDPID=
}

# ---- prfs ----
echo "==> prfs: serve on :$PORT, mount, capture an OPEN"
"$PRFS_HOST" --store "$STORE" --clean --port "$PORT" --plugin "$NFSV4_SO" \
    >/tmp/prfs-v4-host.log 2>&1 &
HOST_PID=$!
sleep 2
umount -f -l "$PMNT" 2>/dev/null || true
mkdir -p "$PMNT"
timeout 30 mount -t nfs -o vers=4.0,proto=tcp,port="$PORT" 127.0.0.1:/ "$PMNT" || die "prfs mount failed"
PMOUNTED=1
capture /tmp/prfs.pcap "$PORT"
echo "hello" >"$PMNT/capme"
python3 -c "open('$PMNT/capme','r+').read(1)" 2>/dev/null || true
stopcap

# ---- knfsd ----
echo "==> knfsd: export, mount, capture an OPEN"
if command -v exportfs >/dev/null; then
    mkdir -p "$KEXPORT"
    echo "hello" >"$KEXPORT/capme"
    command -v systemctl >/dev/null && systemctl is-active --quiet nfs-server ||
        { modprobe nfsd 2>/dev/null; systemctl start nfs-server 2>/dev/null || rpc.nfsd 8 2>/dev/null || true; }
    if exportfs -o rw,sync,no_subtree_check,insecure,fsid=0,no_root_squash "127.0.0.1:$KEXPORT" 2>/dev/null; then
        KEXPORTED=1
        umount -f -l "$KMNT" 2>/dev/null || true
        mkdir -p "$KMNT"
        if timeout 30 mount -t nfs -o vers=4.0,proto=tcp 127.0.0.1:/ "$KMNT" 2>/dev/null; then
            KMOUNTED=1
            capture /tmp/knfsd.pcap 2049
            python3 -c "open('$KMNT/capme','r+').read(1)" 2>/dev/null || true
            stopcap
        else
            echo "   !! knfsd mount failed — skipping knfsd capture"
        fi
    else
        echo "   !! knfsd export failed (fsid=0 taken?) — skipping knfsd capture"
    fi
else
    echo "   !! exportfs not present — skipping knfsd capture"
fi

echo
echo "==== prfs OPEN reply ===="
python3 "$DECODE" /tmp/prfs.pcap 2>/dev/null || echo "  (decode failed)"
echo
echo "==== knfsd OPEN reply ===="
[ -f /tmp/knfsd.pcap ] && python3 "$DECODE" /tmp/knfsd.pcap 2>/dev/null || echo "  (no knfsd capture)"
echo
echo "==== diff hint: compare rflags (CONFIRM/LOCKTYPE_POSIX) and delegation ===="
