#!/bin/bash
# run-qemu-test.sh — boot the initramfs against a synthetic disk in
# qemu-system-aarch64 and assert serial-output markers (tracker #5 QEMU
# proof, Tests A and B). TCG emulation on the CI runner, no KVM.
#
# The disk file is opened read-write (a read-only -drive would make the
# fail-safe test vacuous); whether anything was written is checked by the
# caller via before/after checksums.
#
# Usage:
#   run-qemu-test.sh --kernel vmlinuz --initrd initramfs.cpio.gz \
#       --disk disk.img --timeout 900 --log serial.log \
#       --marker GEMINI-INITRAMFS-PIVOT-OK [--marker ...] \
#       [--forbid GEMINI-INITRAMFS-RESCUE ...] [--append "extra args"]
set -euo pipefail

KERNEL=""; INITRD=""; DISK=""; LOG="serial.log"; TIMEOUT=900; APPEND=""
MARKERS=(); FORBID=()

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --initrd) INITRD="$2"; shift 2 ;;
        --disk) DISK="$2"; shift 2 ;;
        --log) LOG="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --append) APPEND="$2"; shift 2 ;;
        --marker) MARKERS+=("$2"); shift 2 ;;
        --forbid) FORBID+=("$2"); shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "$KERNEL" ] && [ -n "$INITRD" ] && [ -n "$DISK" ] && [ ${#MARKERS[@]} -gt 0 ] \
    || { echo "usage: see header" >&2; exit 2; }

: > "$LOG"
qemu-system-aarch64 \
    -machine virt -cpu max -smp 2 -m 1024 \
    -display none -serial "file:$LOG" -monitor none \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyAMA0 panic=-1 $APPEND" \
    -drive "file=$DISK,format=raw,if=none,id=hd0" \
    -device "virtio-blk-pci,drive=hd0,romfile=" \
    -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }
trap cleanup EXIT

result=""
deadline=$((SECONDS + TIMEOUT))
while [ $SECONDS -lt $deadline ]; do
    for f in ${FORBID[@]+"${FORBID[@]}"}; do
        if grep -q "$f" "$LOG" 2>/dev/null; then
            result="forbidden marker seen: $f"
            break 2
        fi
    done
    missing=""
    for m in "${MARKERS[@]}"; do
        grep -q "$m" "$LOG" 2>/dev/null || { missing="$m"; break; }
    done
    if [ -z "$missing" ]; then
        result=ok
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        result="qemu exited before markers appeared (missing: $missing)"
        break
    fi
    sleep 5
done
[ -n "$result" ] || result="timeout (${TIMEOUT}s) waiting for: $missing"

echo "===== serial log ($LOG, last 120 lines) ====="
tail -120 "$LOG" || true
echo "============================================="
if [ "$result" = ok ]; then
    echo "PASS: all markers present: ${MARKERS[*]}"
else
    echo "FAIL: $result"
    exit 1
fi
