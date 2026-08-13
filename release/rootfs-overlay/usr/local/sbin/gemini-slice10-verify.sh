#!/bin/sh
# gemini-slice10-verify.sh — boot-time self-check for the assembled
# Experimental-slot rootfs (Slice 10, tracker issue #11). Emits serial
# markers the QEMU proof (release.yml) asserts, proving the assembly staged
# every component and left the WMT daemons INERT.
#
# It checks presence only — it never loads a module, never touches CONSYS,
# never starts a daemon. (There is no CONSYS in QEMU, and the whole point of
# the inert-by-default units is that the first power-on is a supervised
# on-device session, not a boot side effect. See userspace/wmt-daemons.)
#
# Note on kver: under QEMU the kernel is a STOCK Debian arm64 kernel, so
# `uname -r` is NOT the gemini kernel's release. The ported modules live
# under /lib/modules/<gemini-kver>/updates, so we glob rather than trust
# uname -r.
set -u

CON=/dev/console
say() { echo "$@" > "$CON" 2>/dev/null || echo "$@"; }

FW_NAMES="ROMv3_patch_1_0_hdr.bin ROMv3_patch_1_1_hdr.bin WIFI_RAM_CODE_6797 WMT_SOC.cfg"
KO_NAMES="mtk_btif mtk_stp_wmt_soc wlan_gen3"
# Slice U1 (issue #26): USB-display modules ride the same staging path.
KO_NAMES="$KO_NAMES udl evdi"
DAEMON_UNITS="wmt-loader.service wmt-launcher.service"

say "GEMINI-SLICE10-VERIFY: begin (uname -r=$(uname -r 2>/dev/null))"

# --- 1. ported connectivity modules present under some /lib/modules/*/updates
mods_ok=1
for m in $KO_NAMES; do
    if ! ls /lib/modules/*/updates/"$m".ko >/dev/null 2>&1; then
        say "GEMINI-SLICE10-MODULE-MISSING $m"
        mods_ok=0
    fi
done
[ "$mods_ok" = 1 ] && say "GEMINI-MODULES-STAGED-OK"

# --- 2. firmware payload staged into /lib/firmware (real blobs on device;
#        named placeholders in public CI — presence is what we assert here)
fw_ok=1
for f in $FW_NAMES; do
    if [ ! -f "/lib/firmware/$f" ]; then
        say "GEMINI-SLICE10-FIRMWARE-MISSING $f"
        fw_ok=0
    fi
done
[ "$fw_ok" = 1 ] && say "GEMINI-FIRMWARE-STAGED-OK"

# --- 3. WMT daemons installed AND inert (units + binaries present, not
#        enabled, not active/running)
d_ok=1
for u in $DAEMON_UNITS; do
    [ -f "/usr/lib/systemd/system/$u" ] || [ -f "/lib/systemd/system/$u" ] || {
        say "GEMINI-SLICE10-DAEMON-UNIT-MISSING $u"; d_ok=0; }
    en=$(systemctl is-enabled "$u" 2>/dev/null || true)
    case "$en" in
        enabled|enabled-runtime)
            say "GEMINI-SLICE10-DAEMON-ENABLED $u ($en)"; d_ok=0 ;;
    esac
    ac=$(systemctl is-active "$u" 2>/dev/null || true)
    case "$ac" in
        active|activating)
            say "GEMINI-SLICE10-DAEMON-ACTIVE $u ($ac)"; d_ok=0 ;;
    esac
done
for b in wmt_loader wmt_launcher; do
    [ -x "/usr/sbin/$b" ] || { say "GEMINI-SLICE10-DAEMON-BIN-MISSING $b"; d_ok=0; }
done
[ "$d_ok" = 1 ] && say "GEMINI-DAEMONS-INERT-OK"

if [ "$mods_ok" = 1 ] && [ "$fw_ok" = 1 ] && [ "$d_ok" = 1 ]; then
    say "GEMINI-SLICE10-VERIFY-OK"
else
    say "GEMINI-SLICE10-VERIFY-FAIL"
fi
