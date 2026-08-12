#!/bin/sh
# capture-c0b.sh -- C0b: the single-query experiment from the held-off
# state. THE decisive observation of the whole program
# (04-docs/DIVERGENCE-DEBUG-PLAN.md): does the CONSYS ROM answer
# WMT_QUERY_STP *before* any firmware/patch push, on a vendor kernel?
#
# Mechanism (single-variable, all vendor code):
#   echo 0x7 0x3 0x1 > /proc/driver/wmt_dbg
# = wmt_dbg_func_ctrl -> mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI): the
# unmodified vendor power-on (mtk_wcn_consys_hw_pwr_on) followed by
# mtk_wcn_soc_sw_init, whose FIRST act on BTIF is init_table_1_2 =
# WMT_QUERY_STP in STP mand mode (wmt_ic_soc.c:709/1011) -- before any
# patch download. With wmt_launcher held off, the subsequent patch
# search self-terminates: wmt_ctrl_ul_cmd's signal wait times out after
# 2000 ms (wmt_ctrl.c:423), sw_init returns -6, func-on fails and the
# vendor code powers CONSYS back off. The query TX bytes, the ROM's
# answer (or its absence), and every register snapshot in between are
# captured by the instrumentation patches into dmesg.
#
# Interpretation (plan's decision tree):
#   CAPTURE-INIT: step="query stp default" result=PASS   -> ROM answers
#     pre-push (expected per vendor source; bsg100's 6.6 failure is a
#     hardware-precondition bug, diff C2 vs C0a).
#   ... result=RX-FAIL                                    -> ROM does NOT
#     answer pre-push even on the vendor kernel (falsifies the
#     vendor-source reading; bisect the C1 working transition).
#
# REQUIREMENTS: instrumented capture kernel (boot3), C0a already taken,
# modinit done, daemons still held off.
#
# Usage: sh capture-c0b.sh

set -u
. "$(dirname "$0")/capture-lib.sh"

cap_init c0b
devmem_init || true
require_daemons_held

[ -w "$CAPHOOK" ] || { echo "FATAL: $CAPHOOK missing - boot the instrumented capture kernel (boot3, silver+Esc)"; exit 1; }
[ -e "$WMTDBG" ]  || { echo "FATAL: $WMTDBG missing - run capture-c0a.sh first (it performs modinit)"; exit 1; }

DMESG_MARK="CAPTURE-C0B-START-$(date +%s)"
echo "$DMESG_MARK" > /dev/kmsg

{
    echo "=== C0b single-query experiment $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    regtable c0b-pre-funcon
    ksnap c0b-pre-funcon

    echo "--- triggering vendor func-on (WIFI) via $WMTDBG ---"
    echo "0x7 0x3 0x1" > "$WMTDBG"

    # power-on ~1 s + query + 2 s patch-search timeout (+ vendor retries);
    # 30 s is generous and lets the vendor failure path complete.
    sleep 30

    # Post-attempt state. If the vendor failure path already powered
    # CONSYS off, CONN rows will read SKIPPED-CONN-UNPOWERED -- itself a
    # data point (the off path ran to completion).
    regtable c0b-post-attempt
    cpupcr_burst c0b-post-attempt
    ksnap c0b-post-attempt
} > "$CAPDIR/c0b-regtable.txt" 2>&1

dmesg_save c0b-dmesg.txt

# Extract the verdict material
sed -n "/$DMESG_MARK/,\$p" "$CAPDIR/c0b-dmesg.txt" > "$CAPDIR/c0b-dmesg-experiment-window.txt"
grep -E "CAPTURE-|HARVEST-" "$CAPDIR/c0b-dmesg-experiment-window.txt" > "$CAPDIR/c0b-kernel-capture-lines.txt" || true

echo "=== C0b verdict material ==="
grep -E 'CAPTURE-INIT: step="query stp default"' "$CAPDIR/c0b-kernel-capture-lines.txt" || \
    echo "WARNING: no init_table_1_2 verdict line found - inspect dmesg manually"
echo "Files in $CAPDIR:"; ls -l "$CAPDIR"
