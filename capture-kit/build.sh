#!/bin/sh
# capture-kit/build.sh -- reproducible containerized build of the gemian
# native 3.18.41 kernel (Reference-slot lineage) with a period toolchain
# (Debian stretch GCC 6.3 cross, see toolchain/Containerfile).
#
# Device-free work: this script never touches hardware.  Its output is
# STAGED and NOT-YET-FLASHED.
#
# Usage:
#   capture-kit/build.sh [--src DIR] [--out DIR] [--jobs N]
#
# From-scratch reproduction:
#   git clone --branch native https://github.com/gemian/gemini-linux-kernel-3.18 <src>
#   (optional, instrumented build) capture-kit/apply-patches.sh <src>
#   capture-kit/build.sh --src <src> --out <out>
#
# Produces in <out>:
#   arch/arm64/boot/Image.gz-dtb   (kernel + appended aeon6797_6m_n DTB,
#                                   exactly what gemian debian/rules ships)
#   .config, System.map, modules (in the tree, modules_install not run)
set -eu

SRC=/mercury/data/projects/gemini_linux/07-kernel/gemian-3.18
OUT=/mercury/data/projects/gemini_linux/07-kernel/gemian-3.18-out
JOBS=$(nproc)
IMAGE=localhost/gemian-3.18-stretch

while [ $# -gt 0 ]; do
    case "$1" in
        --src) SRC=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --jobs) JOBS=$2; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

HERE=$(cd "$(dirname "$0")" && pwd)

# Build the toolchain image if not present (idempotent otherwise).
if ! podman image exists "$IMAGE"; then
    podman build -t "$IMAGE" "$HERE/toolchain"
fi

mkdir -p "$OUT"

# O= out-of-tree build, exactly the gemian debian/rules invocation
# (make gemini_modular_defconfig; make; make modules) plus the explicit
# CROSS_COMPILE this cross container needs.
podman run --rm \
    -v "$SRC":/src \
    -v "$OUT":/out \
    -w /src \
    "$IMAGE" \
    sh -ec "
        cat /toolchain-id.txt
        make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gemini_modular_defconfig
        make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$JOBS
        make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$JOBS modules
    "

echo
echo "== build products =="
ls -l "$OUT/arch/arm64/boot/Image.gz-dtb"
strings "$OUT/init/version.o" 2>/dev/null | grep "Linux version" || true
find "$OUT" -name '*.ko' | wc -l
echo "NOT-YET-FLASHED: this artifact is staged only; nothing is flashed in this slice."
