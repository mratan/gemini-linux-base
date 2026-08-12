#!/bin/sh
# capture-c1.sh -- C1: working-transition capture on the Reference slot.
# (04-docs/DIVERGENCE-DEBUG-PLAN.md capture point C1: release the
# daemons from the held-off state and record the full bring-up event
# stream through patch push and Wi-Fi function-on -- our equivalent of
# bsg100's H18/H35 traces, with the pre-firmware baseline attached.)
#
# The release action is the one plan-sanctioned state change here:
# starting the Android LXC container units that Gemian normally runs at
# boot (multi-user.target wants lxc@android.service +
# droid-hal-init.service; Android init inside the container then starts
# wmt_loader -> wmt_launcher in vendor order -- the exact working
# transition we want, unmodified).
#
# REQUIREMENTS: instrumented capture kernel (boot3), C0a + C0b already
# taken THIS boot if possible (plan: one Reference-slot boot cycle). If
# C0b left the WMT core in a bad state (see checklist abort paths),
# reboot with daemons still masked, skip straight to this script.
#
# Usage: sh capture-c1.sh

set -u
. "$(dirname "$0")/capture-lib.sh"

cap_init c1
devmem_init || true
require_daemons_held

[ -w "$CAPHOOK" ] || echo "WARNING: $CAPHOOK missing (stock kernel?) - trace will lack CAPTURE-* kernel lines"

DMESG_MARK="CAPTURE-C1-START-$(date +%s)"
echo "$DMESG_MARK" > /dev/kmsg

{
    echo "=== C1 working-transition capture $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    regtable c1-pre-release
    ksnap c1-pre-release

    echo "--- releasing the daemons (Android LXC container) ---"
    # systemctl start does not un-mask; units were masked for the
    # hold-off boot, so unmask first, start, and leave the mask OFF from
    # here on (the session is past its pre-firmware captures).
    # shellcheck disable=SC2086  # intentional: two unit names
    systemctl unmask $HOLDOFF_UNITS 2>/dev/null || true
    systemctl start lxc@android.service
    systemctl start droid-hal-init.service

    echo "--- waiting for WMT bring-up (wmt_loader -> wmt_launcher -> firmware push) ---"
    i=0
    while [ $i -lt 120 ]; do
        [ -e /dev/wmtWifi ] && break
        sleep 1; i=$((i + 1))
    done
    if [ -e /dev/wmtWifi ]; then
        echo "OK: /dev/wmtWifi appeared after ${i}s"
    else
        echo "WARNING: /dev/wmtWifi still missing after 120s - container/daemon startup failed?"
        systemctl --no-pager status lxc@android.service droid-hal-init.service 2>&1 | head -30
    fi

    # Give the launcher time to complete the patch push; the H35
    # reference shows the whole push finishing within seconds of start.
    sleep 20

    echo "--- Wi-Fi function-on ---"
    # On Gemian the Wi-Fi function is normally switched on by userspace
    # writing '1' to /dev/wmtWifi (vendor wmt_chrdev_wifi). If the
    # container's own stack has not already done it, do it explicitly so
    # the C1 trace always contains the function-on sequence.
    if ! ip link show wlan0 >/dev/null 2>&1; then
        [ -e /dev/wmtWifi ] && echo 1 > /dev/wmtWifi
        sleep 10
    fi
    ip link show wlan0 >/dev/null 2>&1 && echo "OK: wlan0 exists" || echo "WARNING: no wlan0"

    regtable c1-post-bringup
    cpupcr_burst c1-post-bringup
    ksnap c1-post-bringup
} > "$CAPDIR/c1-regtable.txt" 2>&1

dmesg_save c1-dmesg.txt
sed -n "/$DMESG_MARK/,\$p" "$CAPDIR/c1-dmesg.txt" > "$CAPDIR/c1-dmesg-transition-window.txt"
grep -E "CAPTURE-|HARVEST-" "$CAPDIR/c1-dmesg-transition-window.txt" > "$CAPDIR/c1-kernel-capture-lines.txt" || true

echo "=== C1 trace volume (compare against H35: 1214 BTIF-RX / 1491 BTIF-TX / 592 WMT-RX / 1382 WMT-TX) ==="
for t in HARVEST-BTIF-RX HARVEST-BTIF-TX HARVEST-WMT-RX HARVEST-WMT-TX CAPTURE-INIT CAPTURE-STP-MODE; do
    printf '  %-16s %s\n' "$t" "$(grep -c "$t" "$CAPDIR/c1-kernel-capture-lines.txt" 2>/dev/null || echo 0)"
done
echo
echo "Next: associate to an AP (connmanctl or the Gemian UI), then run"
echo "capture-post-assoc.sh for the post-association snapshot."
echo "Files in $CAPDIR:"; ls -l "$CAPDIR"
