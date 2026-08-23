#!/bin/sh
# run-psci.sh — fire the A72 PSCI bring-up at the device, from this directory.
#
#   stage 1  prerequisites + survey (no PSCI, no clock writes)
#   stage 2  + power the CPUTOP domain and rehearse ATF's clock section, so the
#            input to ATF's frequency check can be read without ATF spinning
#   stage 3  + raw PSCI CPU_ON to a park stub   <- can spin the issuing CPU
#   stage 5  + rehearse EVERY remaining unbounded poll in ATF's path, bounded,
#            and put the state back (add rehearse_cci=1 for the risky one)
#   stage 4  prerequisites + cluster powered, then `echo 1 > cpu8/online` by hand
#   stage 0  restore
#
# TWO THINGS THIS SCRIPT DOES THAT ARE NOT OPTIONAL
#
# 1. It pins cpufreq first. i2c6 carries the CPU regulator, the DA9214 is a
#    paged part, and a BUCKA transition landing inside a BUCKB write is the one
#    way this work can touch the rail the kernel runs on.
#
# 2. It arms a dead-man switch before stage 3. Measured on 2026-08-22: a PSCI
#    call that spins at EL3 does NOT cost only the calling CPU. Every new ssh
#    login installs a seccomp filter, which JITs, which calls
#    kick_all_cpus_sync(), which IPIs the CPU that will never answer — so each
#    attempt to log in and look burns another CPU, and within minutes sshd is
#    gone entirely. Meanwhile systemd is still alive and still petting the RGU,
#    so the watchdog never fires and `uhubctl` never sees a reset to ride: the
#    board sits there, enumerated, unreachable, needing the power button.
#
#    The switch is gemini-reboot's own two register writes (WDT_MODE enabled
#    with EXRST clear, then WDT_SWRST), fired from a detached shell after
#    DEADMAN seconds unless /tmp/a72-ok appears. That turns "wedged until
#    someone walks over" into "back on the network in ~75 s".
#
# NEVER reboot this device with anything but gemini-reboot. See B-40.
set -eu
DEV="${DEV:-root@10.15.19.82}"
EXTRA="${EXTRA:-}"
STAGE="${1:-1}"
CPU="${2:-8}"
DEADMAN="${DEADMAN:-240}"
REPO="${REPO:-$(cd ../../../.. && pwd)}"
HUB="${HUB:-1-10}"
HUBPORT="${HUBPORT:-1}"
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8"

# Rebuild first, always. B-32 and B-33 are both "the thing that ran was not the
# thing in the tree"; a committed .ko beside an edited .c is the same trap.
if [ -n "${SKIP_BUILD:-}" ]; then
    echo "== SKIP_BUILD set: shipping gemini-a72-psci.ko as it stands"
else
    PATH="$REPO/03-tools/gcc-13.3.0-nolibc/aarch64-linux/bin:$PATH" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux- \
        make -s -C "$REPO/07-kernel/build-local/src" M="$PWD" modules
fi

scp -q -o StrictHostKeyChecking=no gemini-a72-psci.ko "$DEV":/tmp/

# netconsole does not survive a reboot, and arming is not listening. Re-arm on
# every run; the listener has to be started separately and BEFORE this, because
# a run that wedges the box takes its own output with it.
"$REPO"/scripts/gemini-netconsole.sh arm >/dev/null 2>&1 || \
    echo "== WARNING could not re-arm netconsole"

# shellcheck disable=SC2029
$SSH "$DEV" "for c in /sys/devices/system/cpu/cpu[0-9]/cpufreq; do
        [ -d \$c ] || continue
        cur=\$(cat \$c/scaling_cur_freq)
        echo \$cur > \$c/scaling_min_freq 2>/dev/null || true
        echo \$cur > \$c/scaling_max_freq 2>/dev/null || true
     done
     grep -H . /sys/devices/system/cpu/cpu0/cpufreq/scaling_{min,max,cur}_freq"

case "$STAGE" in
0|1|2|5)
    # These never call PSCI: every poll in the module is bounded, so a plain
    # foreground insmod is safe and the output comes straight back.
    # shellcheck disable=SC2029
    $SSH "$DEV" "dmesg -C; insmod /tmp/gemini-a72-psci.ko stage=$STAGE cpu_target=$CPU $EXTRA; sleep 1; dmesg"
    ;;
