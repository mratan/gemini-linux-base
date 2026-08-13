#!/bin/bash
# assemble.sh — Slice 10 (tracker issue #11): one-command assembly of the
# flash-ready Gemini PDA Experimental-slot deliverable from what the earlier
# slices produced. Output:
#
#   <out>/boot3-experimental.img   boot3 kernel image: Image.gz + appended DTB
#                                  + our busybox initramfs (bootimg.py, Slice 4)
#   <out>/experimental.img         Debian 13 arm64 loop-image rootfs
#                                  (mkrootfs-loop.sh, Slice 4) with:
#                                    - the ported connectivity .ko + the
#                                      USB-display .ko (udl, evdi — Slice U1,
#                                      issue #26) installed under
#                                      /lib/modules/<kver>/updates
#                                    - the CONSYS firmware payload in /lib/firmware
#                                    - the native WMT daemons + INERT systemd units
#                                    - the Slice 10 boot-time self-check
#   <out>/MANIFEST.txt             recorded manifest: every component's
#                                  provenance + SHA-256 (hashes only, no
#                                  device-unique data)
#   <out>/initramfs-gemini.cpio.gz the device initramfs packed into the boot3 image
#   <out>/NOT-YET-FLASHED.txt      status marker
#
# HARD CONSTRAINTS (see CLAUDE.md, ADR-0002): this script only ever
# reads/writes regular files and its own staging/output dir. It NEVER writes a
# block device, NEVER flashes, NEVER touches the GPT / nvram / boot2 / the
# Gemian rootfs. Everything it emits is a STAGED artifact, NOT-YET-FLASHED,
# waiting for the physical session the RUNBOOK.md describes. Real firmware
# blobs and any images live in the gitignored --out dir; only the manifest
# (text) is meant for git.
#
# Two firmware modes:
#   --payload DIR            stage the REAL CONSYS payload (private blobs;
#                            verified against the catalog first). Local only.
#   --firmware-placeholder   stage named PLACEHOLDER files (public CI: the real
#                            blobs must never enter a public repo/artifact, same
#                            rule as the kernel EXTRA_FIRMWARE embed). The
#                            manifest still records the authoritative catalog
#                            hashes so the deliverable's firmware identity is
#                            pinned regardless of mode.
#
# Release qualification (audit F2/F3, issue #24):
#   --verify-modules-dep   after staging, run depmod on the overlay (works for
#                          the prebuilt --modules-ko path too) and PROVE that
#                          `modprobe -n --show-depends` resolves every staged
#                          module: the connectivity set in the acyclic order
#                          mtk_btif -> mtk_stp_wmt_soc -> wlan_gen3, plus the
#                          USB-display set (udl, evdi). FAILS CLOSED on any
#                          missing/cyclic/unresolvable dependency.
#   --release-qualified    stamp `release_qualified: yes` in the manifest.
#                          Requires --verify-modules-dep to pass; a synthetic /
#                          placeholder run can NOT set it (green != flash-ready
#                          unless the modules.dep proof held).
#   --kernel-provenance S  record which kernel-build run/commit produced the
#                          Image.gz + DTB + .ko this manifest pins.
# depmod always generates modules.dep into the overlay so the shipped rootfs
# can `modprobe` by name; --verify-modules-dep additionally asserts it.
#
# Rootfs build (mkrootfs-loop.sh -> mmdebstrap + mkfs.ext4 -d) needs root;
# run it in CI / rootless-podman / with --build-rootfs under fakeroot-root.
# Without --build-rootfs the script still builds the boot3 image, stages the
# overlays, and writes the manifest (useful device-free where no root exists).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

# ---- authoritative CONSYS Wi-Fi firmware identity (public SHA-256s from
# 04-docs/PAYLOAD-CATALOG.md; the blobs themselves stay private) --------------
# name<TAB>size<TAB>sha256
FW_CATALOG="\
ROMv3_patch_1_0_hdr.bin	211908	d85864206b999501568a69edd8305319f9961fb65d56f77f438be29ff05c57a8
ROMv3_patch_1_1_hdr.bin	46472	8982bba207df136fe56be1b9d5f394d89128b3fd41977a8741928fe105399784
WIFI_RAM_CODE_6797	451904	c28c50efd411c591372b3a57a46cb99709db56e6cd86d65022af2baa88d840a6
WMT_SOC.cfg	80	f4a59b622a4e0c1470e475ce33f3edae43b27f1fbdeba54dc7cf07503d132880"

