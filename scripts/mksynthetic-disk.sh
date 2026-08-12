#!/bin/bash
# mksynthetic-disk.sh — build the synthetic QEMU test disk that mimics the
# Gemini's real layout for the ADR-0002 boot proof (tracker #5): one GPT
# partition NAMED "linux" containing BOTH a fake "Gemian rootfs" directory
# tree AND (optionally) the Experimental loop-image file side by side.
#
# The initramfs must find the loop image by GPT partition name + probe (the
# same logic that finds the real partition on the device) and must leave the
# fake Gemian tree untouched. With --no-image the disk reproduces the
# "image absent" failure case for the fail-safe test: the run must end in a
# rescue shell with the disk's checksum unchanged.
#
# Safety (ADR-0002): operates only on the output FILE given by --out. Never
# touches block devices; the image is populated with mkfs.ext4 -d (no
# mounting, no root needed).
#
# Usage:
#   mksynthetic-disk.sh --out diskA.img --image experimental.img
#   mksynthetic-disk.sh --out diskB.img --no-image
set -euo pipefail

OUT=""
IMAGE=""
NO_IMAGE=""
IMGPATH="/experimental.img"
PARTNAME="linux"

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --no-image) NO_IMAGE=1; shift ;;
        --imgpath) IMGPATH="$2"; shift 2 ;;
        --partname) PARTNAME="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "$OUT" ] || { echo "usage: see header" >&2; exit 2; }
if [ -z "$NO_IMAGE" ] && [ -z "$IMAGE" ]; then
    echo "need --image <experimental.img> or --no-image" >&2; exit 2
fi

STAGE="$(mktemp -d)"
PART="$(mktemp)"
trap 'rm -rf "$STAGE" "$PART"' EXIT

# --- fake Gemian rootfs tree (the thing that must never be modified) -------
mkdir -p "$STAGE"/{etc,usr/bin,home/gemian,sbin,var/log}
cat > "$STAGE/etc/os-release" <<'EOF'
PRETTY_NAME="Synthetic fake-Gemian tree (QEMU test fixture, not a real OS)"
ID=fake-gemian
EOF
echo fake-gemian > "$STAGE/etc/hostname"
cat > "$STAGE/GEMIAN-CANARY.txt" <<'EOF'
Fake Gemian rootfs canary — tracker #5 fail-safe test.
If the bytes of this partition change during a QEMU run that was supposed
to fail safe, the initramfs wrote to the partition that holds the Gemian
reference rootfs, violating ADR-0002.
EOF
# A decoy executable init: proves the initramfs pivots into the loop IMAGE,
# never into the partition's own tree.
printf '#!/bin/sh\necho GEMINI-DECOY-INIT-VIOLATION > /dev/console\n' > "$STAGE/sbin/init"
chmod 755 "$STAGE/sbin/init"
head -c 1048576 /dev/urandom > "$STAGE/var/log/filler.bin"

# --- the loop image, side by side (Test A only) -----------------------------
if [ -z "$NO_IMAGE" ]; then
    mkdir -p "$STAGE$(dirname "$IMGPATH")"
    cp --sparse=always "$IMAGE" "$STAGE$IMGPATH"
fi

# --- one ext4 partition image, populated without mounting -------------------
DU_MB=$(du -sm --apparent-size "$STAGE" | cut -f1)
PART_MB=$((DU_MB + DU_MB / 5 + 48))
truncate -s "${PART_MB}M" "$PART"
mkfs.ext4 -q -F -d "$STAGE" "$PART"
e2fsck -fn "$PART" >/dev/null

# --- GPT disk with that partition named "$PARTNAME" -------------------------
PART_SECTORS=$((PART_MB * 2048))            # 512-byte sectors
START=2048
END=$((START + PART_SECTORS - 1))
TOTAL_SECTORS=$((END + 1 + 2048))           # room for backup GPT
rm -f "$OUT"
mkdir -p "$(dirname "$OUT")"
truncate -s $((TOTAL_SECTORS * 512)) "$OUT"
sgdisk -o -n "1:$START:$END" -t 1:8300 -c "1:$PARTNAME" "$OUT" >/dev/null
dd if="$PART" of="$OUT" bs=512 seek="$START" conv=notrunc,sparse status=none

sgdisk -p "$OUT"
echo "synthetic disk: $OUT ($(du -h --apparent-size "$OUT" | cut -f1)), partition '$PARTNAME'," \
     "image $([ -n "$NO_IMAGE" ] && echo ABSENT || echo "present at $IMGPATH")"
sha256sum "$OUT"
