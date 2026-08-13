# Right-port MUSB (usb11): vendor vs mainline divergence — HS bulk throughput & babble robustness

Date: 2026-08-13. Slice U2 (tracker issue #27, PRD #25, ADR-0004). Device-free line-level
comparison of the two drivers for the same Mentor MUSB IP instance (MAC 0x11200000, U2 PHY
SIF 0x11210800), feeding the opt-in candidate patches in `patches/v6.6/{usb,phy,dts}` and
the on-device test plan below. **Everything here is NOT-YET-VALIDATED on hardware** — the
candidates compile and are argued from evidence, nothing more.

Sides compared:
- **Vendor** (HS + Inventra DMA worked on hardware): UBports 3.18.60
  `drivers/misc/mediatek/usb11/` ("musbfsh"), Gemini build config `CONFIG_MTK_USBFSH=y`
  with ICUSB/DT-USB/QMU all unset — the non-ICUSB code paths are what actually ran.
- **Mainline** (HS works adapter-dependently, PIO-only, ~43–64 Mbit/s, babble wedges the
  host): linux 6.6 `drivers/usb/musb/` + `mediatek.c` glue (patched by `usb/0002`) +
  `phy-mtk-tphy.c` (patched by `phy/0001`), `CONFIG_MUSB_PIO_ONLY=y`.

Hardware baseline this analysis explains (B-22 follow-ups, boot.md build #269, 2026-07-20):
HS enumeration + >200 MB clean iperf3 with a SZNX cdc_ether; 43/51 Mbit/s single-stream,
40/64 Mbit/s -P4, heavy downlink TCP retransmits (device RX overruns under PIO +
single-buffered 512 B FIFOs); an RTL8156 babbled at t≈131 s and **wedged the host until
reboot** (no re-enumeration on replug).

Register-bank note: vendor PHY access is 8-bit at `0x11210800+off`; mapping to tphy's
32-bit registers is `reg = off & ~3`, `bit = (off & 3)*8 + b`. The mapping confirms this
bank **is** the tphy-v1 u2 layout — vendor and mainline program the same silicon.

## 1. Where the two sides agree (ruled out as causes)

- **FIFO/EP config**: `usb/0002`'s `num_eps=6` + EP1–5 512 B single-buffered table now
  matches `musbfsh_config_mt65xx` + `epx_cfg` exactly. Confirmed correct by build #254.
- **multipoint=true is right**: vendor config says `multipoint=false` but
  `musbfsh_start()` runtime-forces `is_multipoint = 1` (mt6797/musbfsh_core.c:1260) and
  uses the per-EP FUNCADDR bank at 0x480 — the vendor *ran* multipoint. Explains build
  #253's regression when we tried `multipoint=false`; vindicates usb/0002.
- **Inventra DMA programming is register-identical**: same 0x200 bank, 8 channels,
  INCR16 burst, raw 32-bit physical address into ADDR, and the same MTK-specific unmask
  write (0x203 `UNMASK_SET=0xff`; mainline glue does the equivalent 32-bit write in
  `mtk_musb_init`). **No AHB/EMI/bus-performance/limiter registers exist anywhere in
  usb11** — nothing vendor programs before enabling DMA that mainline doesn't. The build
  #251 DMA crash is therefore *sequencing/interaction*, not a missing magic register;
  see §4.
- **IRQ**: same line (GIC SPI 73), same level-low sense, same INTRUSBE=0xf7, same L1INTM.

## 2. Divergences (ranked as babble/throughput candidates)

### C1 — Toggle handling: mainline can set a data toggle but never clear it  ⭐ top pick
`mediatek.c:mtk_musb_set_toggle` does `value = readw(TOG); value |= toggle << epnum;
writew(...)` — **OR-only**. The vendor writes toggles absolutely, using the TOGEN high
half as a per-EP write-enable (`musbfsh_host.c:522-529`:
`writel(TOG, ((1<<ep)<<16) | (tog<<ep))`).
Failure mode under mainline: after any URB dequeue/error/re-enumeration where usbcore
resets an endpoint's toggle to DATA0, the stale DATA1 bit in TXTOG/RXTOG can never be
cleared → persistent PID mismatch on that bulk endpoint → RX discards / re-IN storms
("three-strikes") and TX stuck-TXPKTRDY, escalating to babble. This shape matches the
observed RTL8156 wedge (babble after minutes of bulk, unrecoverable without reboot).
**Candidate patch: `usb/0003`** — masked absolute write, DT-gated
(`mediatek,quirk-vendor-toggle`).

### C2 — Babble policy inversion: vendor tolerates, mainline collapses the session
Vendor production path (mt6797/musbfsh_core.c:1195-1205): if DEVCTL has FSDEV|LSDEV set
(a device attached — FSDEV also covers HS), babble is **logged and ignored, session
kept**. (The ICUSB PHY-level babble clear/recover exists but is compiled out.)
Mainline (`musb_core.c` `musb_handle_intr_reset` → `musb_recover_from_babble`): every
babble = drop session, `musb_root_disconnect()`, re-init — and the mediatek glue defines
no `.recover` op. So even a survivable marginal event becomes a visible bus collapse; and
per the #269 wedge, the collapse is not even recovering cleanly on this IP.
**Candidate patch: `usb/0004`** — `babble_keep_session` config flag honored in
`musb_handle_intr_reset` (log-only when a device is attached, vendor semantics), DT-gated
(`mediatek,babble-keep-session`).

### C3 — HS TX slew calibration reads the wrong FM block on this SoC
tphy-v1 does slew calibration against a frequency-meter block at `sif + 0x100`
(`phy-mtk-tphy.c`), but the vendor proves MT6797 usb11's FM block lives at `sif + 0xf00`
(`musbfsh_mt65xx.c:720-759`). Vendor code moreover hardcodes the timeout flag and always
lands `HSTX_SRCTRL = 4`. Under mainline, whatever `0x1121010c` happens to read becomes the
calibration input: 0 → fallback 4 (lucky match), garbage → wrong HS TX slew — a
per-device-marginality knob, consistent with "one adapter fine, another babbles".
**Candidate: pure DT** — `mediatek,eye-src = <4>` on `u2port1` skips calibration and
reproduces the vendor value deterministically (already supported by tphy).

### C4 — Unreplicated PHY tuning write: u2 bank +0x70 bits[19:16] = 0x9
`musbfsh_mt65xx.c:1088-1089` (`CLR8(0x72,0x0F); SET8(0x72,0x09)`) — done in the only
vendor bring-up path, immediately before slew cal; no tphy define touches offset 0x70.
Unnamed field, plausibly HS RX/discriminator tuning. Not patched (we refuse to poke an
unnamed register blind); **test-plan step**: devmem-diff `0x11210870` under the Reference
slot's vendor kernel vs mainline, and only if it differs, consider a tphy patch.

### C5 — `CLK_INFRA_SSUSB_REF` never enabled by the mainline right-port path
Vendor enables two gates before PHY init: `infra_icusb` **and** `sssub_ref_clk`
(= `CLK_INFRA_SSUSB_REF`, the 26 MHz ref gate). Mainline `dts/0014` gives the musb node
only `CLK_INFRA_ICUSB` ("main"); nothing in the right-port path claims the ref gate — if
LK or the left-port path leaves it off (or the clk framework disables it as unused), the
U2 PLL reference is compromised: "enumerates (chirp works) but sustained HS data is
marginal" fits that shape.
**Candidate: pure DT** — add it as the glue's optional "mcu" clock (mediatek.c already
takes "mcu"/"univpll" as optional NULL-tolerant clocks per usb/0002).

### C6 — phy/0001 holds FORCE_SUSPENDM forever; vendor releases it to the MAC
`phy/0001`'s `force-usb-host` sets and holds `P2C_FORCE_SUSPENDM|P2C_RG_SUSPENDM`
(needed at bring-up: on clean LK handover the PHY analog is suspended). The vendor's
recover path instead *releases* force_suspendm at the end (`musbfsh_mt65xx.c:1046`) so
the MAC drives suspendm, and pairs it with `POWER.ENSUSPEND` on the MAC. Permanently
forcing the PHY to ignore MAC suspendm can perturb HS reset/suspend sequencing mid-
session. Lower probability (enumeration works), trivially testable.
**Candidate patch: `phy/0005`** — after the force-usb-host block, optionally release
FORCE_SUSPENDM/RG_SUSPENDM, DT-gated (`mediatek,force-suspendm-release`). Note the MAC
`ENSUSPEND` half is deliberately NOT replicated yet (musb_start composes POWER
differently — vendor RMWs `HSENAB|SOFTCONN|ENSUSPEND`, mainline writes
`ISOUPDATE[|HSENAB]`); revisit only if C6 tests positive.

### C7 — Settle delays (not patched; test-plan awareness)
Vendor: `udelay(800)` after session-force, `mdelay(500)` after MAC start. Mainline: none.
Marginal-analog settling; if candidates above don't move the babble threshold, add delays
as a follow-up experiment rather than cargo-culting them in now.

## 3. Throughput ceiling (~43–64 Mbit/s) — what would lift it
The measured ceiling is PIO (`CONFIG_MUSB_PIO_ONLY=y`, CPU-copied transfers on A53) +
single-buffered 512 B FIFOs (device RX overruns → TCP retransmits). Options, in
increasing risk order:
1. **Double-buffered bulk FIFOs**: config-table change (`BUF_DOUBLE` on the bulk EPs)
   within the same 8 KB FIFO RAM (current table uses 5184 B of it; doubling EP1–2 TX/RX
   to 1024 B each adds 2 KB — fits). Vendor shipped single-buffered, so this goes BEYOND
   vendor — treat as its own experiment, opt-in, after C1/C2 are settled.
2. **Inventra DMA re-attempt**: programming is register-identical to vendor (§1), so the
   #251 crash likely came from interaction with the then-broken FIFO/EP config (mainline
   MT8516-shaped table, pre-#254) — DMA channels programmed against EP FIFO addresses
   that didn't exist wholesale. A re-attempt AFTER C1 lands, with the correct FIFO table,
   is justified; keep `MUSB_PIO_ONLY` the build default and gate the attempt to a
   dedicated experiment image.
For the USB-display workload (bulk-OUT dominant), note the asymmetry: TX (OUT) measured
51-64 Mbit/s — the display direction is the better one, and DL-1x5 compression targets
tens of Mbit/s for desktop updates. C1/C2 robustness matters more than raw throughput.

## 4. The opt-in mechanism (defaults unchanged)
All four candidates activate **only** via the new opt-in variant DTB
`mt6797-gemini-pda-usbexp.dtb` (`dts/0029`), mirroring the HDMI-variant pattern: the
default board DTB and the release image behavior are untouched; an experiment session
packs the variant DTB into a clearly-named boot image. The variant sets:
`mediatek,quirk-vendor-toggle` + `mediatek,babble-keep-session` (+ the SSUSB_REF second
clock) on `&usb1`, and `mediatek,eye-src = <4>` + `mediatek,force-suspendm-release` on
`&u2port1`. Individual candidates can be isolated by deleting properties from the variant
and rebuilding — single-variable discipline.

## 5. On-device test plan (physical-session; do AFTER the Wi-Fi session)
Baseline first, then one variable at a time. Capture: `usbmon` (bus 1), `dmesg -w`,
`ip -s link` error counters, iperf3 numbers. Rollback for every step: reboot on the
default (non-variant) boot image — nothing persists.

1. **Baseline re-measure** (default DTB): SZNX cdc_ether iperf3 up/down + 15-min soak.
   Numbers should reproduce #269 (43/51, 40/64 -P4). Also devmem-read the C4 register
   `0x11210870` and `0x11210800+0x115` region (HSTX_SRCTRL byte) — record both.
2. **Reference-slot cross-read** (Gemian, vendor kernel): devmem the same two registers →
   settles C3 (is HSTX_SRCTRL 4?) and C4 (is 0x70[19:16] 9?) empirically.
3. **Variant DTB, full set**: same iperf3 + soak. If the RTL8156 is on the bench, plug it
   — under C2 it should now log babble without wedging; under C1 it may not babble at
   all. A wedge that survives = candidates insufficient, capture and stop.
4. **Isolate winners**: if (3) differs from (1), rebuild the variant minus one property
   at a time (single-variable) to attribute the change; at minimum separate {C1,C2} from
   {C3,C5,C6}.
5. **Babble-recovery check** (C2 specifically): induce disconnect-under-load (yank the
   adapter mid-iperf3) and verify the host re-enumerates a replugged device without
   reboot.
6. **Display-workload proxy** (pre-adapter): sustained bulk-OUT at display-like rates —
   `dd` to a USB mass-storage stick through the hub for 10+ min, watch for
   three-strikes/babble. With the DL-165 in hand, substitute the runbook Part 5 soak.
7. **Only after C1/C2 verdicts**: schedule the DMA re-attempt and/or double-buffer FIFO
   experiments (§3) as separate sessions.

## 6. Falsifiables
- If step 2 shows vendor HSTX_SRCTRL ≠ 4 or 0x70[19:16] ≠ 9 on live hardware, C3/C4's
  premises are wrong — drop them.
- If step 3 shows no change vs baseline with the full variant, C1–C6 are collectively
  insufficient for the RTL8156-class failure; escalate to C7 delays and a usbmon-level
  comparison against the Reference slot.
- If the wedge persists even with C2's keep-session, the wedge is below the babble
  handler (MAC/PHY state), and the ICUSB-only PHY babble clear/recover sequence
  (musbfsh_mt65xx.c:237-255) becomes the next port candidate.
