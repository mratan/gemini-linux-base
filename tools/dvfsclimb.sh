#!/bin/sh
# Climb the GPU OPP table one step at a time and DWELL at each rate, reporting
# what the clock tree and the supply actually did — not what devfreq recorded.
#
# This separates two explanations that the earlier runs cannot tell apart:
#   * a transition glitch   -> dies DURING a set_freq, at any rate
#   * a stability threshold -> survives the transition, dies while sitting at
#                              a rate the supply cannot sustain
D=/sys/class/devfreq/13040000.gpu
mount -t debugfs none /sys/kernel/debug 2>/dev/null
vgpu() { for r in /sys/class/regulator/*/; do [ "$(cat $r/name 2>/dev/null)" = vgpu ] && cat $r/microvolts; done; }
pll()  { awk '/^ *mfgpll  /{print "mfgpll="$5} /mfg_sel/{print "mfg_sel="$5} /mfg_bg3d/{print "bg3d="$5}' /sys/kernel/debug/clk/clk_summary | tr '\n' ' '; }

echo userspace > $D/governor
DWELL=${1:-10}
for f in 238000000 365000000 442500000 520000000 610000000 700000000 780000000; do
    echo "dvfsclimb: SETTING $f" > /dev/kmsg
    echo $f > $D/userspace/set_freq 2>/dev/null
    echo "dvfsclimb: SET OK $f  vgpu=$(vgpu)  $(pll)" > /dev/kmsg
    i=0
    while [ $i -lt $DWELL ]; do
        sleep 1
        i=$((i+1))
        echo "dvfsclimb: dwell $f  +${i}s" > /dev/kmsg
    done
    echo "dvfsclimb: SURVIVED ${DWELL}s at $f" > /dev/kmsg
done
echo "dvfsclimb: finished the whole table" > /dev/kmsg
