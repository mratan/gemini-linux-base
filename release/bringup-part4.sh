#!/bin/sh
# bringup-part4.sh — first CONSYS Wi-Fi bring-up on the Experimental slot.
# RUNBOOK.md Part 4, adapted for the headless experimental rootfs (Debian 13,
# user root, reached via `ssh -J damkohler root@10.15.19.82`).
#
# RUN IT STEP BY STEP, not all at once — each phase is a decision point. This
# file is the operator's crib; paste one block at a time and read dmesg between.
# The daemons ship INERT on purpose: nothing powers CONSYS as a boot side
# effect (that would race/contaminate). We power it deliberately, watching.
#
# Kernel log is the whole game here (no serial): keep `dmesg -w` running in a
# second SSH session throughout.
set -e
K="$(ls -d /lib/modules/*/updates 2>/dev/null | head -1)"   # gemini kver dir, not `uname -r`
echo "module dir: $K"

# ---------------------------------------------------------------------------
# Phase 0 — sanity before touching CONSYS
# ---------------------------------------------------------------------------
phase0() {
  uname -a
  cat /etc/gemini-experimental-release 2>/dev/null
  ls -l "$K"/mtk_btif.ko "$K"/mtk_stp_wmt_soc.ko "$K"/wlan_gen3.ko
  ls -l /lib/firmware/ROMv3_patch_1_0_hdr.bin /lib/firmware/ROMv3_patch_1_1_hdr.bin \
        /lib/firmware/WIFI_RAM_CODE_6797 /lib/firmware/WMT_SOC.cfg
  # CONSYS must be cold: no daemons, no /dev/stpwmt yet
  systemctl is-active wmt-loader.service wmt-launcher.service 2>/dev/null || true
  ls /dev/stpwmt /dev/wmtWifi 2>&1 || true
  # DISABLE the hardware watchdog first (same fault window as the capture
  # kernel — an unpetted RGU watchdog resets the SoC ~1-2 min into a
  # daemon-less boot). devmem via busybox; iproute2/util already present.
  busybox devmem 0x10007000 32 0x22000000 2>/dev/null || devmem2 0x10007000 w 0x22000000 2>/dev/null || true
  echo "WDT_MODE now: $(busybox devmem 0x10007000 2>/dev/null || devmem2 0x10007000 2>/dev/null)"
}

# ---------------------------------------------------------------------------
# Phase 1 — WMT core only (enough for the first-query capture; no datapath)
# EXPECT: /dev/wmtdetect + /dev/stpwmt appear; powers NOTHING by itself.
# ---------------------------------------------------------------------------
phase1() {
  insmod "$K/mtk_btif.ko"
  insmod "$K/mtk_stp_wmt_soc.ko"
  ls -l /dev/wmtdetect /dev/stpwmt
  dmesg | tail -30
}

# ---------------------------------------------------------------------------
# Phase 2 — Wi-Fi datapath module (single module post-#22; /dev/wmtWifi on load)
# ---------------------------------------------------------------------------
phase2() {
  insmod "$K/wlan_gen3.ko" || modprobe wlan_gen3
  ls -l /dev/wmtWifi
  dmesg | tail -30
}

# ---------------------------------------------------------------------------
# Phase 3 — start the daemons UNDER SUPERVISION (the WMT handshake).
# Watch for: wmt_loader publishing the chip id -> wmt_launcher taking the
# MT6797 SoC/BTIF path (STP_BTIF_FULL) -> on srh_patch offering the ROM
# patches from /lib/firmware -> kernel request_firmware()s + pushes to the
# CONSYS MCU. This is the FIRST time the ported stack powers CONSYS.
# Do NOT `systemctl enable` — keep manual this session.
# ---------------------------------------------------------------------------
phase3() {
  systemctl start wmt-loader.service      # oneshot: chip-id handshake via /dev/wmtdetect
  systemctl start wmt-launcher.service     # long-running: STP config + ROM-patch offer
  journalctl -u wmt-launcher -n 50 --no-pager
  dmesg | tail -60
}

# ---------------------------------------------------------------------------
# Phase 4 — verdict gate (04-docs/DIVERGENCE-DEBUG-PLAN.md decision tree)
#   PASS  WMT_QUERY_STP -> proceed to patch push, then Wi-Fi function-on.
#   FAIL / hard reset  -> that's the divergence. If the SoC RESETS (device
#     vanishes) like the capture kernel did on cold CONSYS access, STOP: a
#     USB-serial adapter is now required to read the reset reason (issue #31).
#     If it STALLS (no reset, no PASS), capture the enumerated register set at
#     the defined sequence points and diff against the C0a baseline in bring-up
#     order (power -> clocks -> reset -> BTIF init -> first TX -> first RX);
#     the first divergence is the finding. Single-variable changes only.
# ---------------------------------------------------------------------------

# Abort/recovery at any phase: `rmmod wlan_gen3 mtk_stp_wmt_soc mtk_btif`
# (reverse order); `systemctl stop wmt-launcher wmt-loader`; reboot to Android
# (no-key default) if wedged. boot2/nvram are never touched by any of this.

case "${1:-}" in
  0|phase0) phase0 ;;
  1|phase1) phase1 ;;
  2|phase2) phase2 ;;
  3|phase3) phase3 ;;
  *) echo "usage: $0 {0|1|2|3}  (run phases in order, read dmesg between)"; exit 2 ;;
esac
