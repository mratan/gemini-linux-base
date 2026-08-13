# First-session RUNBOOK — External display validation (USB display primary)

Tracker issue #18 (execution) / #29 (this revision, Slice U4). The ordered procedure for the
**first physical session** that validates an external display on the Gemini. **Since
ADR-0004 (2026-08-13) the primary route is the USB display** — a DisplayLink DL-165/DL-195
adapter on the right-port USB host, driven by upstream `udl` — because the Planet 30-001-01
cable the Native HDMI path requires is unobtainable. The HDMI sequence this runbook
originally carried is preserved, unchanged in substance, as **Appendix A**, and runs only if
a Planet cable is ever obtained.

Everything the device-free slices produced is staged: `udl.ko` + `evdi.ko` ride the
release-qualified image (Slice U1, `configs/gemini-usbdisplay.config`, fork b9655c5); the
adapter shortlist and the aarch64-DisplayLinkManager GO verdict are in
`04-docs/USB-DISPLAY-RESEARCH-2026-08-13.md` (gemini_linux repo, Slice U3). This runbook
only **validates** — it develops nothing new.

Track 3 always yields to the Internal Wi-Fi Prototype: if both are on the bench in one
session, do the Wi-Fi capture/bring-up first (`release/RUNBOOK.md`), then this.

**Status: NOT-YET-VALIDATED.** Every step below is unproven on hardware until a physical
session runs it and its captures land in `04-docs/captures/<date>/`.

---

## ⛔ GATE — do not execute any physical step yet

**Nothing here runs until the remote-only period is declared over and the device is in
hand.** Reading this document, building images, and buying the adapter are device-free and
fine. Anything that touches the device is not.

If you are reading this during the remote-only period, stop at the end of "Part 0".

---

## Standing safety rules (permanent — apply to every step)

- **Never `mtk wl`; never any operation that writes the GPT.** Single-partition targeted
  writes only (`mtk w boot3 …`).
- **Never flash or modify `nvram`/`nvdata`** (IMEI, Wi-Fi calibration/MAC).
- **Never flash boot2, never write the `linux` partition's Gemian rootfs** (ADR-0002).
  boot2 + Gemian is the Reference slot and must survive intact — it is also your fallback.
- Single-variable changes; every capture committed as text only, **after** scrubbing
  device-unique identifiers (MACs/IMEIs/EDID serials), to `04-docs/captures/<date>/`.

## Boot slots and recovery combo

| Slot | Key held at power-on | Contents |
|---|---|---|
| boot1 | none | rooted Android |
| boot2 | **silver** side button | Gemian 3.18.41 — **Reference slot, untouchable / fallback** |
| boot3 | **silver + Esc** | Experimental kernel this session |
| recovery | **Esc** | stock recovery |

**Stuck-DA recovery:** power off, hold **Esc + silver** to re-enter download mode, retry.
boot2 is never touched, so the device is never bricked by this runbook.

---

## Honest expectations (read before judging results)

- **1920×1080@60 is a display MODE, not a promise of 60 fps motion.** DisplayLink ships
  compressed framebuffer updates over USB 2.0 high-speed (480 Mbit/s shared bus). Desktop
  work (terminals, editor, browser) should feel fine; full-screen video will visibly lag.
