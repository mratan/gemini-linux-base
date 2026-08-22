#!/bin/sh
# Drive the GPU OPP table by hand, with no GPU load at all. This isolates
# dev_pm_opp_set_rate() — the regulator write plus the clk_set_rate — from
# every other thing panfrost does. If the machine dies here, #56 is not a
# job-fault bug.
D=/sys/class/devfreq/13040000.gpu
echo userspace > $D/governor
FREQS=$(cat $D/available_frequencies)
N=${1:-200}
echo "dvfsloop: $N sweeps over: $FREQS" > /dev/kmsg
i=0
while [ $i -lt $N ]; do
    for f in $FREQS; do
        echo $f > $D/userspace/set_freq 2>/dev/null
    done
    for f in $(echo $FREQS | tr ' ' '\n' | tac | tr '\n' ' '); do
        echo $f > $D/userspace/set_freq 2>/dev/null
    done
    i=$((i+1))
    [ $((i % 20)) -eq 0 ] && echo "dvfsloop: sweep $i, cur=$(cat $D/cur_freq)" > /dev/kmsg
done
echo "dvfsloop: finished $N sweeps" > /dev/kmsg