KO_NAMES="mtk_btif mtk_stp_wmt_soc wlan_gen3"
# Slice U1 (issue #26, PRD #25, ADR-0004): USB-display modules staged and
# depmod-verified alongside the connectivity set. udl is built in-tree by the
# kernel (CONFIG_DRM_UDL=m, configs/gemini-usbdisplay.config); evdi is the
# vendored out-of-tree modules/evdi. Both have no module-to-module deps by
# construction (their DRM helpers are forced =y), which the depmod
# verification below proves rather than assumes.
DISPLAY_KO_NAMES="udl evdi"

# ---- defaults --------------------------------------------------------------
KERNEL_DIR=""
MODULES_KO=""
KBUILD=""
MODULES_SRC="$REPO/modules/connectivity"
DAEMONS_OVERLAY="$REPO/userspace/wmt-daemons/rootfs-overlay"
PAYLOAD=""
FW_PLACEHOLDER=0
BUSYBOX=""
SPEC="$REPO/scripts/boot3-header.json"
RELEASE_OVERLAY="$REPO/release/rootfs-overlay"
OUT="$REPO/release/out"
BUILD_ROOTFS=0
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-}"
KVER=""
VERIFY_MODULES_DEP=0
RELEASE_QUALIFIED=0
KERNEL_PROVENANCE=""

usage() { sed -n '2,60p' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel-dir) KERNEL_DIR="$2"; shift 2 ;;
        --modules-ko) MODULES_KO="$2"; shift 2 ;;
        --kbuild) KBUILD="$2"; shift 2 ;;
        --modules-src) MODULES_SRC="$2"; shift 2 ;;
        --daemons-overlay) DAEMONS_OVERLAY="$2"; shift 2 ;;
        --payload) PAYLOAD="$2"; shift 2 ;;
        --firmware-placeholder) FW_PLACEHOLDER=1; shift ;;
        --busybox) BUSYBOX="$2"; shift 2 ;;
        --spec) SPEC="$2"; shift 2 ;;
        --release-overlay) RELEASE_OVERLAY="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --build-rootfs) BUILD_ROOTFS=1; shift ;;
        --cross-compile) CROSS_COMPILE="$2"; shift 2 ;;
        --kver) KVER="$2"; shift 2 ;;
        --verify-modules-dep) VERIFY_MODULES_DEP=1; shift ;;
        --release-qualified) RELEASE_QUALIFIED=1; shift ;;
        --kernel-provenance) KERNEL_PROVENANCE="$2"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "unknown arg: $1" >&2; usage 2 ;;
    esac
done

