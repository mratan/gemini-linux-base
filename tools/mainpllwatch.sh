#!/bin/sh
# Watch MAINPLL and UNIVPLL while the GPU climbs its OPP table.
#
# mfg_sel's parents include syspll_d3 (mainpll/3) and univpll_d3 (univpll/3),
# both fixed factors that propagate rate changes. If the clock framework ever
# picks one of those to serve a GPU OPP, it reprograms a PLL that clocks the
# whole SoC — every I2C, MSDC, UART and AXI master on the die.
#
# All of APMIXEDSYS is always powered, so these reads are safe whatever the
# MFG domain is doing.
D=/sys/class/devfreq/13040000.gpu
regs() {
  echo "MAINPLL[0x220]=$(busybox devmem 0x1000c220 32) [0x224]=$(busybox devmem 0x1000c224 32)" \
       "UNIVPLL[0x230]=$(busybox devmem 0x1000c230 32) [0x234]=$(busybox devmem 0x1000c234 32)" \
       "MFGPLL[0x240]=$(busybox devmem 0x1000c240 32) [0x244]=$(busybox devmem 0x1000c244 32)" \
       "CFG5=$(busybox devmem 0x10000050 32)"
}
echo userspace > $D/governor
echo "mainpllwatch: BEFORE  $(regs)" > /dev/kmsg
for f in 238000000 520000000 610000000; do
    echo "mainpllwatch: -> $f" > /dev/kmsg
    echo $f > $D/userspace/set_freq 2>/dev/null
    echo "mainpllwatch: at $f  $(regs)" > /dev/kmsg
done
echo "mainpllwatch: done" > /dev/kmsg
