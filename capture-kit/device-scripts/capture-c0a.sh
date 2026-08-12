#!/bin/sh
# capture-c0a.sh -- C0a: Pre-firmware capture, "nothing has touched BTIF".
# (04-docs/DIVERGENCE-DEBUG-PLAN.md, capture point C0a.)
#
# Run ON THE DEVICE as root, over SSH, on a Reference-slot userspace boot
# with the WMT daemons held off (see CHECKLIST-first-session.md).
# Works degraded on the stock Gemian kernel (devmem rows only); full
# coverage (PMIC, EMI CRC, in-kernel CPUPCR burst) needs the
# instrumented capture kernel booted from boot3.
#
# READ-ONLY: this script performs no state-changing action other than
# the optional 'modinit' step below, which registers the vendor
# connectivity drivers exactly as wmt_loader's ioctl would but powers
# nothing on (verified against wmt_detect.c / conn_drv_init.c).
#
# Usage: sh capture-c0a.sh [--skip-modinit]

set -u
. "$(dirname "$0")/capture-lib.sh"

SKIP_MODINIT=0
[ "${1:-}" = "--skip-modinit" ] && SKIP_MODINIT=1

cap_init c0a
devmem_init || true
require_daemons_held

{
    echo "=== C0a pre-firmware capture $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="

    # Sequence point 1: cold state, connectivity stack not even
    # module-initialized (MTK_WCN_REMOVE_KERNEL_MODULE => LK-handoff
    # cold state; the plan's evidence that LK leaves CONSYS off).
    regtable c0a-pre-modinit
    cpupcr_burst c0a-pre-modinit
    ksnap c0a-pre-modinit

    if [ "$SKIP_MODINIT" = 0 ] && [ -w "$CAPHOOK" ]; then
        echo "--- triggering modinit (wmt_loader-equivalent driver registration) ---"
        echo modinit > "$CAPHOOK"
        sleep 2
        # Sequence point 2: drivers registered, still no power / no BTIF
        # traffic. This is the state C0b starts from.
        regtable c0a-post-modinit
        cpupcr_burst c0a-post-modinit
        ksnap c0a-post-modinit
        [ -e "$WMTDBG" ] && echo "OK: $WMTDBG present (modinit took)" \
                         || echo "WARNING: $WMTDBG missing after modinit"
    else
        echo "--- modinit skipped (flag or stock kernel) ---"
    fi
} > "$CAPDIR/c0a-regtable.txt" 2>&1

dmesg_save c0a-dmesg.txt
grep -E "CAPTURE-|HARVEST-" "$CAPDIR/c0a-dmesg.txt" > "$CAPDIR/c0a-kernel-capture-lines.txt" || true

echo "C0a done. Files in $CAPDIR:"
ls -l "$CAPDIR"
echo "NOT-YET-FLASHED discipline note: this script runs only in the"
echo "physical session, after the remote-only period has ended."