log() { echo "==> $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }

need sha256sum
mkdir -p "$OUT"
OVDIR="$OUT/overlays"
rm -rf "$OVDIR"; mkdir -p "$OVDIR"
MOD_OV="$OVDIR/modules"
FW_OV="$OVDIR/firmware"
mkdir -p "$MOD_OV" "$FW_OV"

sha() { sha256sum "$1" | cut -d' ' -f1; }
git_head() { git -C "$REPO" rev-parse HEAD 2>/dev/null || echo "unknown"; }

# ============================================================================
# 1. Connectivity modules -> $MOD_OV/lib/modules/<kver>/updates/*.ko
# ============================================================================
declare -a KO_FILES=()
declare -a DISPLAY_KO_FILES=()
if [ -n "$MODULES_KO" ]; then
    log "[1/6] using prebuilt modules from $MODULES_KO"
    for m in $KO_NAMES; do
        [ -f "$MODULES_KO/$m.ko" ] || die "prebuilt module missing: $MODULES_KO/$m.ko"
        KO_FILES+=("$MODULES_KO/$m.ko")
    done
    for m in $DISPLAY_KO_NAMES; do
        [ -f "$MODULES_KO/$m.ko" ] || die "prebuilt module missing: $MODULES_KO/$m.ko (USB display, issue #26 — kernel-build must ship it)"
        DISPLAY_KO_FILES+=("$MODULES_KO/$m.ko")
    done
elif [ -n "$KBUILD" ]; then
    log "[1/6] building connectivity modules against $KBUILD"
    [ -f "$KBUILD/Module.symvers" ] || die "not a built kernel tree: $KBUILD (no Module.symvers)"
    make -C "$KBUILD" M="$MODULES_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        modules -j"$(nproc)"
    for m in $KO_NAMES; do
        [ -f "$MODULES_SRC/$m.ko" ] || die "module did not build: $m.ko"
        KO_FILES+=("$MODULES_SRC/$m.ko")
    done
    log "      building evdi (modules/evdi) against $KBUILD"
    make -C "$KBUILD" M="$REPO/modules/evdi" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        modules -j"$(nproc)"
    [ -f "$REPO/modules/evdi/evdi.ko" ] || die "module did not build: evdi.ko"
    DISPLAY_KO_FILES+=("$REPO/modules/evdi/evdi.ko")
    # udl is in-tree: the kernel build itself must have produced it
    [ -f "$KBUILD/drivers/gpu/drm/udl/udl.ko" ] \
        || die "udl.ko missing from $KBUILD — is configs/gemini-usbdisplay.config merged (CONFIG_DRM_UDL=m)?"
    DISPLAY_KO_FILES+=("$KBUILD/drivers/gpu/drm/udl/udl.ko")
else
    die "need --modules-ko DIR or --kbuild DIR"
fi

# kver from vermagic of the first .ko unless overridden
if [ -z "$KVER" ]; then
    need modinfo
    KVER="$(modinfo -F vermagic "${KO_FILES[0]}" | awk '{print $1}')"
fi
[ -n "$KVER" ] || die "could not determine kernel release (kver)"
log "      gemini kernel release: $KVER"

MODINSTDIR="$MOD_OV/lib/modules/$KVER/updates"
if [ -n "$KBUILD" ] && [ -z "$MODULES_KO" ]; then
    # Proper modules_install (copies .ko + runs postprocessing). The former
    # wmt_chrdev_wifi<->wlan_gen3 EXPORT_SYMBOL cycle was resolved in issue #22
    # (chardev merged into wlan_gen3), so depmod now emits a valid modules.dep
    # with the acyclic order mtk_btif -> mtk_stp_wmt_soc -> wlan_gen3.
    make -C "$KBUILD" M="$MODULES_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        INSTALL_MOD_PATH="$MOD_OV" modules_install
else
    mkdir -p "$MODINSTDIR"
    for k in "${KO_FILES[@]}"; do cp "$k" "$MODINSTDIR/"; done
fi
# USB-display modules (Slice U1): plain copy on both paths — udl.ko comes out
# of the kernel tree (not MODULES_SRC), so modules_install above never sees it.
mkdir -p "$MODINSTDIR"
for k in "${DISPLAY_KO_FILES[@]}"; do cp "$k" "$MODINSTDIR/"; done
for m in $KO_NAMES $DISPLAY_KO_NAMES; do
    [ -f "$MODINSTDIR/$m.ko" ] || die "module not staged: $MODINSTDIR/$m.ko"
done
log "      staged $(echo $KO_NAMES $DISPLAY_KO_NAMES | wc -w) modules under /lib/modules/$KVER/updates"

# ---- depmod on the STAGED overlay (audit finding F2) -----------------------
# The prebuilt --modules-ko path used by release.yml previously only COPIED the
# .ko and never ran depmod, so the shipped modules.dep was unproven. Run depmod
# here for BOTH paths: it writes modules.dep{,.bin} into the overlay (so the
# rootfs can `modprobe` by name on the device), and with --verify-modules-dep
# we PROVE the acyclic load order resolves. depmod derives deps from the .ko
# symbol tables, so it works on prebuilt modules just as well as freshly built.
MODULES_DEP="$MOD_OV/lib/modules/$KVER/modules.dep"
MODDEP_VERIFIED=0
need depmod
if ! depmod -b "$MOD_OV" "$KVER" 2>/dev/null; then
    [ "$VERIFY_MODULES_DEP" = 1 ] && die "depmod failed for $KVER under $MODINSTDIR"
    echo "      warning: depmod reported problems (non-fatal without --verify-modules-dep)" >&2
fi
if [ "$VERIFY_MODULES_DEP" = 1 ]; then
    need modprobe
    [ -s "$MODULES_DEP" ] || die "modules.dep missing/empty after depmod: $MODULES_DEP"
    log "      verifying modprobe -n resolves all $(echo $KO_NAMES $DISPLAY_KO_NAMES | wc -w) modules"
    for m in $KO_NAMES $DISPLAY_KO_NAMES; do
        grep -q "updates/$m.ko" "$MODULES_DEP" || die "modules.dep has no entry for $m.ko"
        out="$(modprobe -n --show-depends -d "$MOD_OV" -S "$KVER" "$m" 2>&1)" \
            || die "modprobe -n failed for $m: $out"
        echo "$out" | grep -q "/updates/$m.ko" \
            || die "modprobe -n for $m did not resolve its own object: $out"
    done
    # the datapath module must pull the whole acyclic chain, in order
    chain="$(modprobe -n --show-depends -d "$MOD_OV" -S "$KVER" wlan_gen3 2>&1)"
    prev=-1
    for dep in mtk_btif mtk_stp_wmt_soc wlan_gen3; do
        n="$(printf '%s\n' "$chain" | grep -n "/updates/$dep.ko" | head -1 | cut -d: -f1)"
        [ -n "$n" ] || die "modprobe -n wlan_gen3 omitted $dep.ko (broken chain): $chain"
        [ "$n" -gt "$prev" ] || die "modprobe -n wlan_gen3 load order wrong at $dep.ko: $chain"
        prev="$n"
    done
    MODDEP_VERIFIED=1
    log "      modules.dep verified: mtk_btif -> mtk_stp_wmt_soc -> wlan_gen3 (acyclic)"
fi
if [ "$RELEASE_QUALIFIED" = 1 ] && [ "$MODDEP_VERIFIED" != 1 ]; then
    die "--release-qualified requires --verify-modules-dep to pass (fail closed)"
fi

# ============================================================================
# 2. Firmware payload -> $FW_OV/lib/firmware/<names>
# ============================================================================
mkdir -p "$FW_OV/lib/firmware"
if [ "$FW_PLACEHOLDER" = 1 ]; then
    log "[2/6] staging PLACEHOLDER firmware (public mode; real blobs stay private)"
    while IFS=$'\t' read -r name size want; do
        printf 'GEMINI-FIRMWARE-PLACEHOLDER %s (real blob is private; sha256=%s)\n' \
            "$name" "$want" > "$FW_OV/lib/firmware/$name"
    done <<< "$FW_CATALOG"
elif [ -n "$PAYLOAD" ]; then
    log "[2/6] staging REAL firmware payload from $PAYLOAD (verifying first)"
    # verify against catalog if the payload's repo carries verify-payload.sh
    PROOT="$(cd "$PAYLOAD/../.." && pwd)"
    if [ -x "$PROOT/scripts/verify-payload.sh" ]; then
        "$PROOT/scripts/verify-payload.sh" "$PAYLOAD" >/dev/null \
            || die "verify-payload.sh FAILED — refusing to stage a mismatched payload"
        log "      verify-payload.sh: PASS"
    else
        echo "      warning: verify-payload.sh not found at $PROOT/scripts; \
verifying against embedded catalog only" >&2
    fi
    SRC_FW="$PAYLOAD/rootfs-overlay/lib/firmware"
    while IFS=$'\t' read -r name size want; do
        [ -f "$SRC_FW/$name" ] || die "payload missing $name (looked in $SRC_FW)"
        got="$(sha "$SRC_FW/$name")"
        [ "$got" = "$want" ] || die "firmware hash mismatch $name: $got != $want (catalog)"
        cp "$SRC_FW/$name" "$FW_OV/lib/firmware/$name"
    done <<< "$FW_CATALOG"
    log "      staged + hash-verified $(echo "$FW_CATALOG" | wc -l) firmware blobs"
else
    die "need --payload DIR (real blobs) or --firmware-placeholder (public CI)"
fi

# ============================================================================
# 3. WMT daemons overlay (units are committed; binaries come from daemons.yml
#    / a local `make -C userspace/wmt-daemons all install`)
# ============================================================================
log "[3/6] daemon overlay: $DAEMONS_OVERLAY"
[ -d "$DAEMONS_OVERLAY" ] || die "daemons overlay not found: $DAEMONS_OVERLAY"
for u in wmt-loader.service wmt-launcher.service; do
    [ -f "$DAEMONS_OVERLAY/usr/lib/systemd/system/$u" ] || die "daemon unit missing: $u"
done
# inert-by-default guard: the daemons overlay must NOT enable the units
[ ! -e "$DAEMONS_OVERLAY/etc/systemd/system/multi-user.target.wants/wmt-loader.service" ] \
    && [ ! -e "$DAEMONS_OVERLAY/etc/systemd/system/multi-user.target.wants/wmt-launcher.service" ] \
    || die "a WMT daemon unit is enabled in the overlay — must stay INERT (ADR/Slice 7)"
DAEMON_BINS_PRESENT=1
for b in wmt_loader wmt_launcher; do
    [ -f "$DAEMONS_OVERLAY/usr/sbin/$b" ] || DAEMON_BINS_PRESENT=0
done
[ "$DAEMON_BINS_PRESENT" = 1 ] || \
    echo "      warning: daemon binaries absent (build with 'make -C userspace/wmt-daemons all install'); units still staged" >&2

# ============================================================================
# 4. boot3 image = kernel + DTB + device initramfs
# ============================================================================
BOOT3=""
if [ -n "$KERNEL_DIR" ] && [ -n "$BUSYBOX" ]; then
    log "[4/6] packing boot3 image (kernel + dtb + initramfs)"
    IMG="$KERNEL_DIR/Image.gz"
    [ -f "$IMG" ] || die "kernel not found: $IMG"
    # DTB is flat in the kernel-build CI artifact, but under dts/mediatek/ in a
    # raw kernel tree — accept either.
    DTB="$KERNEL_DIR/mt6797-gemini-pda.dtb"
    [ -f "$DTB" ] || DTB="$KERNEL_DIR/dts/mediatek/mt6797-gemini-pda.dtb"
    [ -f "$DTB" ] || die "dtb not found under $KERNEL_DIR (mt6797-gemini-pda.dtb)"
    INITRAMFS="$OUT/initramfs-gemini.cpio.gz"
    python3 "$REPO/scripts/mkinitramfs.py" --busybox "$BUSYBOX" \
        --init "$REPO/initramfs/init" --out "$INITRAMFS" >/dev/null
    BOOT3="$OUT/boot3-experimental.img"
    python3 "$REPO/scripts/bootimg.py" pack --spec "$SPEC" \
        --kernel "$IMG" --dtb "$DTB" --ramdisk "$INITRAMFS" --out "$BOOT3" >/dev/null
    # round-trip self-check: unpack must reproduce the exact inputs
    RT="$OUT/.roundtrip"; rm -rf "$RT"
    python3 "$REPO/scripts/bootimg.py" unpack --img "$BOOT3" --out-dir "$RT" >/dev/null
    KOUT="$RT/kernel.gz"; [ -f "$KOUT" ] || KOUT="$RT/kernel"   # gzip vs raw Image
    [ "$(sha "$KOUT")" = "$(sha "$IMG")" ] || die "boot3 round-trip: kernel mismatch"
    [ "$(sha "$RT/dtb")" = "$(sha "$DTB")" ] || die "boot3 round-trip: dtb mismatch"
    [ "$(sha "$RT/ramdisk.cpio.gz")" = "$(sha "$INITRAMFS")" ] || die "boot3 round-trip: ramdisk mismatch"
    rm -rf "$RT"
    log "      boot3 image packs + round-trips byte-exactly"
else
    log "[4/6] skipping boot3 image (need --kernel-dir and --busybox)"
fi

# ============================================================================
# 5. rootfs loop image (mkrootfs-loop.sh + overlays) — needs root
# ============================================================================
ROOTFS=""
if [ "$BUILD_ROOTFS" = 1 ]; then
    log "[5/6] building rootfs loop image (mkrootfs-loop.sh + overlays)"
    [ "$(id -u)" = 0 ] || die "--build-rootfs needs root (mmdebstrap + mkfs.ext4 -d)"
    ROOTFS="$OUT/experimental.img"
    OV_ARGS=(--overlay "$MOD_OV" --overlay "$FW_OV" --overlay "$DAEMONS_OVERLAY")
    [ -d "$RELEASE_OVERLAY" ] && OV_ARGS+=(--overlay "$RELEASE_OVERLAY")
    # --usb-display: the deliverable image always carries the USB-display
    # userspace (Slice U5, issue #30) — packaging.yml's separate minbase
    # pivot proof stays lean because it calls mkrootfs-loop.sh directly.
    "$REPO/scripts/mkrootfs-loop.sh" --out "$ROOTFS" --usb-display "${OV_ARGS[@]}"
else
    log "[5/6] skipping rootfs loop image (pass --build-rootfs, needs root)"
fi

# ============================================================================
# 6. Manifest
# ============================================================================
log "[6/6] writing manifest"
MANIFEST="$OUT/MANIFEST.txt"
{
    echo "# Gemini PDA Experimental-slot deliverable — assembly manifest"
    echo "# Slice 10 (tracker issue #11). Hashes only; NO device-unique data."
    echo "# STATUS: NOT-YET-FLASHED (see RUNBOOK.md; nothing is flashed device-free)."
    echo "generated_utc:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "fork_git_commit: $(git_head)"
    echo "kernel_release:  $KVER"
    echo "firmware_mode:   $([ "$FW_PLACEHOLDER" = 1 ] && echo placeholder || echo real)"
    echo "modules_dep_verified: $([ "$MODDEP_VERIFIED" = 1 ] && echo yes || echo no)"
    echo "usb_display_userspace: $([ "$BUILD_ROOTFS" = 1 ] && echo "staged (sway/foot/seatd/modetest — Slice U5, issue #30)" || echo "n/a (rootfs not built this run)")"
    echo "release_qualified:    $([ "$RELEASE_QUALIFIED" = 1 ] && echo yes || echo no)"
    [ -n "$KERNEL_PROVENANCE" ] && echo "kernel_provenance:    $KERNEL_PROVENANCE"
    echo
    echo "## boot3 image (kernel + appended DTB + device initramfs)"
    if [ -n "$BOOT3" ]; then
        echo "boot3_image      $(sha "$BOOT3")  ($(stat -c%s "$BOOT3") B)"
        echo "  Image.gz       $(sha "$IMG")"
        echo "  dtb            $(sha "$DTB")  (mt6797-gemini-pda.dtb)"
        echo "  initramfs      $(sha "$OUT/initramfs-gemini.cpio.gz")  (initramfs-gemini.cpio.gz)"
        [ -f "$KERNEL_DIR/kernel.config" ] && echo "  kernel.config  $(sha "$KERNEL_DIR/kernel.config")"
    else
        echo "  (boot3 image not built in this run)"
    fi
    echo
    echo "## ported connectivity modules  (/lib/modules/$KVER/updates)"
    echo "# module source tree: modules/connectivity @ fork commit above (see PROVENANCE.md)"
    for m in $KO_NAMES; do
        f="$MODINSTDIR/$m.ko"
        vm="$(modinfo -F vermagic "$f" 2>/dev/null || echo '?')"
        echo "  $m.ko  $(sha "$f")  [vermagic: $vm]"
    done
    echo "# NOTE: module set is mtk_btif -> mtk_stp_wmt_soc -> wlan_gen3 (acyclic);"
    echo "#       wmt_chrdev_wifi was merged into wlan_gen3 (issue #22). modprobe"
    echo "#       ordering works via the generated modules.dep. See RUNBOOK.md step 4."
    echo
    echo "## USB-display modules  (/lib/modules/$KVER/updates)  — Slice U1, ADR-0004"
    echo "# udl: in-tree (CONFIG_DRM_UDL=m, configs/gemini-usbdisplay.config), primary route"
    echo "# evdi: modules/evdi @ fork commit above (see its PROVENANCE.md), secondary route"
    for m in $DISPLAY_KO_NAMES; do
        f="$MODINSTDIR/$m.ko"
        vm="$(modinfo -F vermagic "$f" 2>/dev/null || echo '?')"
        echo "  $m.ko  $(sha "$f")  [vermagic: $vm]"
    done
    echo "# NOTE: no module-to-module deps (DRM helpers are =y). Right-port MUSB"
    echo "#       throughput/babble robustness for display-grade bulk is issue #27;"
    echo "#       nothing here is validated on hardware (NOT-YET-FLASHED)."
    echo
    echo "## modules.dep  (depmod on the staged overlay — proves load order)"
    if [ -s "$MODULES_DEP" ]; then
        echo "  modules.dep  $(sha "$MODULES_DEP")"
        sed 's/^/    /' "$MODULES_DEP"
        echo "# modprobe -n --show-depends resolves all $(echo $KO_NAMES $DISPLAY_KO_NAMES | wc -w) modules: $([ "$MODDEP_VERIFIED" = 1 ] && echo VERIFIED || echo 'not verified this run')"
    else
        echo "  (modules.dep not generated this run)"
    fi
    echo
    echo "## CONSYS firmware payload  (/lib/firmware)  — authoritative catalog hashes"
    while IFS=$'\t' read -r name size want; do
        if [ "$FW_PLACEHOLDER" = 1 ]; then
            echo "  $name  $want  ($size B) [catalog; staged as PLACEHOLDER this run]"
        else
            echo "  $name  $want  ($size B) [catalog; staged + verified]"
        fi
    done <<< "$FW_CATALOG"
    echo
    echo "## native WMT daemons  (INERT — units present, not enabled)"
    echo "# daemon source: userspace/wmt-daemons @ fork commit above (see PROVENANCE.md)"
    for u in wmt-loader.service wmt-launcher.service; do
        echo "  $u  $(sha "$DAEMONS_OVERLAY/usr/lib/systemd/system/$u")"
    done
    if [ "$DAEMON_BINS_PRESENT" = 1 ]; then
        for b in wmt_loader wmt_launcher; do
            echo "  $b  $(sha "$DAEMONS_OVERLAY/usr/sbin/$b")"
        done
    else
        echo "  (daemon binaries not present in this run; built by daemons.yml / make install)"
    fi
    echo
    echo "## Slice 10 self-check overlay"
    if [ -f "$RELEASE_OVERLAY/usr/local/sbin/gemini-slice10-verify.sh" ]; then
        echo "  gemini-slice10-verify.sh  $(sha "$RELEASE_OVERLAY/usr/local/sbin/gemini-slice10-verify.sh")"
    fi
    echo
    echo "## Experimental-slot rootfs loop image"
    if [ -n "$ROOTFS" ]; then
        echo "rootfs_image (build id)  $(sha "$ROOTFS")  ($(stat -c%s "$ROOTFS") B)"
    else
        echo "  (rootfs loop image not built in this run; needs root / CI)"
    fi
} > "$MANIFEST"

cat > "$OUT/NOT-YET-FLASHED.txt" <<'EOF'
STAGED Gemini PDA Experimental-slot deliverable (ADR-0002; Slice 10, issue #11).
Nothing here has been flashed. No flashing happens during the remote-only
period. The Gemian Reference slot (boot2 + the `linux` partition's own rootfs)
is NEVER written, resized or reformatted. The boot3 image goes to boot3 only;
experimental.img is copied as a FILE onto the `linux` partition BESIDE Gemian.
See release/RUNBOOK.md before any physical session.
EOF

echo
echo "=== assembly complete -> $OUT ==="
echo "  boot3 image:   ${BOOT3:-<not built>}"
echo "  rootfs image:  ${ROOTFS:-<not built>}"
echo "  manifest:      $MANIFEST"
echo
cat "$MANIFEST"
