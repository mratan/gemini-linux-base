#!/bin/sh
# Graceful-failure smoke test (Slice 7 acceptance criterion 3):
# with NO /dev/wmtdetect and NO /dev/stpwmt present, both daemons must
#   - print a clear error naming the missing device node,
#   - exit nonzero but cleanly (no crash signal),
#   - terminate on their own well inside the timeout (no hang).
#
# Usage: smoke-no-devnodes.sh <bindir> [runner...]
#   bindir  directory containing wmt_loader and wmt_launcher
#   runner  optional command prefix to execute foreign-arch binaries,
#           e.g.: qemu-aarch64-static -L /usr/aarch64-linux-gnu
set -u

BINDIR=$1
shift
# Remaining args form the runner prefix (may be empty).

if [ -e /dev/wmtdetect ] || [ -e /dev/stpwmt ]; then
    echo "SKIP: WMT device nodes exist on this host; smoke test expects none" >&2
    exit 77
fi

export WMT_DEV_WAIT_SEC=2
HARD_TIMEOUT=30
fails=0

run_case() {
    name=$1; expect_msg=$2; shift 2
    echo "--- smoke: $name (expecting clean nonzero exit, message matching '$expect_msg')"
    out=$(timeout $HARD_TIMEOUT "$@" 2>&1)
    rc=$?
    echo "$out" | sed 's/^/    /'
    echo "    exit code: $rc"
    if [ $rc -eq 0 ]; then
        echo "FAIL($name): exited 0 with no device nodes present"; fails=$((fails+1))
    elif [ $rc -eq 124 ]; then
        echo "FAIL($name): hung (killed by timeout)"; fails=$((fails+1))
    elif [ $rc -gt 128 ]; then
        echo "FAIL($name): crashed (signal exit $rc)"; fails=$((fails+1))
    fi
    if ! echo "$out" | grep -q "$expect_msg"; then
        echo "FAIL($name): missing clear error message '$expect_msg'"; fails=$((fails+1))
    fi
}

run_case wmt_loader   "wmtdetect" "$@" "$BINDIR/wmt_loader"
run_case wmt_launcher "stpwmt"    "$@" "$BINDIR/wmt_launcher" -p /lib/firmware

if [ $fails -gt 0 ]; then
    echo "smoke-no-devnodes: $fails failure(s)"
    exit 1
fi
echo "smoke-no-devnodes: PASS"
exit 0
