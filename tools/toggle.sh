#!/bin/sh
# Toggle between just TWO GPU OPPs, numbering every transition.
#
# Three separate runs died on their sixth clk_set_rate, at 1 s, 2 s and 10 s
# spacing and at three different target rates. If that is real, this dies at
# transition ~6 no matter which two rates are used — and #56 is a bug about
# the number of rate changes, not about any particular frequency.
D=/sys/class/devfreq/13040000.gpu
A=${1:-238000000}
B=${2:-365000000}
N=${3:-60}
GAP=${4:-1}
echo userspace > $D/governor
echo "toggle: $A <-> $B, $N transitions, ${GAP}s apart" > /dev/kmsg
i=0
while [ $i -lt $N ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 1 ]; then f=$B; else f=$A; fi
    echo "toggle: transition $i -> $f" > /dev/kmsg
    echo $f > $D/userspace/set_freq 2>/dev/null
    echo "toggle: transition $i OK, cur=$(cat $D/cur_freq) CON1=$(busybox devmem 0x1000c244 32)" > /dev/kmsg
    sleep $GAP
done
echo "toggle: completed all $N transitions" > /dev/kmsg
