# Capturing the vendor's ISP register programme (#66)

The MT6797 vendor kernel writes only 16 of pass1's 555 registers itself. Every
tuning value the camera HAL uses — OBC, LSC, BNR, the demosaic block, AWB, RRZ —
arrives from userspace, either through `ISP_WriteReg` (an ioctl taking arbitrary
`(module, offset, value)` triples) or through a command-queue descriptor buffer
whose address the kernel is simply handed. So reading `camera_isp.c` gives the
address map and stops there.

`ISP_WriteRegToHw()` already logs every write, gated on
`IspInfo.DebugMask & ISP_DBG_WRITE_REG`, and that mask is settable by ioctl. So
the whole programme is recordable on the stock stack with no patching and no
`/dev/mem` — which matters, because the Android side is `CONFIG_DEVMEM=n`.

## Runbook

**1. Get to the vendor stack.** boot1 is stock rooted Android (Magisk) and the
round trip is software-only, ~4 minutes — see `gemini-boot1-android-trip` and
`04-docs/RUNBOOK-FLASHING.md`. Do NOT flash anything to get there.

**2. Push the helper.** Build it on the Gemini's own Debian first (the repo
toolchain is nolibc, so it cannot build userspace):

```sh
scp 07-kernel/wt-main/tools/ispcap/ispcap.c root@10.15.19.82:/tmp/
ssh root@10.15.19.82 'gcc -static -O2 -o /tmp/ispcap /tmp/ispcap.c'
scp root@10.15.19.82:/tmp/ispcap .
# then, on boot1 Android:
adb push ispcap /data/local/tmp/ && adb shell su -c 'chmod +x /data/local/tmp/ispcap'
```

**3. Arm the trace, then run the camera.**

```sh
adb shell su -c '/data/local/tmp/ispcap 0x8'      # ISP_DBG_WRITE_REG
adb shell su -c 'dmesg -c > /dev/null'            # start from a clean log
# open the camera app — see gemini-android-camera-streams: the screen must be
# AWAKE, and never use `su -c` for `am`/`input`, which is what that memory is for
adb shell su -c 'dmesg' > isp-write-trace.txt
adb shell su -c '/data/local/tmp/ispcap -d' ; adb shell su -c 'dmesg' > isp-dump.txt
```

**4. Decode.** `04-docs/reference/mt6797-isp-pass1-registers.txt` maps offsets to
names, and marks the 16 the kernel writes so they can be told from HAL writes.

## If the ioctl fails

`ISP_CMD_DUMP_REG` and `ISP_CMD_DEBUG_FLAG` are **enum positions** — 9 and 12 in
the `gemian-3.18` header, which is a CROSS-CHECK for the stock kernel boot1 runs
rather than the same build. `ispcap` prints the ioctl number it issues and takes
`CMD_DEBUG_FLAG=` / `CMD_DUMP_REG=` / `ISPDEV=` from the environment. On EINVAL
or ENOTTY, recount the enum against the header for the running kernel before
concluding the driver is uncooperative.

## Why this is the right first move

It turns "reverse engineer an undocumented ISP" into "record what the working
one does" — the same move that closed #65, where diffing the SENINF instance the
vendor was actually using found three stacked faults in an afternoon after weeks
of guessing.
