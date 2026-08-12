#!/bin/bash
# fetch-qemu-boot-bits.sh — download (in CI, from the Debian archive) the
# pieces the QEMU proof needs but which must never be committed to this
# public repo as binaries (tracker #5):
#   * a stock Debian arm64 kernel (vmlinuz + modules) to boot qemu -M virt
#     — the gemini kernel needn't boot QEMU;
#   * the static arm64 busybox from the Debian busybox-static package, for
#     scripts/mkinitramfs.py.
#
# Uses `mmdebstrap --variant=extract`, which resolves+unpacks the packages
# without running any maintainer scripts (so no qemu binfmt needed here).
#
# Usage: fetch-qemu-boot-bits.sh <extract-dir>
# Prints shell assignments: VMLINUZ=... MODULES_BASE=... BUSYBOX=...
set -euo pipefail

DIR="${1:?usage: fetch-qemu-boot-bits.sh <extract-dir>}"
SUITE="${SUITE:-trixie}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"

mmdebstrap --variant=extract --architectures=arm64 \
    --include=busybox-static,linux-image-arm64 \
    "$SUITE" "$DIR" "$MIRROR" >&2

VMLINUZ="$(ls "$DIR"/boot/vmlinuz-*-arm64 2>/dev/null | sort -V | tail -1)"
MODULES_BASE="$(ls -d "$DIR"/usr/lib/modules/*-arm64 "$DIR"/lib/modules/*-arm64 2>/dev/null | sort -V | tail -1)"
BUSYBOX="$DIR/usr/bin/busybox"
[ -x "$BUSYBOX" ] || BUSYBOX="$DIR/bin/busybox"

[ -n "$VMLINUZ" ] && [ -f "$VMLINUZ" ] || { echo "no vmlinuz found under $DIR/boot" >&2; ls "$DIR/boot" >&2 || true; exit 1; }
[ -n "$MODULES_BASE" ] || { echo "no modules dir found under $DIR" >&2; exit 1; }

# The extract variant unpacks debs without running maintainer scripts, so
# depmod never ran and modules.dep does not exist — generate it here. The
# host depmod handles foreign-arch (arm64) modules fine; without the
# kernel's System.map it warns about unresolved kernel symbols but still
# records the module-to-module dependencies we need.
if [ ! -f "$MODULES_BASE/modules.dep" ]; then
    KVER="$(basename "$MODULES_BASE")"
    BASEDIR="${MODULES_BASE%/lib/modules/*}"
    echo "running depmod -b $BASEDIR $KVER (modules.dep missing in extract)" >&2
    depmod -b "$BASEDIR" "$KVER" 2>/dev/null || true
fi
[ -f "$MODULES_BASE/modules.dep" ] || { echo "no modules.dep under $MODULES_BASE (depmod failed)" >&2; exit 1; }
[ -f "$BUSYBOX" ] || { echo "no busybox binary found in extract" >&2; exit 1; }
file "$BUSYBOX" >&2 || true

echo "VMLINUZ=$VMLINUZ"
echo "MODULES_BASE=$MODULES_BASE"
echo "BUSYBOX=$BUSYBOX"
