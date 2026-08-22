#!/bin/sh
# #56 acceptance: GPU-composited sway, a busy redrawing WINDOW, and repeated
# client surfaces arriving and leaving while the GPU is rendering.
#
# Xwayland is present and works; sway starts it lazily, so nothing exists
# until an X client connects and DISPLAY must come from sway's environment
# (an earlier version of this script launched glxgears from an ssh shell with
# no DISPLAY, got "couldn't open display", and passed twenty cycles with the
# GPU suspended the whole time — a test that could not fail).
#
# glxgears is the window from the original report. It is vsync-limited and on
# its own only reaches the lowest GPU OPP, so gpuwedge's hog runs alongside it
# to drive devfreq into the range that used to be fatal. The run ABORTS unless
# it observes both the GPU active and devfreq above 520 MHz — the threshold
# beyond which the old kernel reparented the GPU onto a system PLL.
CYCLES=${1:-20}
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
GPU=/sys/bus/platform/devices/13040000.gpu/power/runtime_status
FREQ=/sys/class/devfreq/13040000.gpu/cur_freq

echo "swaycycle: starting sway (gles2 on renderD128)" > /dev/kmsg
setsid /usr/local/sbin/gemini-dock > /root/sway.log 2>&1 &
for i in $(seq 1 30); do
    S=$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v lock | head -1)
    [ -n "$S" ] && break
    sleep 1
done
[ -n "$S" ] || { echo "swaycycle: ABORT — sway never came up" > /dev/kmsg; exit 1; }
export WAYLAND_DISPLAY=$(basename "$S")
export SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.*.sock 2>/dev/null | head -1)
sleep 4
echo "swaycycle: sway up on $WAYLAND_DISPLAY" > /dev/kmsg

swaymsg exec glxgears >/dev/null 2>&1
sleep 6
if pgrep -x Xwayland >/dev/null; then
    echo "swaycycle: Xwayland started on demand: $(pgrep -a Xwayland | head -1)" > /dev/kmsg
else
    echo "swaycycle: ABORT — no Xwayland, so glxgears is not a window" > /dev/kmsg
    pkill -x sway; exit 1
fi

setsid /root/gpuwedge hog 600 512 24 8 > /root/hog.log 2>&1 &
sleep 5

active=0; maxf=0
for i in $(seq 1 20); do
    [ "$(cat $GPU)" = active ] && active=$((active+1))
    f=$(cat $FREQ); [ "$f" -gt "$maxf" ] && maxf=$f
    sleep 0.25
done
echo "swaycycle: GPU active $active/20, devfreq reached ${maxf}, glxgears $(pgrep -x glxgears >/dev/null && echo alive || echo DEAD)" > /dev/kmsg
if [ "$active" -lt 3 ] || [ "$maxf" -le 520000000 ] || ! pgrep -x glxgears >/dev/null; then
    echo "swaycycle: ABORT — this run would prove nothing" > /dev/kmsg
    pkill -x gpuwedge; pkill -x glxgears; pkill -x sway; exit 1
fi

i=0
while [ $i -lt $CYCLES ]; do
    i=$((i+1))
    echo "swaycycle: cycle $i opening qterminal (gpu=$(cat $GPU) freq=$(cat $FREQ))" > /dev/kmsg
    swaymsg exec qterminal >/dev/null 2>&1
    sleep 4
    f=$(cat $FREQ); [ "$f" -gt "$maxf" ] && maxf=$f
    pkill -x qterminal
    sleep 2
    echo "swaycycle: cycle $i survived (gpu=$(cat $GPU) freq=$(cat $FREQ))" > /dev/kmsg
done
echo "swaycycle: PASS — $CYCLES cycles, glxgears $(pgrep -x glxgears >/dev/null && echo alive || echo died), highest OPP ${maxf}" > /dev/kmsg
pkill -x gpuwedge; pkill -x glxgears; pkill -x sway
