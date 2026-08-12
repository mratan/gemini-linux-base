#!/bin/sh
# normalize-capture.sh -- normalize capture output for mechanical diffing
# (04-docs/DIVERGENCE-DEBUG-PLAN.md "Diff method" step 1).
#
# Two modes:
#   normalize-capture.sh table <file...>
#       Emit the plan's field-per-line table "register,sequence-point,value"
#       (CSV) from CAPTURE-REG / CAPTURE-PMIC lines, whether they came from
#       the device scripts (plain) or from dmesg (kernel timestamp prefix).
#   normalize-capture.sh trace <file...>
#       Strip kernel timestamps from HARVEST-* / CAPTURE-* trace lines so
#       C1 traces diff with plain `diff` against the mirrored bsg100
#       H18/H35 logs (equally timestamp-stripped).
#
# Runs anywhere (device or workstation); POSIX sh + sed/awk only.

set -u
mode=${1:-}; shift 2>/dev/null || true

strip_ts() {
    # tolerate CR line endings (serial logs), then remove
    # "[   12.345678] " kernel prefixes if present
    sed -E -e 's/\r//g' -e 's/^\[[[:space:]]*[0-9]+\.[0-9]+\][[:space:]]//'
}

case "$mode" in
    table)
        cat "$@" | strip_ts | awk '
            /^CAPTURE-REG: /  { print $3 "," $2 "," $6; next }   # name,seq,value (addr in $4 dropped: fixed per name)
            /^CAPTURE-PMIC: / { print $3 "," $2 "," $6; next }
            /^CAPTURE-CPUPCR: .*sample=/ { printf "CPUPCR[%s],%s,%s\n", substr($3,8), $2, $5; next }
        ' ;;
    trace)
        cat "$@" | strip_ts | grep -E "^(HARVEST-|CAPTURE-)" ;;
    *)
        echo "usage: $0 table|trace <file...>" >&2; exit 2 ;;
esac
