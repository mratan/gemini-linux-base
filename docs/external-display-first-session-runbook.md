# First-session RUNBOOK — External display (HDMI) bring-up & validation

Tracker issue #18 (Slice H1). The ordered procedure for the **first physical session**
that validates HDMI-out on the Gemini. Everything the earlier device-free slices produced
(#13 DPI encoder + sii902x bridge, #16 right-port mux, #23 the DDP route + opt-in variant DTB)
is staged; this runbook only **flashes and validates** — it develops nothing new.

Track 3 always yields to the Internal Wi-Fi Prototype: if both are on the bench in one
session, do the Wi-Fi capture/bring-up first (`release/RUNBOOK.md`), then this.

---

## ⛔ GATE — do not execute any step below yet

**Nothing here runs until the remote-only period is declared over and the device is in hand.**
Building the HDMI-variant boot image, checking hashes, and reading this document are
device-free and fine. Anything that writes the device is not.

If you are reading this during the remote-only period, stop at the end of "Part 0".

---

## Standing safety rules (permanent — apply to every step)

- **Never `mtk wl`; never any operation that writes the GPT.** Single-partition targeted
  writes only (`mtk w boot3 …`).
- **Never flash or modify `nvram`/`nvdata`** (IMEI, Wi-Fi calibration/MAC).
- **Never flash boot2, never write the `linux` partition's Gemian rootfs** (ADR-0002). boot2 +
  Gemian is the Reference slot and must survive intact — it is also your fallback here.
- **The right-port mux is one-at-a-time by construction:** HDMI-out and right-port USB host are
  mutually exclusive (GPIO72 interlock). Selecting HDMI disables right-port USB host; that is
  expected, not a fault.
- Single-variable changes; every capture committed as text only, **after** scrubbing
  device-unique identifiers (MACs/IMEIs/EDID serial), to `04-docs/captures/<date>/`.

## Boot slots and recovery combo

| Slot | Key held at power-on | Contents |
|---|---|---|
| boot1 | none | rooted Android |
| boot2 | **silver** side button | Gemian 3.18.41 — **Reference slot, untouchable / fallback** |
| boot3 | **silver + Esc** | Experimental kernel this session |
| recovery | **Esc** | stock recovery |

**Stuck-DA recovery:** power off, hold **Esc + silver** to re-enter download mode, retry.
boot2 is never touched, so the device is never bricked by this runbook — worst case, restore
boot3 from its backup (Part 1) and boot Gemian (silver).

---

## Part 0 — device-free prep (safe now)

The HDMI pipe is **opt-in**: the default `mt6797-gemini-pda.dtb` keeps every HDMI-pipe node
disabled (DSI panel path untouched). HDMI bring-up uses the **variant** DTB
`mt6797-gemini-pda-hdmi.dtb` (from `patches/v6.6/dts/0028-…-hdmi-bringup-variant.patch`), which
`#include`s the base and enables `disp_ovl1/disp_rdma1/disp_dsc0/dpi0/sii9022/hdmi_connector`.

1. **Build the HDMI-variant boot3 image.** Same kernel build as the release path, but pack the
   **variant** DTB instead of the base one:
   - build the kernel (CI `kernel-build` artifact, or local `scripts/build.sh`);
   - pack boot3 with `scripts/bootimg.py` using `mt6797-gemini-pda-hdmi.dtb` as the appended DTB
     (this is the only difference from the release boot3 image).
   Keep it clearly named, e.g. `boot3-hdmi-bringup.img`, and **NOT-YET-FLASHED**.
2. **Prep the mux command.** The right-port mux is selected at runtime via sysfs (from #16):
   `echo hdmi > /sys/devices/platform/rightport-mux/mode` (read back with `cat`).
3. **Bring to the session:** `boot3-hdmi-bringup.img` + its hash; the Planet **30-001-01** HDMI
   cable (generic USB-C→HDMI cables do **not** work — the port is not DP Alt Mode); a monitor +
   HDMI cable; the mtkclient workstation; optional FTDI serial @ 921600.

> The rest of this runbook is **physical-session work — gated on the remote-only period being
> over.**

---

## Part 1 — back up the current boot3 *(first thing on the device)*

- [ ] Confirm the UBports TWRP backup of boot3 still exists (`/home/gemini/boot3-twrp-backup.img`
      on Gemian) — restore path #1.
- [ ] Read-back the live boot3 via mtkclient (download mode, **read only**):
      `mtk r boot3 boot3-preHDMI-$(date +%Y%m%d).img` — keep outside git; restore path #2.
- **Risk:** none beyond entering download mode (read-only). **Abort:** if mtkclient wedges,
  power off, Esc + silver, retry; the device still boots boot2 unchanged.

## Part 2 — flash the HDMI-variant kernel to boot3

- [ ] Enter download mode; flash the variant boot image (boot3 **only**, single partition):
      `mtk w boot3 boot3-hdmi-bringup.img`
- [ ] Verify: read boot3 back, compare SHA-256 against your Part-0 hash.
- **Risk:** replaces boot3 (backed up twice). **NOT** boot2, **NOT** the GPT. **Abort/recovery:**
  flash fails/hangs → power off, Esc + silver, retry; if the variant kernel won't boot, restore
  boot3 from Part 1 and boot Gemian (silver) to regroup.

## Part 3 — route the right port to HDMI

- [ ] Boot boot3 (silver + Esc). Log in (serial or USB-gadget SSH on the left port — the left
      port is unaffected by the right-port mux).
- [ ] Confirm the mux driver is present, then select HDMI:
      ```sh
      cat  /sys/devices/platform/rightport-mux/mode      # expect: usb-host (boot default)
      echo hdmi > /sys/devices/platform/rightport-mux/mode
      cat  /sys/devices/platform/rightport-mux/mode      # expect: hdmi
      ```
- **Risk:** right-port USB host goes away while in HDMI mode (expected — mutual exclusion).
  **Abort:** `echo usb-host > …/mode` to revert; nothing persists across reboot (default is
  usb-host).

## Part 4 — validate the display pipe (the actual checks)

Do these in order; each narrows where a failure is.

- [ ] **CRTC/connector present.** With the variant DTB the second CRTC + HDMI connector should
      exist:
      ```sh
      ls /sys/class/drm/                 # expect a second card/connector for HDMI
      cat /sys/class/drm/*/status        # the HDMI connector should read 'connected' with cable in
      modetest -c 2>/dev/null | grep -i -A2 'HDMI\|connector'   # if libdrm-tests present
      ```
      Internal DSI panel must still be up (it is a separate CRTC).
- [ ] **EDID reads over DDC.** Plug the 30-001-01 into a powered monitor:
      ```sh
      cat /sys/class/drm/card*-HDMI*/edid | wc -c        # non-zero → EDID read via the SiI9024A DDC
      # or: get-edid / edid-decode if available
      ```
      EDID proves the SiI9024A is alive and the monitor is seen — independent of scanout.
- [ ] **Standard mode scans out.** Attempt a modeset to a standard mode the EDID advertises
      (e.g. 1080p): `modetest -s <connector>:<mode>` (or let the compositor drive it). Expect a
      stable image on the monitor.
- [ ] **Internal panel unaffected** throughout (glance at the Gemini's own screen).
- **Capture (scrubbed):** `dmesg | grep -iE 'sii902x|mtk_dpi|mediatek-drm|dpi|dsc|drm'`,
  `/sys/class/drm/*/status`, the EDID byte count (NOT the raw EDID — it contains a serial),
  and any `modetest` output. Commit text-only to `04-docs/captures/<date>/`.

## Part 5 — first-divergence decision tree

- **CRTC/connector absent** → the variant DTB didn't take (wrong DTB packed, or a node stayed
  disabled). Re-check Part 0 step 1 and `dmesg` for mediatek-drm component-bind errors. Device-
  free fix; no reserve needed.
- **Connector present but EDID empty / no HPD** → SiI9024A not powered or DDC not up: check the
  bridge's reset/1V2 GPIOs and that the mux actually routed (Part 3). Still a wiring/enable
  check, not the reserve.
- **EDID reads but no scanout** → **this is the R3 check.** The board wires a **12-line
  dual-edge (DDR)** DPI bus, while mainline `sii902x.c` hardcodes **24-bit rising-edge** input.
  If the pixels don't come through, this format mismatch is the prime suspect. The device-free
  fix to try first (per #17): teach `sii902x` a 12-bit/dual-edge input mode
  (`SII902X_TPI_AVI_INPUT_BITMODE_12BIT`) paired with MT6797 DPI DDR programming; **only if that
  fails**, fall back to the vendor `sil9024` forward-port (**#17 reserve**).
  Also note: upstream `mtk_dsc_config` sets `DSC_DUAL_INOUT` unconditionally — review for the
  single ~1440-wide pipe if the DSC hop misbehaves.
- **Scanout works** → HDMI video-out is validated. Proceed to the follow-ons below.

## After this session

- **#19** — automatic HPD-gated mux switching (replace the static sysfs select with the vendor
  CC-attach → probe-HPD → mux behavior). Needs this validation first.
- **#20** — mediatek-drm atomic-KMS hardening (the `flip_done`/`vblank` wedge). Required before
  a dual-CRTC **extended desktop**; the device-free part is reviewing/backporting the candidate
  upstream MediaTek DRM fixes one at a time.
- **#21** — usable extended desktop (compositor multi-head), the Track 3 endpoint, after #20.
- Audio-over-HDMI, MHL, HDCP remain **deferred** (ADR-0003).
