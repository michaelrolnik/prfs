#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# Run the pjdfstest POSIX filesystem conformance suite over a prfs NFS mount.
#
# prfs is a SYNTHETIC target, not a POSIX-faithful fs, so expect by-design
# failures: it doesn't enforce Unix modes (ACCESS grants everything), uses a
# deterministic logical clock (not wall-clock), and chflags is BSD-only. The
# signal worth reading is the STRUCTURAL categories — mkdir/rmdir/unlink/link/
# symlink/rename/open/mknod/mkfifo — where prfs should largely conform.
#
# pjdfstest and `mount` both need root, so run this with sudo:
#
#   sudo scripts/pjdfstest.sh                 # whole suite
#   sudo scripts/pjdfstest.sh mkdir           # one category (tests/mkdir)
#   sudo scripts/pjdfstest.sh tests/open/00.t # one test file
#   sudo KEEP=1 scripts/pjdfstest.sh          # leave the server + mount up after
#   sudo VERBOSE=1 scripts/pjdfstest.sh open  # per-assertion (prove -v)
#
# Config via env: PORT, MNT, STORE, PJD_DIR, PJD_REPO, PRFS_HOST, NFSV3_SO, KEEP,
# VERBOSE. Defaults assume an in-tree `meson` build in ./build.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-20490}"
MNT="${MNT:-/mnt/prfs}"
STORE="${STORE:-/tmp/prfs-pjd}"
PRFS_HOST="${PRFS_HOST:-$REPO/build/prfs-host}"
NFSV3_SO="${NFSV3_SO:-$REPO/build/nfsv3.so}"
PJD_DIR="${PJD_DIR:-$REPO/build/pjdfstest}" # checkout+build cache (build/ is gitignored)
PJD_REPO="${PJD_REPO:-https://github.com/pjd/pjdfstest}"
KEEP="${KEEP:-0}"
VERBOSE="${VERBOSE:-0}"
LOG="${LOG:-/tmp/pjdfstest.log}"

die() {
    echo "pjdfstest.sh: $*" >&2
    exit 1
}

[ "$(id -u)" -eq 0 ] || die "must run as root (mount + pjdfstest need it): sudo $0 $*"
[ -x "$PRFS_HOST" ] || die "no prfs-host at $PRFS_HOST (run: meson compile -C build)"
[ -f "$NFSV3_SO" ] || die "no nfsv3.so at $NFSV3_SO (run: meson compile -C build)"

STARTED_HOST=0
MOUNTED=0
HOST_PID=

cleanup() {
    if [ "$KEEP" = 1 ]; then
        echo "==> KEEP=1: leaving server (pid ${HOST_PID:-?}) and mount $MNT up"
        return
    fi
    [ "$MOUNTED" = 1 ] && umount "$MNT" 2>/dev/null || true
    [ "$STARTED_HOST" = 1 ] && [ -n "$HOST_PID" ] && kill -INT "$HOST_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 1. Build pjdfstest on first run (cached under build/).
if [ ! -x "$PJD_DIR/pjdfstest" ]; then
    echo "==> building pjdfstest in $PJD_DIR"
    rm -rf "$PJD_DIR"
    git clone --depth 1 "$PJD_REPO" "$PJD_DIR"
    (cd "$PJD_DIR" && autoreconf -ifs && ./configure && make) >"$PJD_DIR/build.log" 2>&1 ||
        die "pjdfstest build failed — see $PJD_DIR/build.log"
fi
[ -d "$PJD_DIR/tests" ] || die "no tests/ under $PJD_DIR"

# 2. Serve prfs on PORT unless something already listens there.
if ss -tlnH "sport = :$PORT" 2>/dev/null | grep -q ":$PORT"; then
    echo "==> using the server already listening on :$PORT"
else
    echo "==> starting prfs-host on :$PORT (store $STORE, --clean --time-advance)"
    "$PRFS_HOST" --store "$STORE" --clean --port "$PORT" --time-advance \
        --plugin "$NFSV3_SO" >/tmp/prfs-pjd-host.log 2>&1 &
    HOST_PID=$!
    STARTED_HOST=1
    sleep 2
    kill -0 "$HOST_PID" 2>/dev/null || {
        echo "prfs-host failed to start:"
        cat /tmp/prfs-pjd-host.log
        exit 1
    }
fi

# 3. Mount (NFSv3, explicit ports, no rpcbind/NLM).
mkdir -p "$MNT"
if mountpoint -q "$MNT"; then
    echo "==> $MNT already mounted"
else
    echo "==> mounting 127.0.0.1:/ -> $MNT"
    mount -t nfs -o "vers=3,proto=tcp,port=$PORT,mountport=$PORT,mountproto=tcp,nolock" \
        127.0.0.1:/ "$MNT"
    MOUNTED=1
fi

# 4. Resolve the target (whole suite, a category name, or a path) and run it.
#    pjdfstest creates its scratch files in the CWD, so cd into the mount first.
target="$PJD_DIR/tests"
if [ "$#" -ge 1 ]; then
    case "$1" in
    /*) target="$1" ;;                    # absolute path
    tests/*) target="$PJD_DIR/$1" ;;      # repo-relative
    *) target="$PJD_DIR/tests/$1" ;;      # bare category, e.g. mkdir
    esac
fi
[ -e "$target" ] || die "no such test target: $target"

prove_args=(-r)
[ "$VERBOSE" = 1 ] && prove_args=(-rv)

echo "==> running: prove ${prove_args[*]} $target   (cwd=$MNT)"
echo "    NOTE: prfs is synthetic — permission/ownership/timestamp/chflags failures are by design."
cd "$MNT"
set +e
prove "${prove_args[@]}" "$target" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e

echo
echo "==> done (prove exit $rc). Full log: $LOG"
echo "    Result line:"
grep -E "^(Result:|Files=)" "$LOG" | tail -2 || true
exit "$rc"
