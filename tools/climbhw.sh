#!/bin/sh
# Climb to each OPP and read MFGPLL_CON1 from the HARDWARE afterwards.
# Stops at the rate given as $1 and does not go further, so the machine is
# left sitting at the last rate rather than being pushed past it.
D=/sys/class/devfreq/13040000.gpu
STOP=${1:-610000000}
echo userspace > $D/governor
echo "climbhw: start, CON1=$(busybox devmem 0x1000c244 32)" > /dev/kmsg
for f in 238000000 365000000 442500000 520000000 610000000 700000000 780000000; do
    echo "climbhw: -> $f" > /dev/kmsg
    echo $f > $D/userspace/set_freq 2>/dev/null
    sleep 1
    echo "climbhw: at $f  devfreq_cur=$(cat $D/cur_freq)  CON0=$(busybox devmem 0x1000c240 32) CON1=$(busybox devmem 0x1000c244 32) CFG5=$(busybox devmem 0x10000050 32)" > /dev/kmsg
    [ "$f" = "$STOP" ] && break
done
echo "climbhw: stopped at $STOP, leaving it there" > /dev/kmsg
