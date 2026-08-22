#!/bin/sh
# run-a72.sh — fire the A72 bring-up at the device, from the repo root.
#
# The whole sequence is in the module; this exists so the experiment is one
# command the moment the device is reachable again.
#
#   stage 1  read-only survey (writes nothing)
#   stage 2  + prerequisites (VPROC2, EXT_BUCK_ISO, SRAM LDO) and read-backs
#   stage 3  + MP2 cluster power-up, ATF's power_on_little_cl steps 2..9
#   stage 4  + cluster B clock and the big PLL
#   stage 5  + boot address and ATF's 14-step power_on_little for the core
#   stage 6  + iDVFS enable before the core is released
#   stage 0  restore (cores down, cluster down, isolation re-asserted, VPROC2 off)
#
# NEVER reboot this device with anything but gemini-reboot. See B-40.
set -eu
DEV="${DEV:-root@10.15.19.82}"
STAGE="${1:-5}"
CPU="${2:-8}"
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8"

scp -o StrictHostKeyChecking=no gemini-a72-bringup.ko "$DEV":/tmp/
# shellcheck disable=SC2029
$SSH "$DEV" "dmesg -C; insmod /tmp/gemini-a72-bringup.ko stage=$STAGE cpu_target=$CPU; sleep 1; dmesg"