*)
    # Record the log boundary before launch. Searching the whole accumulating
    # file can match a previous boot's CPU_ON result and disarm recovery for a
    # call that is actually still outstanding.
    LOG=$(ls -t "$REPO"/04-docs/captures/netconsole-*.log 2>/dev/null | head -1)
    if [ -z "$LOG" ]; then
        echo "== no netconsole log to watch — start the listener and re-run"
        exit 1
    fi
    START_BYTES=$(wc -c < "$LOG")

    echo "== arming the dead-man switch: hardware reset in ${DEADMAN}s unless /tmp/a72-ok appears"
    # shellcheck disable=SC2029
    $SSH "$DEV" "cat > /tmp/a72-deadman.sh <<'EOS'
#!/bin/sh
# Fires gemini-reboot's reset unless the run cancels it. Deliberately nothing
# but two register writes: by the time this matters, anything that needs RCU,
# an IPI, or a fresh process may already be unable to complete.
n=\$1
while [ \$n -gt 0 ]; do
    [ -f /tmp/a72-ok ] && exit 0
    n=\$((n-1))
    sleep 1
done
echo 'a72-deadman: FIRING the RGU reset' > /dev/kmsg
sync
busybox devmem 0x10007000 32 0x22000011
busybox devmem 0x10007014 32 0x1209
EOS
chmod +x /tmp/a72-deadman.sh
rm -f /tmp/a72-ok
dmesg -C
setsid nohup /tmp/a72-deadman.sh $DEADMAN >/tmp/deadman.log 2>&1 </dev/null &
setsid nohup sh -c 'insmod /tmp/gemini-a72-psci.ko stage=$STAGE cpu_target=$CPU $EXTRA
    # Do NOT cancel the reset the moment insmod returns. The dangerous
    # aftermath of a CPU_ON — an interconnect stall, the seccomp-JIT IPI
    # cascade, an A72 running where nothing expects one — develops seconds
    # later, and a dead-man that has already been disarmed is no use at all.
    # Prove the machine still works first.
    i=0
    while [ \$i -lt 20 ]; do
        sleep 1
        i=\$((i+1))
    done
    if [ -d /proc/1 ] && [ -r /proc/stat ] && ls /sys/devices/system/cpu >/dev/null 2>&1; then
        echo a72: alive 20s after the run, cancelling the dead-man > /dev/kmsg
        touch /tmp/a72-ok
    fi' >/tmp/insmod.log 2>&1 </dev/null &
sleep 1; echo launched" || true
    echo "== launched. Riding the recovery from here; do NOT ssh in to look."

    # Watching netconsole rather than the device is the whole point: a login
    # while an SMC is outstanding is what turns one lost CPU into a lost board.
    echo "== watching $LOG from byte $START_BYTES"
    i=0
    while [ $i -lt $((DEADMAN - 40)) ]; do
        if tail -c +$((START_BYTES + 1)) "$LOG" | grep -q "CPU_ON RETURNED"; then
            echo "== the SMC RETURNED — no reset needed"
            exit 0
        fi
        if tail -c +$((START_BYTES + 1)) "$LOG" | \
           grep -Eq "REFUSING CPU_ON|VPROC2 ENABLE FAILED|could not build the instrument|==== end ===="; then
            echo "== module stopped before issuing CPU_ON — no SMC is outstanding"
            exit 2
        fi
        sleep 2
        i=$((i + 2))
    done

    echo "== no return after ${i}s: ATF is spinning. Dropping the hub port so the"
    echo "== dead-man reset does not leave the preloader parked in download mode."
    "$REPO"/03-tools/uhubctl/uhubctl -l "$HUB" -p "$HUBPORT" -a off >/dev/null 2>&1
    sleep 140
    "$REPO"/03-tools/uhubctl/uhubctl -l "$HUB" -p "$HUBPORT" -a on >/dev/null 2>&1
    echo "== port back on, waiting for the device"
    i=0
    while [ $i -lt 30 ]; do
        sleep 15
        if $SSH "$DEV" 'echo DEVICE-UP nproc=$(nproc) up=$(cut -d. -f1 /proc/uptime)' 2>/dev/null; then
            exit 0
        fi
        i=$((i + 1))
    done
    echo "== DEVICE DID NOT RETURN — see B-40's recovery correction"
    exit 1
    ;;
esac