- **Current measured host ceiling** (B-22 follow-up, 2026-07-20): PIO-only MUSB at
  ~43–64 Mbit/s TCP. DL-1x5 compression may still deliver a usable desktop at that rate;
  the throughput/robustness headroom work is Slice U2 (issue #27) — check its status
  before this session and note which (if any) opt-in U2 candidates are in the image.
- **Known residual risk:** a babbling/misbehaving USB device can wedge the MUSB host until
  reboot (the RTL8156 did exactly that). If the adapter turns out to be such a device,
  that is a *finding*, not a failed session — capture and move to U2.

## Part 0 — device-free prep (safe now)

1. **Image:** the standard release-qualified Experimental-slot image already contains
   `udl.ko` + `drm_shmem_helper.ko` + `evdi.ko` under `/lib/modules/<kver>/updates`
   (manifest section "USB-display modules") and the U5-staged userspace display stack:
   sway/foot/seatd + `modetest`, the launch helper `/usr/local/sbin/gemini-usb-display-start`,
   and `/etc/gemini/sway-usbdisplay.conf` (presence proven in CI by the
   `GEMINI-USBDISPLAY-USERSPACE-OK` marker). No variant DTB, no DT change, no mux change —
   the USB display needs none of them.
2. **Hardware to buy/bring** (from the U3 shortlist — chipset over brand):
   - Preferred: **Plugable UGA-165** (DL-165, documented 1080p60, DVI-I + passive
     DVI→HDMI adapter) — or the **Plugable UD-160-A dock** (same DL-165 + powered hub +
     AX88772A Ethernet in one box, the ideal desk topology).
   - A **powered** USB 2.0/3.0 hub if using a bare adapter alongside other peripherals
     (the right port's VBUS budget is ~500 mA; a DL-1x5 alone can draw all of it).
   - Monitor + DVI or HDMI cable; the mtkclient workstation; optional FTDI serial @921600.
3. **Know your fallback numbers:** note the release MANIFEST hashes for the image you
   bring; nothing in this session writes anything persistent to the device beyond the
   normal boot3 flash done per `release/RUNBOOK.md`.

> Everything below is **physical-session work — gated on the remote-only period being over.**

## Part 1 — boot and baseline

- [ ] Boot boot3 (silver + Esc) into the Experimental slot per `release/RUNBOOK.md`.
      Log in (serial or left-port gadget SSH; the left port keeps charging throughout).
- [ ] Baseline captures before plugging anything:
      ```sh
      lsusb -t                                  # right-port MUSB bus present, empty
      ls /sys/class/drm/                        # note existing cards (internal panel)
      cat /sys/devices/platform/rightport-mux/mode   # expect: usb-host (boot default)
      modinfo udl evdi | grep -E '^(filename|vermagic)'   # staged modules resolvable
      ```
- **Risk:** none. **Abort:** n/a.

## Part 2 — adapter enumerates at high speed

- [ ] Plug the DL-165 adapter (directly first; hub topology comes in Part 6) into the
      right port. Then:
      ```sh
      lsusb -t          # expect the DisplayLink device on the MUSB bus at 480M ("HS")
      lsusb -v -d 17e9: | head -40    # 17e9 = DisplayLink VID; note bcdDevice/chip
      dmesg | tail -30                # enumeration lines; NO babble/three-strikes
      ```
- [ ] **Confirm 480M.** If it enumerates at 12M (full-speed), stop and capture — that
      contradicts the dts/0019 state and is a finding in itself.
- **Capture:** `lsusb -t`, scrubbed `lsusb -v` header, dmesg excerpt.
- **Risk:** a babbling device can wedge the MUSB host (see Honest expectations).
  **Abort/rollback:** unplug; if the host is wedged, reboot — nothing persists.

## Part 3 — udl binds, a second DRM card appears

- [ ] `udl` should auto-load via modalias; if not, `modprobe udl` (modules.dep for the
      updates dir is depmod-verified at assembly time).
      ```sh
      dmesg | grep -i udl               # bind messages, no errors
      ls /sys/class/drm/                # expect a NEW cardN + cardN-DVI-I-1 (or HDMI-A)
      cat /sys/class/drm/cardN-*/status # 'connected' with a powered monitor attached
      ```
- [ ] Internal panel still alive (glance at the Gemini's screen).
- **Capture:** dmesg udl lines, `ls /sys/class/drm/`, connector status.
- **First-divergence pointers:** device enumerates but udl doesn't bind → wrong module
  staging (check `modprobe -n --show-depends udl` against the updates dir) or a non-DL-1xx
  chip (check `lsusb` VID/PID against the shortlist). Binds but no connector → capture
  dmesg and stop; that's an upstream-udl question, not a wiring one.

## Part 4 — EDID and modes

- [ ] ```sh
      cat /sys/class/drm/cardN-*/edid | wc -c   # non-zero → EDID read through the adapter
      modetest -M udl -c 2>/dev/null | sed -n '1,40p'   # connector + mode list
      ```
      Expect the monitor's standard modes; confirm 1920x1080@60 is listed (DL-165 spec).
- **Capture:** EDID byte count (NOT raw EDID — serial inside), mode list.

## Part 5 — pixels: console/pattern, then single-output desktop

- [ ] **Pattern or console first** (no compositor variables):
      `modetest -M udl -s <connector>:1920x1080@60` → stable test pattern on the monitor.
      Alternatively fbcon: `con2fbmap` the udl fbdev (udl exposes fbdev emulation) and
      check a VT appears.
- [ ] **Single-output desktop** on udl alone: run `gemini-usb-display-start` (U5-staged;
      it finds the udl DRM card, pins wlroots to it via `WLR_DRM_DEVICES` with the pixman
      renderer, and launches sway + foot from `/etc/gemini/sway-usbdisplay.conf`). Do
      **not** hand-start a compositor without that pin: an unpinned wlroots will open the
      mediatek-drm card too and can hit the #20 atomic-KMS wedge on the internal panel.
      If the helper refuses the seat over SSH (flagged open question), retry from the
      serial/panel console and capture — don't improvise around it.
- [ ] **Sustained-use soak (the U2-relevant observation):** 15+ minutes of terminal
      scrolling/editor use on the external screen; watch `dmesg -w` for URB errors,
      resets, babble; note subjective update latency.
- **Capture:** modetest invocation + result, soak notes, any dmesg noise.
- **Risk:** worst case the MUSB host wedges → reboot restores; nothing persistent.

## Part 6 — desk topology: powered hub / dock

- [ ] Repeat Parts 2–5 with the adapter (or UD-160-A dock) behind the powered hub, plus a
      keyboard/mouse and (if present) the dock's Ethernet. Confirm: display + input +
      network coexist on the one MUSB bus; left-port charging unaffected
      (`bq25890` still reports Charging).
- **Capture:** `lsusb -t` of the full tree, coexistence notes, any bandwidth pain.

## Part 7 — extended desktop (endpoint attempt — expected partial)

- [ ] Attempt the two-DRM-device extended desktop (mediatek-drm panel + udl) with the
      staged compositor. **Expected to be partial**: the internal panel currently runs
      fbdev (the atomic-KMS wedge, #20), and multi-GPU compositing is the #21 endpoint
      work. Any result here is bonus data for #20/#21 — capture, don't debug live.
- **Capture:** compositor logs, which outputs lit, dmesg.

## After this session

- **#27 (U2)** — feed it: the soak results, any babble events, throughput feel. Its
  opt-in candidates (babble recovery, double-buffered FIFOs, DMA) get their own
  before/after test using Part 5's soak as the baseline.
- **#20 / #21** — atomic-KMS hardening and multi-GPU extended desktop, fed by Part 7.
- **#19** — HPD-gated mux switching: **Native HDMI branch only** (Appendix A); irrelevant
  to the USB display (the mux stays in usb-host).
- **evdi/DisplayLinkManager (secondary route)** — only if a modern DL-3xxx adapter is
  ever bought: `evdi.ko` is already staged; the aarch64 DisplayLinkManager 6.3.0-48
  install is documented in the U3 research doc. Not part of the first session.

---

## Appendix A — Native HDMI validation (research branch; needs a Planet 30-001-01 cable)

**Run only if a Planet 30-001-01 cable is obtained** (ADR-0004 demoted this path; ADR-0003
still governs its internals). Preserved from the pre-regroup runbook, substance unchanged.

### A.0 — device-free prep

The HDMI pipe is **opt-in**: the default `mt6797-gemini-pda.dtb` keeps every HDMI-pipe node
disabled. HDMI bring-up uses the **variant** DTB `mt6797-gemini-pda-hdmi.dtb`
(`patches/v6.6/dts/0028`), which enables
`disp_ovl1/disp_rdma1/disp_dsc0/dpi0/sii9022/hdmi_connector`.

1. Build the HDMI-variant boot3 image: same kernel build as the release path, but pack
   `mt6797-gemini-pda-hdmi.dtb` as the appended DTB (`scripts/bootimg.py`). Name it
   `boot3-hdmi-bringup.img`, NOT-YET-FLASHED.
2. Mux command (from #16): `echo hdmi > /sys/devices/platform/rightport-mux/mode`.
3. Bring: the variant image + hash, the Planet 30-001-01 cable, monitor + HDMI cable,
   mtkclient workstation, optional FTDI serial.

**Mux exclusivity (this appendix only):** HDMI-out and right-port USB host are mutually
exclusive (GPIO72 interlock) — selecting HDMI takes the right-port USB (and any USB
display on it) away. Expected, not a fault. Default at boot is usb-host; nothing persists.

### A.1 — back up the current boot3

- Confirm the UBports TWRP backup of boot3 exists (`/home/gemini/boot3-twrp-backup.img` on
  Gemian); read back live boot3 via mtkclient (**read only**):
  `mtk r boot3 boot3-preHDMI-$(date +%Y%m%d).img` — keep outside git.

### A.2 — flash the HDMI-variant kernel to boot3

- Download mode; `mtk w boot3 boot3-hdmi-bringup.img` (boot3 **only**); read back and
  compare SHA-256. Recovery: restore Part A.1 image; Gemian (silver) always boots.

### A.3 — route the right port to HDMI

```sh
cat  /sys/devices/platform/rightport-mux/mode      # expect: usb-host (boot default)
echo hdmi > /sys/devices/platform/rightport-mux/mode
cat  /sys/devices/platform/rightport-mux/mode      # expect: hdmi
```
Revert with `echo usb-host > …/mode`; nothing persists across reboot.

### A.4 — validate the display pipe

In order; each narrows a failure:

1. **CRTC/connector present** (`ls /sys/class/drm/`, connector `status` reads `connected`
   with cable in; `modetest -c`). Internal DSI panel must still be up (separate CRTC).
2. **EDID reads over DDC** (`cat /sys/class/drm/card*-HDMI*/edid | wc -c` non-zero) —
   proves the SiI9024A is alive independent of scanout.
3. **Standard mode scans out** (`modetest -s <connector>:<mode>`, e.g. 1080p) — stable
   image on the monitor; internal panel unaffected throughout.

Capture (scrubbed): `dmesg | grep -iE 'sii902x|mtk_dpi|mediatek-drm|dpi|dsc|drm'`,
connector statuses, EDID byte count (not raw), modetest output.

### A.5 — first-divergence decision tree

- **CRTC/connector absent** → variant DTB didn't take (wrong DTB packed / node disabled).
  Check A.0 and dmesg component-bind errors. Device-free fix; no reserve needed.
- **Connector present, EDID empty / no HPD** → SiI9024A not powered or DDC not up: check
  the bridge's reset/1V2 GPIOs and that the mux actually routed (A.3).
- **EDID reads but no scanout** → **the R3 check.** The board wires a **12-line dual-edge
  (DDR)** DPI bus; mainline `sii902x.c` hardcodes **24-bit rising-edge** input. Device-free
  fix first (per #17): teach `sii902x` a 12-bit/dual-edge input mode
  (`SII902X_TPI_AVI_INPUT_BITMODE_12BIT`) paired with MT6797 DPI DDR programming; **only if
  that fails**, the vendor `sil9024` forward-port (**#17 reserve**). Also: upstream
  `mtk_dsc_config` sets `DSC_DUAL_INOUT` unconditionally — review for the single
  ~1440-wide pipe if the DSC hop misbehaves.
- **Scanout works** → Native HDMI video-out validated; #19 (HPD mux) becomes relevant
  again, and audio/MHL/HDCP remain deferred (ADR-0003).
