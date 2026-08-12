#!/bin/bash
# mkrootfs-loop.sh — scripted (never hand-built) Debian 13 arm64 rootfs as a
# LOOP-IMAGE FILE for the Gemini PDA Experimental slot (ADR-0002, tracker #5).
#
# This is the loop-image counterpart of bsg100's scripts/mkrootfs.sh (which
# targets the whole `linux` partition and is kept unchanged as reference).
# ADR-0002: the Experimental rootfs is a FILE placed BESIDE the Gemian rootfs
# on the `linux` partition; that partition is never written, resized or
# reformatted by anything in this pipeline. Accordingly this script touches
# only its staging directory and the --out image file. The result is a
# staged artifact: NOT-YET-FLASHED, and never copied anywhere by CI.
#
# Runs in CI on ubuntu-24.04 as root (mmdebstrap root mode + qemu-user-static
# binfmt for the arm64 maintainer scripts):
#   sudo scripts/mkrootfs-loop.sh --out out/experimental.img
#
# The image is deliberately minbase-small (CI artifact budget): enough to
# boot systemd to a serial shell and prove the ADR-0002 boot path in QEMU.
# Fatter package sets (desktop etc.) come later and follow mkrootfs.sh.
#
# Dev-image conveniences (NOT for a flashed daily driver — see PACKAGING.md):
# passwordless root console login + serial autologin, so the QEMU proof can
# assert an interactive shell without secrets in the repo.
set -euo pipefail

SUITE=trixie
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
OUT=""
MARGIN_MB=96

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --suite) SUITE="$2"; shift 2 ;;
        --mirror) MIRROR="$2"; shift 2 ;;
        --margin-mb) MARGIN_MB="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "$OUT" ] || { echo "usage: mkrootfs-loop.sh --out experimental.img" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "must run as root (mmdebstrap root mode)" >&2; exit 1; }

TARGET="$(mktemp -d)"
trap 'rm -rf "$TARGET"' EXIT

PKGS=systemd-sysv,udev,kmod,e2fsprogs,busybox-static

echo "==> [1/4] mmdebstrap --variant=minbase $SUITE (arm64) -> $TARGET"
mmdebstrap --variant=minbase --architectures=arm64 \
    --include="$PKGS" "$SUITE" "$TARGET" "$MIRROR"

echo "==> [2/4] Configure target"
echo gemini-exp > "$TARGET/etc/hostname"
cat > "$TARGET/etc/hosts" <<'EOF'
127.0.0.1	localhost
127.0.1.1	gemini-exp
EOF
# / is the loop-mounted image; the initramfs mounts it and moves the host
# partition (the one that also carries the Gemian rootfs) to /host.
mkdir -p "$TARGET/host"
cat > "$TARGET/etc/fstab" <<'EOF'
# / is a loop-image file mounted by the Experimental-slot initramfs
# (ADR-0002). The backing partition is moved to /host by the initramfs.
EOF

# Build provenance + flash status marker, readable in the booted system.
cat > "$TARGET/etc/gemini-experimental-release" <<EOF
GEMINI_EXPERIMENTAL_ROOTFS=1
STATUS=NOT-YET-FLASHED
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
SUITE=$SUITE
GIT_SHA=${GITHUB_SHA:-local}
CI_RUN=${GITHUB_RUN_ID:-none}
EOF

# Dev image: passwordless root console login (autologin below never asks,
# but a bare `login` on another tty shouldn't dead-end either).
sed -i 's/^root:\*:/root::/; s/^root:x:/root::/' "$TARGET/etc/shadow"

# Serial console autologin (device: ttyS0; QEMU -M virt: ttyAMA0 — the
# drop-in is on the template so both instances get it).
mkdir -p "$TARGET/etc/systemd/system/serial-getty@.service.d"
cat > "$TARGET/etc/systemd/system/serial-getty@.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 921600,115200,57600,9600 %I $TERM
EOF

# Boot proof marker for the QEMU test (tracker #5 Test A): prints once
# multi-user is reached.
cat > "$TARGET/etc/systemd/system/gemini-proof.service" <<'EOF'
[Unit]
Description=Gemini Experimental rootfs boot marker (tracker #5 QEMU proof)
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo GEMINI-ROOTFS-SHELL-OK > /dev/console'
[Install]
WantedBy=multi-user.target
EOF
mkdir -p "$TARGET/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/gemini-proof.service \
    "$TARGET/etc/systemd/system/multi-user.target.wants/gemini-proof.service"

# Interactive-shell marker: printed by the autologin root shell itself.
cat >> "$TARGET/root/.profile" <<'EOF'
echo GEMINI-LOGIN-SHELL-OK
cat /etc/gemini-experimental-release 2>/dev/null
EOF

echo "==> [3/4] Pack ext4 loop image"
DU_MB=$(du -sm --apparent-size "$TARGET" | cut -f1)
SIZE_MB=$((DU_MB + DU_MB / 4 + MARGIN_MB))
rm -f "$OUT"
mkdir -p "$(dirname "$OUT")"
truncate -s "${SIZE_MB}M" "$OUT"
# -d populates without mounting: nothing in this pipeline ever mounts or
# writes a block device.
mkfs.ext4 -q -L gemini-exp -d "$TARGET" "$OUT"

echo "==> [4/4] fsck + report"
e2fsck -fn "$OUT" >/dev/null
echo "rootfs staging: ${DU_MB} MiB -> image: ${SIZE_MB} MiB"
sha256sum "$OUT"
echo "STATUS: NOT-YET-FLASHED staged artifact (ADR-0002 loop image)"
