#!/bin/sh
# capture-post-assoc.sh -- post-association snapshot (issue #9 acceptance
# criterion; the steady-state golden reference after C1). Run after
# associating the Reference slot to an AP (connmanctl / Gemian UI).
#
# READ-ONLY: register reads + wireless status queries only.
#
# PRIVACY NOTE: wireless status output contains the AP's and the
# device's MAC addresses. Per the standing never-commit rule these
# capture files must be scrubbed (scrub-capture.sh) before anything is
# committed to git.
#
# Usage: sh capture-post-assoc.sh

set -u
. "$(dirname "$0")/capture-lib.sh"

cap_init post-assoc
devmem_init || true

{
    echo "=== post-association snapshot $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    regtable c1-post-associate
    cpupcr_burst c1-post-associate
    ksnap c1-post-associate
} > "$CAPDIR/post-assoc-regtable.txt" 2>&1

{
    echo "--- wireless state ---"
    iw dev 2>/dev/null || true
    iw dev wlan0 link 2>/dev/null || iwconfig wlan0 2>/dev/null || true
    cat /proc/net/wireless 2>/dev/null || true
    ip -o addr show wlan0 2>/dev/null || true
    echo "--- wmt debug state ---"
    ls -la /dev/wmtWifi /dev/stpwmt 2>/dev/null || true
    lsmod 2>/dev/null | head -20
} > "$CAPDIR/post-assoc-wireless.txt" 2>&1

dmesg_save post-assoc-dmesg.txt
grep -E "CAPTURE-|HARVEST-" "$CAPDIR/post-assoc-dmesg.txt" > "$CAPDIR/post-assoc-kernel-capture-lines.txt" || true

echo "Post-association snapshot done. Files in $CAPDIR:"; ls -l "$CAPDIR"
echo "REMINDER: run scrub-capture.sh before committing any of this."
