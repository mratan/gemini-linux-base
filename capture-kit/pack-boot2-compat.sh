#!/bin/sh
# pack-boot2-compat.sh -- pack the instrumented Reference-lineage kernel
# into a boot2-COMPATIBLE Android v0 boot image, and round-trip verify.
#
# "boot2-compatible" means: LK-bootable with the Gemian rootfs -- same
# header facts and the same Gemian ramdisk as the shipped Gemian boot
# image (debian_boot.img). Per ADR-0002 and the divergence-debug plan,
# this image is DESTINED FOR BOOT3 ONLY during the physical session
# (boot2, the Reference slot, is never flashed).
#
# NOTHING IS FLASHED BY THIS SCRIPT. Output is staged + checksummed and
# marked NOT-YET-FLASHED.
#
# Inputs:
#   --out-dir   staging dir (default: capture-kit/staging -- gitignored,
#               contains the Gemian ramdisk, which is never committed)
#   --kernel-out  kernel build output dir (from build.sh)
#   --ref       reference Gemian boot image (local flash-set; supplies
#               the ramdisk and cross-checks the header facts)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BOOTIMG="$REPO/scripts/bootimg.py"

KOUT=/mercury/data/projects/gemini_linux/07-kernel/gemian-3.18-out-capture
REF=/mercury/data/projects/gemini_linux/02-firmware/flash-set/debian_boot.img
OUTDIR="$HERE/staging"

while [ $# -gt 0 ]; do
    case "$1" in
        --out-dir) OUTDIR=$2; shift 2 ;;
        --kernel-out) KOUT=$2; shift 2 ;;
        --ref) REF=$2; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[ -f "$REF" ] || { echo "FATAL: reference Gemian boot image not found: $REF"; exit 1; }
[ -f "$KOUT/arch/arm64/boot/Image.gz" ] || { echo "FATAL: no built kernel in $KOUT (run build.sh first)"; exit 1; }

mkdir -p "$OUTDIR"
UNPACK="$OUTDIR/ref-unpacked"
rm -rf "$UNPACK"; mkdir -p "$UNPACK"

echo "== 1. unpack reference Gemian boot image (header facts + ramdisk) =="
python3 "$BOOTIMG" unpack --img "$REF" --out-dir "$UNPACK"

echo "== 2. pack instrumented kernel + gemian DTB + Gemian ramdisk =="
IMG="$OUTDIR/boot3-capture-gemian.img"
python3 "$BOOTIMG" pack \
    --spec "$UNPACK/header.json" \
    --kernel "$KOUT/arch/arm64/boot/Image.gz" \
    --dtb "$KOUT/arch/arm64/boot/dts/aeon6797_6m_n.dtb" \
    --ramdisk "$UNPACK/ramdisk.cpio.gz" \
    --out "$IMG"

echo "== 3. round-trip verify =="
RT="$OUTDIR/roundtrip"
rm -rf "$RT"; mkdir -p "$RT"
python3 "$BOOTIMG" unpack --img "$IMG" --out-dir "$RT"
fail=0
for pair in \
    "$KOUT/arch/arm64/boot/Image.gz:$RT/kernel.gz" \
    "$KOUT/arch/arm64/boot/dts/aeon6797_6m_n.dtb:$RT/dtb" \
    "$UNPACK/ramdisk.cpio.gz:$RT/ramdisk.cpio.gz"; do
    a=${pair%%:*}; b=${pair#*:}
    ha=$(sha256sum "$a" | cut -d' ' -f1); hb=$(sha256sum "$b" | cut -d' ' -f1)
    if [ "$ha" = "$hb" ]; then
        echo "  OK  $(basename "$a") round-trips byte-exact ($ha)"
    else
        echo "  FAIL $(basename "$a"): $ha != $hb"; fail=1
    fi
done
[ "$fail" = 0 ] || { echo "ROUND-TRIP FAILED"; exit 1; }

echo "== 4. stage + checksum + mark =="
( cd "$OUTDIR" && sha256sum "$(basename "$IMG")" > SHA256SUMS )
cat > "$OUTDIR/NOT-YET-FLASHED.txt" <<EOF
NOT-YET-FLASHED
===============
boot3-capture-gemian.img is STAGED ONLY. Nothing in this slice is
flashed. Remote-only period rules apply until the user declares the
device in hand.

When the physical session comes:
  - target partition: boot3 ONLY (Experimental slot home, ADR-0002).
    NEVER boot2 (Reference slot) even though this kernel is
    boot2-compatible by construction.
  - single-partition targeted write only (mtk w boot3 <img>); never
    'mtk wl'; never anything touching the GPT or nvram/nvdata.
  - boot selection: hold silver+Esc at power-on for boot3.
Packed $(date -u +%Y-%m-%dT%H:%M:%SZ) from:
  kernel : $KOUT (gemian native 3.18.41 + capture-kit patches)
  ramdisk: Gemian debian_boot.img ramdisk (local flash-set; NEVER commit)
  header : $REF header facts (v0, page 2048, kernel_addr 0x40080000)
EOF
cat "$OUTDIR/SHA256SUMS"
echo
echo "staged: $IMG"
echo "REMINDER: $OUTDIR contains the Gemian ramdisk -- it stays out of git."
