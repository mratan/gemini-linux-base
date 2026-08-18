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
OVERLAYS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --suite) SUITE="$2"; shift 2 ;;
        --mirror) MIRROR="$2"; shift 2 ;;
        --margin-mb) MARGIN_MB="$2"; shift 2 ;;
        # --overlay DIR (repeatable): copy DIR's contents into the rootfs
        # staging tree before packing, root-owned (Slice 10 integration
        # uses this to inject the ported /lib/modules, /lib/firmware and the
        # WMT daemon overlay). Backward compatible: with no --overlay the
        # behaviour is identical to before, so packaging.yml is unaffected.
        --overlay) OVERLAYS+=("$2"); shift 2 ;;
        # --usb-display (Slice U5, tracker issue #30, ADR-0004): add the
        # USB-display userspace package set (sway/foot/seatd + modetest +
        # usbutils) so the deliverable image can drive a udl adapter with no
        # improvised installs at the first session. Default OFF to keep the
        # minbase QEMU pivot proof (packaging.yml) small; release/assemble.sh
        # always passes it for the deliverable image.
        --usb-display) USB_DISPLAY=1; shift ;;
        # --headless (issue: first-session SSH access, 2026-08-17): add an SSH
        # server + iproute2 so the Experimental slot is reachable over the USB
        # cable with NO serial adapter. Paired with release/headless-overlay,
        # which brings up a USB RNDIS gadget (+ ACM rescue console) at boot and
        # installs root's authorized_keys. release/assemble.sh always passes it
        # for the deliverable; packaging.yml's minbase QEMU proof does not, so
        # that proof stays lean. openssh-server's maintainer scripts run under
        # qemu-user-static during the mmdebstrap build, so host keys and the
        # sshd privsep user are generated correctly at build time.
        --headless) HEADLESS=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "$OUT" ] || { echo "usage: mkrootfs-loop.sh --out experimental.img" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "must run as root (mmdebstrap root mode)" >&2; exit 1; }

TARGET="$(mktemp -d)"
trap 'rm -rf "$TARGET"' EXIT

PKGS=systemd-sysv,udev,kmod,e2fsprogs,busybox-static
if [ "${USB_DISPLAY:-0}" = 1 ]; then
    # wlroots compositor (multi-GPU-capable; pixman renderer works on udl),
    # terminal, seat daemon, modetest (the runbook's pattern test), lsusb,
    # dbus + a monospace font for foot. See docs/external-display-first-
    # session-runbook.md Parts 3-5 and release/rootfs-overlay's
    # gemini-usb-display-start helper.
    # wl-mirror: output-mirroring client for the mirrored-display mode
    # (endpoint priority reordered 2026-08-13: docked single-output ->
    # mirrored -> extended; issue #21).
    PKGS="$PKGS,sway,foot,seatd,libdrm-tests,usbutils,dbus,fonts-dejavu-core,wl-mirror"
fi
if [ "${HEADLESS:-0}" = 1 ]; then
    # openssh-server: reachable over the USB RNDIS gadget (release/headless-
    # overlay). iproute2: the gadget script uses `ip` to address the interface.
    PKGS="$PKGS,openssh-server,iproute2"
fi

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

# Overlays (Slice 10): copy each --overlay tree over the staging root,
# root-owned. tar (not cp) so that overlaying e.g. lib/modules or lib/firmware
# extracts THROUGH the merged-usr symlinks (`/lib` -> `usr/lib`) instead of
# trying to replace the symlink with a directory (--keep-directory-symlink);
# symlinks and modes inside the overlay are preserved. --no-same-owner is
# REQUIRED: without it, root's tar restores the archive's stored ownership
# (the overlay files come from a checkout owned by the build user, uid 1000),
# which broke sshd StrictModes on /root/.ssh/authorized_keys (owner must be
# root). With it, extracted files take root:root, so mkfs.ext4 -d records
# root:root. Overlays are applied in order; later wins.
for ov in ${OVERLAYS[@]+"${OVERLAYS[@]}"}; do
    [ -d "$ov" ] || { echo "overlay dir not found: $ov" >&2; exit 1; }
    echo "==> [2b/4] overlay: $ov -> rootfs"
    ( cd "$ov" && tar -cf - . ) \
        | tar -C "$TARGET" --keep-directory-symlink --no-same-owner -xf -
done

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
