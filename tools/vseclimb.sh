#!/bin/sh
# Move the vgpu rail through the OPP table's voltages with the GPU CLOCK
# PINNED at its lowest OPP. Nothing else changes.
#
# The pair of experiments:
#   dvfsclimb -> clock and voltage both move, dies at 610->700
#   vseclimb  -> ONLY the voltage moves
# If this one dies too, the rail is the problem and the frequency is
# innocent — which would also mean this rail is not what we think it is,
# because 1.09 V is inside the RT5735's own declared range and inside the
# vendor's own GPU OPP table.
D=/sys/class/devfreq/13040000.gpu
echo userspace > $D/governor
echo 238000000 > $D/userspace/set_freq
sleep 1
echo "vseclimb: clock pinned at $(cat $D/cur_freq)" > /dev/kmsg

# PROGVSEL0 = 0x11, bit 7 = buck enable, VOUT = 600mV + code*6.25mV
#  0xaa=42 -> 862500   0xb2=50 ->  912500   0xb8=56 ->  950000
#  0xc1=65 -> 1006250  0xc7=71 -> 1043750   0xcf=79 -> 1093750
#  0xd5=85 -> 1131250
for v in 0xaa 0xb2 0xb8 0xc1 0xc7 0xcf 0xd5; do
    echo "vseclimb: ABOUT TO WRITE PROGVSEL0=$v" > /dev/kmsg
    i2cset -y -f 3 0x1c 0x11 $v b 2>/dev/null
    echo "vseclimb: WROTE $v, reads back $(i2cget -y -f 3 0x1c 0x11 b 2>&1)" > /dev/kmsg
    i=0
    while [ $i -lt 8 ]; do sleep 1; i=$((i+1)); echo "vseclimb: dwell $v +${i}s" > /dev/kmsg; done
    echo "vseclimb: SURVIVED 8s at $v" > /dev/kmsg
done
echo "vseclimb: restoring 0xaa" > /dev/kmsg
i2cset -y -f 3 0x1c 0x11 0xaa b 2>/dev/null
echo "vseclimb: finished the whole voltage range" > /dev/kmsg
