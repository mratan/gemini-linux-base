# blockers.md — Known Blockers, Issues and Risks

Consolidated from hardware.md, driver_ports.md, code_review/findings.md and
the archive. One entry per blocker, with what unblocks it. Maintained per the
CLAUDE.md documentation requirements.

**Status legend:** 🔴 blocking the current milestone · 🟡 blocking a later
milestone · 🟢 resolved (kept for history)

---

## Operating decision: driver-work freeze (2026-06-10, LIFTED 2026-07-04)

**No new driver code until first serial output on hardware.** Six subsystems
were already "code complete" against a device that had never booted anything
newer than 3.18, and several carry verification-blocked findings that only
hardware or datasheets can clear. While the freeze was in effect, the only
permitted work was: documentation, evidence extraction (vendor DTB / spec
PDF / boot images), Phase-3 build/packaging scripts, and fixes to *existing*
patches required for the minimal boot (e.g. B-4). Rationale: fable-report.md §4.3.

**Lifted 2026-07-04** — see B-1 (resolved) and boot.md's "First Clean Serial
Capture" entry. First serial output on hardware has been achieved (stock
Android boot chain, over the now-working FTDI rig). Driver work may resume.
Note: the previously "code complete" driver subsystems are still unverified
against real hardware running Linux 6.6 (that requires B-2, the first 6.6
boot) — resuming work does not retroactively validate them.

---

## 🟢 B-1 — FTDI serial cable not yet arrived (RESOLVED 2026-07-04)

The primary Phase 3 blocker. All hardware verification was gated on it.
- **Resolution:** cable arrived; a 1.8V/3.3V-selectable adapter was initially
  wired at 1.8 V (matching the SoC's native pad voltage but wrong for the
  USB-C mux path, which rides standard 3.3 V USB D+/D− logic — see
  kernel.md). Switching the adapter to 3.3 V produced a fully clean capture
  of the stock Android preloader → LK → ATF boot chain at 921600 baud on the
  first attempt. This also retroactively confirms the 2026-06-12 garbled
  captures (see boot.md) were caused by signal-level mismatch, not a wiring
  or baud fault.
- **Evidence:** `logs/2026-07-04-01-first-serial-attempt.log`; full writeup
  in [boot.md](boot.md) under "First Clean Serial Capture — console
  confirmed working (2026-07-04)".
- **Unblocked:** the driver-work freeze condition ("first serial output on
  hardware") is now satisfied. Next: flash a Linux 6.6 `boot2` image and
  capture its console the same way (see B-2).

## 🟢 B-2 — LK → mainline kernel handoff unverified (RESOLVED 2026-07-04)

Everything assumes LK will load and start a 6.6 `Image.gz` + appended DTB the
way it boots the 3.18 image. Compounding risk: the archive records a rebuilt
3.18 kernel — byte-identical DTB, identical packaging and load addresses, even
the identical GCC 4.9 toolchain — that still failed to boot, **root cause never
found** (`archive/progress2.md` session 2). Whatever killed that build may kill
a 6.6 build identically.
- **Treat the first 6.6 flash as an experiment about LK, not about Linux 6.6.**
- **Next steps for the 6.6 flash attempt (unblocked by B-1, 2026-07-04):**
  1. ✅ Build: already done and validated 2026-06-10 (commit `19e91dc`) —
     `Image.gz` + `mt6797-gemini-pda.dtb` + full modules, 0 errors, against
     clean `~/linux-6.6` (`v6.6`) with the current patch set. Outputs live in
     `~/gemini-build/OUTPUT/` on the Mac host (not committed — build
     artifacts). No patch changes since, so no rebuild was needed.
  2. ✅ Packaged into a `boot.img` (2026-07-04) with the new
     `scripts/pack-boot-img.py`: copies the header and ramdisk byte-for-byte
     from `planet/kali_boot.img` (verified by direct hex inspection to be a
     plain AOSP v0 boot header, no MTK wrapper — kernel_addr `0x40080000`,
     ramdisk_addr `0x45000000`, tags `0x44000000`, page size 2048, all
     matching boot.md), and substitutes only the kernel blob
     (`Image.gz` + appended DTB). Result: `logs/2026-07-04-02-first-6.6-flash/new_kali_boot.img`.
  3. ✅ Provenance recorded — see boot.md "6.6 boot.img Packaged" entry for
     kernel tag/commit, patch commit, and checksums. `.config` copied to
     `logs/2026-07-04-02-first-6.6-flash/config`.
  4. ⬜ **Next (requires hands on the hardware — not automatable):** start
     the capture first — `scripts/ftdi-monitor.py --log
     logs/2026-07-04-02-first-6.6-flash.log` (proven rig from B-1: 3.3 V
     adapter, VBUS on the 5 V pin) — before plugging/powering the device.
  5. ⬜ Flash only `boot2`: `mtk w boot2
     logs/2026-07-04-02-first-6.6-flash/new_kali_boot.img` (never `mtk wl`).
  6. ⬜ Power on and capture. Compare against the B-1 baseline
     (`logs/2026-07-04-01-first-serial-attempt.log`): same preloader/LK
     preamble is expected; divergence starts wherever LK either fails to
     load/jump to the 6.6 image, or the 6.6 kernel itself goes silent after
     the jump — that divergence point is the diagnostic signal.
  7. ✅ Result added to boot.md ("First 6.6 Flash — flashed, captured,
     silent after el3_exit", 2026-07-04): preloader→LK→ATF handoff is
     byte-identical to the B-1 baseline, then silent after `el3_exit` —
     **but the B-1 stock-kernel baseline goes silent at the exact same
     point**, so this is inconclusive, not a failure. Root cause of the
     silence (both boots): LK injects its own cmdline
     (`console=ttyMT0,921600n1 ... printk.disable_uart=1`) at handoff,
     `ttyMT0` isn't a mainline console name, and our `.config` has
     `CONFIG_CMDLINE=""` with no `FORCE` flag, so nothing overrides it.
  8. ✅ Rebuilt with the cmdline force fix, reflashed, recaptured — **same
     exact silence at `el3_exit`**, a third identical result. This falsified
     the console-naming hypothesis (earlycon prints within the first few
     instructions of `start_kernel`, before any cmdline-dependent logic
     could matter) and pointed to something more fundamental: the CPU isn't
     executing our kernel's code at all after the jump.
  9. ✅ **Root cause found** (see boot.md "Cmdline Fix Retested" entry,
     2026-07-04): our packaged boot.img's `kernel_addr` (`0x40080000`,
     copied from the vendor header) is `dram_base + 0x80000` — correct
     under the vendor kernel's *old* (pre-v4.6) boot protocol, but **not
     2MB-aligned**, which the *modern* protocol our 6.6 kernel uses
     (`PHYS_BASE=1` flag, unconditional in all mainline arm64 kernels since
     v4.6 — confirmed this is not a `CONFIG_RELOCATABLE`/`RANDOMIZE_BASE`
     effect) strictly requires. Loading a modern kernel at a
     non-2MB-aligned address is a protocol violation — silent crash,
     matching all 3 identical failures exactly.
  10. ✅ **Retested and falsified** (boot.md "Aligned kernel_addr Retested",
      2026-07-04): flashed the 2MB-aligned repackage — identical silent
      failure, and the log proved LK **ignores the boot.img header's
      `kernel_addr` field entirely**, always loading at the hardcoded
      `0x40080000`. The packaging-layer fix could never have worked.
  11. ✅ **Real root cause identified**: since LK cannot be redirected via
      the header, the kernel itself must self-relocate off the
      non-2MB-aligned `0x40080000` — which is exactly what
      `CONFIG_RELOCATABLE=y` (arch/arm64 default) does. The prior session's
      fix had **disabled** `CONFIG_RELOCATABLE` while chasing the unrelated
      `text_offset=0x0` red herring, removing the only mechanism that makes
      booting at this address possible. That is the most likely true cause
      of every silent failure so far.
  12. ✅ Reverted `configs/gemini-cmdline.config` to leave
      `CONFIG_RELOCATABLE`/`CONFIG_RANDOMIZE_BASE` at defconfig defaults;
      kept `CONFIG_CMDLINE_FORCE`. Rebuilt clean in VM
      (`~/build-6.6-reloc-restored.log`). Repackaged at the original,
      LK-honored `kernel_addr=0x40080000` (no override):
      `logs/2026-07-04-05-relocatable-restored/new_kali_boot.img`, sha256
      `c8bb8f6bbf13b434efb351ca48d9a41dddfd7dec18c07c2d920cb24db7f43134`.
  13. ✅ Retested with RELOCATABLE restored — still identical silence after
      `el3_exit`. Falsified.
  14. ✅ **Baseline test** (boot.md "Vendor Baseline", 2026-07-04): flashed
      the unmodified vendor `planet/kali_boot.img` and captured a full run.
      Also silent after `el3_exit` — but this doesn't clear 6.6, since the
      vendor cmdline's `printk.disable_uart=1` (a 3.18-fork-only parameter,
      not implemented in mainline) is a known, deliberate cause unrelated to
      our kernel. **We have never had a real baseline of successful
      post-el3_exit console output on this hardware.**
  15. ✅ Found our explicit-address `earlycon=uart8250,mmio32,0x11002000`
      always uses the generic 8250 early driver, never the MediaTek-specific
      `early_mtk8250_setup` (only reachable via DT-node match on uart0's
      compatible string, confirmed `"mediatek,mt6797-uart"` /
      `"mediatek,mt6577-uart"` at `0x11002000`). Switched to the bare
      DT-node `earlycon` form, rebuilt, repackaged:
      `logs/2026-07-04-07-mtk-earlycon/new_kali_boot.img`, sha256
      `37a64a6d14b05f0f5aa8cbe18cd81647a92750844ae25dd45d12b7d9f19bc166`.
  16. ✅ Retested (power-button boot, not USB charge-mode) — still identical
      silence. Rules out boot mode as a variable.
  17. ✅ **Pivotal result** (boot.md "Pivotal Result: Silence After el3_exit
      Is Not a Failure Signal", 2026-07-04): flashed the unmodified vendor
      `planet/kali_boot.img`, held the power button, and the device **fully
      booted to the Android desktop UI — user-confirmed, visually
      verified.** The serial capture still ends at the exact same
      `el3_exit` point as every other attempt, with **zero output** despite
      the confirmed-successful boot. **This invalidates the diagnostic
      method used in steps 9-16**: silence after `el3_exit` carries no
      information about success or failure on this hardware/rig — it is
      what every boot looks like, vendor or mainline, working or not. The
      `CONFIG_RELOCATABLE` and earlycon-driver fixes may or may not be
      correct in principle, but the "still silent" results used to argue
      about them were never valid evidence either way.
  18. ✅ **ACTUAL ROOT CAUSE OF EVERYTHING ABOVE** (boot.md "ROOT CAUSE OF
      ALL 2026-07-04 SILENT RESULTS", 2026-07-04): after flashing 6.6 to
      `boot2`, a plain power-button boot **booted normally into Android** —
      and re-checking every capture from today shows LK loading
      `partition boot` (stock Android) in every single run. **`boot2` was
      never booted; our 6.6 kernel has never executed.** Plain power-button
      and USB-plug boots select OS 1; `boot2` needs the Gemini multi-boot
      button combo (left silver button + power — confirm exact combo).
      The uniform silence is fully explained by the stock Android kernel
      honouring `printk.disable_uart=1`. Steps 7-17's hypotheses and
      "results" are all void (never actually tested); the DEVAPC theory is
      withdrawn. Kept config changes (CMDLINE_FORCE + bare earlycon,
      RELOCATABLE at defaults) are reasonable but unexercised.
  19. ⬜ **Next (the real first 6.6 boot attempt):** `boot2` already holds
      the mtk-earlycon 6.6 image. Start a capture
      (`logs/2026-07-04-09-first-real-6.6-boot.log`), power on with the
      **boot2 combo** (silver + power), and verify the capture shows
      `Loading DTB from partition boot2` before interpreting anything —
      that line is now a mandatory validity check for every run.
      Optional sanity first: combo-boot the vendor image to prove the combo
      and finally capture a real successful Kali serial boot.
- **Diagnostic ladder if the first boot is silent** (decide now, not then):
  1. Re-verify the cable against the known-good 3.18 boot (B-1 first action).
  2. `earlycon` variants (explicit `earlycon=uart8250,mmio32,0x11002000`).
  3. Check whether LK itself prints on the UART / rejects the image (size
     limit, header fields).
  4. Bisect the DTS to an absolute minimum (cpus + memory + uart only).
  5. Read ramoops/pstore from Android after the failed boot
     (`/sys/fs/pstore`) — the board DTS now places ramoops in the region
     Android already maps.
- **Unblocks:** B-1, then first flash.

- **RESOLVED 2026-07-04.** After step 19, the strategy changed: rather than
  chase the `boot2` combo, the 6.6 test kernel was flashed to the **default
  `boot` slot** (boots on plain power-on, no button combo) — see boot.md's
  "FIRST 6.6 ATTEMPT" through "SEVENTH RESULT" entries and CLAUDE.md's
  current-decisions note. From there, LK's DTB pre-jump fixups were peeled
  off one panic at a time (each one a hard vendor-DTB-shape dependency in
  `mt_boot.c`, confirmed against `docs/vendor-dtb/gemini_kali_boot.dts`):
  1. cpu `clock-frequency` on all 10 cpu nodes (else infinite loop in
     `target_fdt_cpus`).
  2. `mediatek,mt6797-atf-ramdump-memory` reserved-memory compatible (else
     "Can not find atf ram dump!" panic).
  3. `mediatek,scp` **device** node (compatible `mediatek,scp`, root-level,
     `status="disabled"` — no 6.6 driver drives it) — LK's
     `platform_fdt_scp()` looks this up and patches its status before
     handoff; a `scp-share` *reserved-memory* node alone was NOT sufficient,
     the device node was the actual fix.
  With all three in place, LK printed `[LK]jump to K64 0x40080000` for the
  first time (`logs/2026-07-04-21-scp-node-boot.log`) — **the LK→mainline
  handoff is now proven working.** Two further kernel-side (not LK-side)
  issues were found and fixed via `CONFIG_CMDLINE`
  (`configs/gemini-cmdline.config`):
  4. 🟡 **NARROWED 2026-07-06, root cause reclassified 2026-07-06** — SMP
     secondary-CPU PSCI bringup hang, worked around with `maxcpus=1`/now
     `maxcpus=8` (not fully resolved — full 10-core SMP is now tracked as
     **B-16**, not B-13, see below). PSCI `CPU_ON` instrumentation
     (`arch/arm64/kernel/psci.c`, boot.md "PSCI CPU_ON diagnostic") showed
     CPU0–7 (both Cortex-A53 clusters) bring up cleanly in ~35ms; the hang is
     specifically at CPU8, the first core of the third cluster (2x
     Cortex-A72). Its PSCI `CPU_ON` SMC never returns — ATF firmware itself
     hangs, not a Linux-side defect. The original 2026-07-04 hang (which
     stalled at CPU1, before any clk fix existed) is now believed to have
     actually been the clk_ignore_unused bug wearing a different hat, not a
     distinct CPU1 defect — `maxcpus=2` alone boots CPU1 cleanly today
     (boot.md, `logs/2026-07-06-73-psci-cpu1-diag/`).
     `configs/gemini-cmdline.config` now uses `maxcpus=8` (validated
     `logs/2026-07-06-77-maxcpus8/`, boot.md "BUILD — maxcpus=8"): all 8 A53
     cores online, clean boot to `systemctl is-system-running` = `running`.
     Full SMP (cpu8/9) was originally assumed to share B-13's root cause but
     **this was disproven 2026-07-06** (boot.md "BUILD #11") — see B-16.
  5. 🟢 **RESOLVED 2026-07-06** — real root cause found and fixed (was:
     kernel hangs at `clk: Disabling unused clocks`, worked around with
     `clk_ignore_unused`). Full diagnosis and validation: boot.md "BUILD
     #62/#65/#67/#69". Summary: the clock that hangs is `infra_uart0` — the
     debug console's own baud clock. `drivers/tty/serial/8250/8250_mtk.c`
     fetches it with plain `devm_clk_get()` and only reads its rate, never
     enabling it (unlike the `"bus"` clock in the same function, which
     correctly uses `devm_clk_get_enabled()`). The hardware clock is left
     running by the bootloader, but Linux's own `enable_count` stays 0, so
     `late_initcall`'s `clk_disable_unused` cuts it — killing the only
     console, indistinguishable from a hang. Genuine upstream driver gap,
     not MT6797-specific. Fix:
     `patches/v6.6/serial/0001-serial-8250_mtk-hold-baud-clock-enabled.patch`
     (`devm_clk_get_enabled()` for both the `"baud"`-named and legacy-unnamed
     fallback paths). Validated on hardware (build #69,
     `logs/2026-07-06-69-uart-clk-fix-validation/`): `clk_disable_unused`
     completes, `infra_uart0` is skipped (enable_count now > 0), boot
     continues through eMMC mount and systemd with **no
     `clk_ignore_unused` needed**. Confirmed *not* the same root cause as
     B-13 (that one is a scpsys/power-domain issue with no domain even
     registered in this config; this one is a plain clk-framework refcount
     bug in a UART driver). **Folded back into the production build
     2026-07-06** (build #71, boot.md "BUILD #71"): `gemini-usb.config`
     (mtu3 gadget) re-added alongside the fix, `configs/gemini-cmdline.config`
     updated to drop `clk_ignore_unused` for real, validated end-to-end over
     SSH-over-USB with no regression (clean boot to `graphical.target` in
     19s, `g_ether` gadget working).
  With both workarounds, Linux 6.6 reached `Run /init as init process` —
  **first full boot to userspace** — before panicking in `switch_root` for
  reasons now tracked under B-7 (no eMMC controller node in DT at all).
  Patch: `patches/v6.6/dts/0001-arm64-dts-mediatek-add-gemini-pda-board.patch`
  (scp node + reserved-memory additions). **Phase 3 success criterion (first
  Linux 6.6 boot with diagnostic serial output) is met.**

## 🔴 B-3 — LK memory/DTB fixup behaviour unknown

The vendor DTB carries a 1 GB placeholder memory node that preloader/LK fixes
up at boot. Unknown: does LK apply the same fixup to *our* DTB, and does it
preserve our `reserved-memory` carve-outs? Wrong answer = kernel sees 1 GB, or
stomps ATF/TEE regions (silent death).
- **Unblocks:** first boot — check `dmesg` memory map and `/proc/iomem`.

## 🟢 B-4 — dts/0006 display nodes default `status="okay"` (RESOLVED 2026-06-10)

Fixed: all 12 MM nodes (larb0, smi_common, disp_* ×9, mutex) now carry
`status = "disabled"`, matching `dsi0`/`mipi_tx0`. While fixing it, two more
pre-existing defects were found and fixed (see findings.md addendum):
- `dts/0006` was **corrupt** (hunk header declared 137 added lines, body had
  150) — it had *never applied*; the prior "compiles" claim was untestable.
- No patch added `mt6797-gemini-pda.dtb` to the dts `Makefile` — the board
  DTB would never have been built. Entry added to `dts/0001`.
Both patches regenerated from an in-tree edit (`git diff`), verified against
a pristine v6.6 checkout: all 10 patches `git apply --check` clean, and the
board DTB compiles (clang -E + dtc, 15,260 bytes).

## 🟡 B-5 — Datasheets missing for external chips

Verification-blocked BLOCK findings that no amount of repo work can clear
(see findings.md "Re-verification pass" for exactly what was and wasn't
clearable from the MT6797 spec PDF now in `docs/`):

| Chip | Patch | What is blocked |
|------|-------|-----------------|
| Richtek RT5735 | `regulator/0001` | Slew table, VSEL0/VSEL1 active-register polarity, PID value — drives the CPU rail; **do not enable on hardware until confirmed** |
| ON Semi FUSB301A | `usb/0001` | TYPE-register role decode (current logic FIXME-flagged, likely inverted) |
| AWINIC AW9523B | `gpio/0001` | ID value 0x23, CTL bit semantics |
| MT6797 MIPITX | `phy/0004` | PLL/PCW register layout, lock bit. Needs **"MT6797 Software Register Table (Part II)"** — the functional spec in `docs/` explicitly defers all DSI/MIPITX registers to that document (§6.4.3). Worth hunting for the Part II PDF. |

- **Unblocks:** datasheet acquisition, or empirical verification on hardware
  (Phase 4+; chip-ID reads first).

## 🟡 B-6 — `xhci-mtk` has no MT6797 entry in its device table

Phase 8 networking (USB-Ethernet) assumes the generic `mediatek,mtk-xhci`
binding will match. Unverified; if it does not bind, Phase 8 needs a small
compatible/device-table patch.
- **Unblocks:** hardware test once Phase 4 is stable.

## 🟢 B-7 — Rootfs / userspace compatibility (RESOLVED 2026-07-05 — 2019 Kali userspace boots under 6.6)

**Resolution (boot.md SIXTEENTH RESULT):** with eMMC working, the vendor
ramdisk's `switch_root` onto `/dev/mmcblk0p29` succeeds and the 2019 Kali
userspace boots fully under Linux 6.6 — systemd 239, udev, sshd, connman,
`kali login:` prompt on ttyS0 at 22.5s, multi-user + graphical targets
reached. **No fresh rootfs needed.** One follow-up (userspace, not kernel):
the vendor `kpoc_charger`/droid-hal-init daemon misreads the charging state
(vendor battery sysfs paths don't exist under 6.6), triggers its power-off
path and sysrq-remounts everything read-only ~28s in; disable/mask
`droid-hal-init`, `lxc@android` and the charger units on p29. Original
blocker text and the msdc0 bring-up chronicle below for history.

The 2019 Kali `linux.img` userspace was built against kernel 3.18. Running it
under 6.6 is plausible but unproven (module loading, udev, device names).
- **Recommendation (fable-report §5.8):** build a fresh arm64 rootfs with
  `mmdebstrap` — the procedure is already proven (`archive/PROGRESS.md`) and
  removes the 2019-userspace variable entirely. Decision pending.
- **Confirmed blocking, 2026-07-04** (boot.md "SEVENTH RESULT" entry): with
  B-2 now resolved and Linux 6.6 reaching `Run /init as init process`, init's
  script (from the 2019 Kali ramdisk) unconditionally does
  `switch_root` onto `/dev/mmcblk0p29`, which panics
  (`/dev/mmcblk0p29: Can't lookup blockdev` → `Attempted to kill init!`)
  because **no MMC/eMMC/SDHCI controller node exists anywhere in the device
  tree** — confirmed by grepping both mainline `mt6797.dtsi` and our board
  DTS. This is a prerequisite for *either* rootfs option (reused 2019 image
  or fresh mmdebstrap build): without an eMMC node, nothing can mount any
  root filesystem from internal storage. Next concrete step: add an MT6797
  MSDC/eMMC device-tree node — mainline's `drivers/mmc/host/mtk-sd.c`
  (`mtk-sd` driver) already supports the MT6797 MSDC IP block, so this should
  not require a new driver, only correct DT wiring (reg/clocks/pinctrl,
  vendor DTB at `docs/vendor-dtb/gemini_kali_boot.dts` has the reference
  node shape).
- **MSDC0 node added, then deferred, 2026-07-04:** an `msdc0@11230000` node
  was added using `compatible = "mediatek,mt6795-mmc"` (nearest upstream
  MSDC IP generation; mainline `mtk-sd.c` has no MT6797 entry). Probe
  required adding a second `state_uhs` pinctrl state (`mtk-sd.c` hard-fails
  probe without one, even for fixed non-removable eMMC with no real UHS
  signaling). With that added, probe proceeds but then **hangs completely
  silently** — no printk, ATF `aee_wdt_dump` fires ~36s later
  (`logs/2026-07-04-29-msdc0-uhs-boot.log`). Hypothesized the MSDC50_0 clock
  mux being unrouted (`msdc_init_hw()`'s `readl_poll_timeout(..., CKSTB, 0,
  0)` has **no timeout**, spins forever) and added
  `assigned-clocks`/`assigned-clock-parents` routing the mux to
  `msdcpll_d2` (mirroring the mt8173 binding example) —
  **no effect**: `logs/2026-07-04-31-msdc0-assigned-clocks-boot.log` hangs at
  the byte-identical timestamp/PC/LR as before, disproving the clock-wait
  theory. Attempted to symbolicate the hang PC/LR against `System.map` but
  KASLR's runtime slide made the addresses meaningless (resolved to
  irrelevant `omap_dm_timer` symbols). Added `nokaslr` to
  `configs/gemini-cmdline.config` for the next attempt at this, but **root
  cause is still unknown.**
- **Decision, 2026-07-04: MSDC/eMMC deferred, not required for Phase 4's
  first milestone.** The Gemini also has a removable SD card as a future
  alternate storage path, and per CLAUDE.md principle 5 (bootability first),
  neither is required just to prove the kernel/DT stack is otherwise sound.
  The `msdc0` node is left in the board DTS but `status = "disabled"`
  (comment references this entry for whoever resumes it). The vendor init
  script's unconditional `switch_root` (which is what actually needs eMMC)
  is bypassed for now via `rdinit=/bin/sh` on the cmdline — see boot.md
  "EIGHTH RESULT" for the resulting clean boot to an interactive shell.
  Resume this by symbolicating the hang PC with `nokaslr` before trying
  anything else; a genuine register-layout mismatch (needing a proper
  `mt6797_compat` table entry in `mtk-sd.c` rather than the `mt6795`
  stand-in) is the leading remaining hypothesis.
- **Update 2026-07-05 (msdc0 resumed, root cause found):** with the display
  fragment disabled (B-13, boot.md TENTH RESULT) and the headless `#32` build
  actually booting (after a stale-`boot`-slot detour — boot.md ELEVENTH
  RESULT), msdc0 finally probed: hclk-mux fix works (hclk 273 MHz),
  `msdc_init_hw` completes. The old "silent hang" is now diagnosed as an
  **MSDC IRQ storm**: ATF's watchdog dump (real PC this time, `nokaslr`)
  lands in `__irq_resolve_mapping` with hwirq 111 = GIC SPI 79 = msdc0.
  `msdc_irq()` always returns `IRQ_HANDLED` so the spurious detector never
  trips, and with `maxcpus=1` the storm starves everything, including printk.
  The mt6795-compat register-layout-mismatch hypothesis is still open as the
  reason the line screams. A storm guard (mask + dump raw
  `MSDC_INT/INTEN/PS` after 100k hits, disable line) is in the mtk-sd
  instrumentation patch; next boot (`#33`,
  `logs/2026-07-05-28-msdc0-irqstorm-guard/`) will name the stuck bit.
- **Update 2026-07-05 (storm root cause = IRQ polarity; first shell):** the
  `#33` storm-guard boot (boot.md TWELFTH RESULT,
  `logs/2026-07-05-29-msdc0-irqstorm-guard-boot.log`) dumped
  `MSDC_INT=0 MSDC_INTEN=0 MSDC_PS=81ff0002` — the storm is not from the MSDC
  event logic at all. Vendor DTB declares msdc0's IRQ as SPI 79
  **level-LOW**; our DTS said level-HIGH, so the idle line read permanently
  asserted. With the guard disabling the line, boot reached
  `Run /bin/sh as init process` and a live serial shell (MMC commands then
  time out, interrupt-less). Fix: `IRQ_TYPE_LEVEL_LOW` in the board DTS
  (patch 0001 regenerated); build `#34` in
  `logs/2026-07-05-30-msdc0-irq-levellow/` awaits flashing. The
  mt6795-register-layout hypothesis is likely retired as the storm cause.
- **Update 2026-07-05 (polarity confirmed; next failure = empty OCR):** `#34`
  boot (boot.md THIRTEENTH RESULT,
  `logs/2026-07-05-31-msdc0-irq-levellow-boot.log`): storm guard silent —
  polarity fix confirmed. Card init now fails with `no support for card's
  volts` / `-22`: no `vmmc-supply`/`vqmmc-supply` in our msdc0 node →
  empty `ocr_avail`. Fixed in build `#35`
  (`logs/2026-07-05-32-msdc0-vmmc-supply/`) with fixed always-on regulators
  (vemc 3.0 V → vmmc, vdd_1v8 stub → vqmmc); awaits flashing.
- **Update 2026-07-05 (vmmc confirmed; -84 CRC = mt6795 compat mismatch —
  the layout hypothesis returns, now proven):** `#35` boot (boot.md
  FOURTEENTH RESULT): volts error gone, card now fails with CRC `-84`.
  Vendor MT6797 `msdc_reg.h` proves `mt6795_compat` wrong on two counts:
  MT6797 has a 12-bit CKDIV (mt6795 data assumes 8-bit, so CKMOD bits
  landed inside the divider → wrong card clock) and PAD_TUNE0 at 0xf0
  (mt6795 writes 0xec, which doesn't exist). Switched compatible to
  `mediatek,mt2701-mmc` (12-bit div, PAD_TUNE0, async_fifo, data_tune).
  Also stripped all bias/input-enable pinconf from the msdc0 pin groups —
  upstream `pinctrl-mt6797.c` has no pinconf support, the failures
  reverted state application and leaked a GPIO125 pin claim. Build `#37`
  in `logs/2026-07-05-34-msdc0-mt2701-compat/` awaits flashing.
- **Update 2026-07-05 (eMMC WORKS — controller half of B-7 resolved):** `#37`
  boot (boot.md FIFTEENTH RESULT,
  `logs/2026-07-05-35-msdc0-mt2701-compat-boot.log`): banner matches, storm
  guard silent, no pinctrl errors, no CRC. `mmc0: new high speed MMC card at
  address 0001` → `mmcblk0: mmc0:0001 DF4064 58.2 GiB` with **all 33
  partitions** (p1–p33, incl. the p29 Kali rootfs), plus boot0/boot1/rpmb.
  The mt2701-compat + pinmux-only fix is confirmed. Card runs at legacy
  "high speed" (52 MHz) — fine for bring-up; HS200/HS400 tuning is a later
  optimisation. What remains of B-7 is the original rootfs question: point
  init at `/dev/mmcblk0p29` (drop `rdinit=/bin/sh`, let the vendor ramdisk
  `switch_root`) and see whether the 2019 Kali userspace survives 6.6, or
  build a fresh mmdebstrap rootfs.
- **Unblocks:** decision + a Phase 4 build session; does not block Phase 3.

## 🟡 B-8 — R63419 panel requires dual-DSI for native resolution

The panel is dual-DSI (port0+port1, 4 lanes each); the current port is
single-DSI only. Single-DSI may reduce resolution/refresh **or fail to display
entirely** — unknown until hardware test. Mainline MTK DRM dual-DSI support is
weak. The spec PDF confirms the dual-DSI architecture (2 clock + 8 data lanes)
but no register detail (B-5).
- **Unblocks:** Phase 5 hardware iteration; manage expectations — a reduced
  display mode may need to be accepted per the project priority order.

## 🟢 B-9 — Touchscreen chip identity unknown (RESOLVED 2026-08-20: Novatek NT36772 at 0x62/0x01, trim ID `00 00 03 72 66 03`; the remaining defect is B-31)

Novatek model is runtime-identified (`nvtpid`); cannot pick a driver path
until the ID is read on hardware or found in a Gemian/Kali boot log.
- **Unblocks:** first I2C-capable boot, or a community boot log.

## 🟢 B-10 — Build VM deleted (RESOLVED 2026-06-10 — rebuilt, smaller, reproducible)

Rebuilt the same day as Debian 13 arm64 (cloud image + cloud-init — fully
scripted, no manual installer): 10 GiB virtual disk (5.0 GB actual on host,
`discard=unmap` + `fstrim` keep it compact), GCC 14.2, SSH key auth, 9p host
share. Full kernel build verified: **0 errors, Image.gz + Gemini DTB + all
ported-driver modules** (~12 min). Rebuild recipe = `~/gemini-build/vm/seed/`
+ base image + `start-vm.sh`; provisioning script `~/provision-build.sh` in
the VM replays clone→patch→config→build. CLAUDE.md Build VM table updated.
Original entry below for history:

<details><summary>Original blocker text</summary>

The space cleanup removed **the entire `~/gemini-build/` directory** — the VM
qcow2 (with its `~/linux-6.6` and `~/gemini_linux` copies), the start script,
and all snapshots. Trash is empty; no Time Machine local snapshots. Nothing
authoritative was lost (patches and evidence live in this repo), but **no
kernel can be built until the VM is rebuilt** — kernel builds cannot run on
macOS (case-insensitive FS causes phantom file collisions, observed directly;
host-tool chain requires Linux).
- **Partially mitigated (2026-06-10):** the Mac-side kernel checkout is
  restored (`/Volumes/extdata/github/linux-6.6`, shallow clone of v6.6). This
  supports patch validation and DTS compilation (clang -E + dtc) on macOS,
  which is how B-4 was fixed and verified — but not kernel/module builds.
- **Unblocks:** rebuild the VM before the FTDI cable arrives so Phase 3 isn't
  serialised behind it. Recipe: `archive/claude.md` (QEMU `virt` + HVF, Kali
  arm64, 8 GB/8 CPU, port 5522, virtfs share), then rsync this repo and the
  kernel tree in, `apt install build-essential bc bison flex libssl-dev
  libelf-dev`, and re-create the CLAUDE.md snapshot baseline.

</details>

## 🟡 B-11 — Mainline MT6797 pinctrl has NO EINT (GPIO interrupt) support

Discovered 2026-06-10 while compile-checking the board DTS: dtc warns that
`&pio` is not an interrupt controller, and inspection of
`drivers/pinctrl/mediatek/pinctrl-mt6797.c` (v6.6) confirms it registers **no
`mtk_eint_hw` data at all** — no GPIO line on this SoC can deliver an
interrupt under mainline. This breaks every planned GPIO-IRQ consumer:

| Consumer | Phase | IRQ |
|----------|-------|-----|
| RT9466 charger | 7 | GPIO246 |
| AW9523B keyboard | 6 | GPIO87 (EINT10) |
| FUSB301A USB-C CC | 4+ | TBD |
| Novatek touchscreen | 5+ | TBD |

The charger node in `dts/0001` was `status="okay"` with
`interrupt-parent = <&pio>` — its probe would have failed at IRQ resolution.
**Fixed:** node now `disabled` with the rationale in-DTS.

- **Hardware facts for the future fix** (vendor DTB line 2835): EINT
  controller `eintc@1000b000`, reg `0x1000b000`, GIC SPI 170 level-high,
  192 EINT lines (`max_eint_num = 0xc0`), plus a GPIO→EINT mapping table.
  The mainline pattern is `mtk_eint_hw` data in the pinctrl driver + an
  `"eint"` reg + `interrupt-controller` + `interrupts` on the pio node
  (see `pinctrl-mt2701.c` for a same-generation example).
- **Workaround until then:** polled mode where drivers support it
  (rt9467 cannot poll — charger stays disabled).
  **Correction 2026-07-12 (Phase 6):** the original claim that
  `gpio-matrix-keypad` "can poll" was wrong — v6.6 `matrix_keypad.c` is
  IRQ-only, and no polling mode exists upstream even in current mainline
  (checked the file's full git log). Polling support was added locally:
  `patches/v6.6/input/0001-Input-matrix_keypad-add-polling-mode.patch`
  (optional `poll-interval` DT property → delayed-work scan loop, no row
  IRQs). The Gemini keyboard (build #147) uses it with
  `poll-interval = <20>`; the aw9523b DTS node has its interrupt
  properties removed until EINT exists (annotated in-DTS for restore).
- **Unblocks:** EINT support in `pinctrl-mt6797.c` — **driver work, queued
  behind the freeze**; not needed for Phase 3/4. Now also the Phase 6
  Stage B follow-up (switch keyboard from polling to IRQ) and a Phase 7
  prerequisite (RT9466 charger IRQ).

## 🟡 B-12 — MT6351 PMIC has no mainline support (hardware.md was wrong)

Discovered 2026-06-10 while configuring the first VM kernel build:
`CONFIG_REGULATOR_MT6351` does not exist — direct inspection of v6.6 shows
**no MT6351 MFD, regulator, or RTC driver anywhere in mainline** (only the
ASoC codec `sound/soc/codecs/mt6351.c`). hardware.md previously marked the
PMIC "Upstreamed" (claimed mainlined in 6.2) — corrected.

Impact by phase:
- **Phase 3:** none — UART/boot need no PMIC regulators; LK leaves rails up.
- **Phase 4:** eMMC `vmmc`/`vqmmc` must be `regulator-fixed` stubs in the
  board DTS (rails already configured by LK; `mtk-sd.c` regulators optional).
- **Phase 7:** fuel-gauge plan unchanged (was already "no mainline support");
  RTC now also deferred.
- **Long-term:** a real MT6351 MFD + regulator driver port (mt6397-family
  pattern, vendor `drivers/misc/mediatek/pmic/` as register reference) is a
  new driver_ports.md item — **queued behind the freeze**.

---

## 🟢 B-13 — cpu0 display-boot hard-lock ROOT-CAUSED (2026-07-07): DSI IRQ unmasked at probe wedges cpu0; scpsys domain-table bug was a separate, already-fixed issue

**RESOLVED at the diagnostic level 2026-07-07** (boot.md builds #127–#139):
the cpu0 "hard lock" was never a hardware/bus lock and never scpsys — the
mtk_dsi driver requests its level-low IRQ (GIC SPI 229) at probe time,
unmasking it while LK's leftover DSI engine state holds the line asserted;
cpu0 acks it and `mtk_dsi_irq()` wedges without EOI (status read stalls on
the unclocked/LK-state DSI block), which blocks ALL further interrupt
delivery to cpu0 at the GIC while the core keeps executing. Proof chain:
irqs-off cpu0 spin survives (#129) → irqs-on dies even with
cpuidle.off/nohlt (#131) → GIC observer catches SPI 229 stuck ACTIVE at
hang time (#133) → disable_irq after request is already too late (#135) →
IRQ_NOAUTOEN before request defeats the hang (#137) → clean build boots
to `systemd is-system-running: running` with the display stack enabled
(#139, SSH-validated). **Proper fix landed 2026-07-07 (build #141,
boot.md "BUILD #141"):**
`patches/v6.6/drm/0008-drm-mediatek-dsi-enable-irq-only-while-powered.patch`
— IRQ_NOAUTOEN at probe, `enable_irq()` at end of `mtk_dsi_poweron()`
(clocks on, engine reset), `disable_irq()` in `mtk_dsi_poweroff()` before
clocks off; replaces the interim DEBUG patch. Validated on hardware: no
regression (systemd `running`, `/proc/interrupts` shows IRQ 15 / SPI 229
registered with 0 counts, masked until power-on).
Remaining follow-ups: DRM master deferred-probe chain (ordinary bringup
work), debug-instrumentation cleanup, A72-cluster (B-16) retest.

**Update 2026-07-07 (SMI larb pin-active regression, boot.md BUILD #155/#157):**
new, distinct MM-domain-adjacent hang found and reverted during OVL
frame-fetch debugging. The DSI IRQ fix above cleared the DRM bind hang, but
`flip_done timed out` persisted (boot.md BUILD #151) — root-caused to SMI
larb0 sitting permanently runtime-suspended (mainline only resumes SMI larbs
via mtk_iommu device links, and MT6797 has no mainline M4U driver, so nothing
ever claims it). A fix pinning the larb active at probe
(`memory/0003-…pin-larbs-active-when-no-iommu-driver.patch`) was landed in
build #154, but its guard tested the wrong condition
(`IS_ENABLED(CONFIG_MTK_IOMMU)`, a Kconfig symbol compiled in generally,
rather than whether *this* larb's DT node has an `iommus` phandle) and
silently never fired. **Build #155 corrected the guard** to
`!of_property_present(dev->of_node, "iommus")`, making the pin-active
`pm_runtime_resume_and_get()` call actually execute for the first time — and
this reproduced the same class of MM-domain hard-hang as BUILD #79
(`configs/gemini-display.config`'s `COMMON_CLK_MT6797_MMSYS` finding):
serial log looked completely normal up to the expected USB-mux cutoff, but
the USB gadget never enumerated (vs. 15-35s normally) and the user directly
observed a crash/reboot. **Build #157 reverted `memory/0003` entirely**
(regenerating the patch against a clean tree produced a zero-line diff,
confirming full revert to stock `mtk-smi.c`) and confirmed stable: normal
boot, USB gadget enumerated normally, SSH-live `uptime` with no crash,
`flip_done timed out` persists as expected (larb correctly left unpinned,
`runtime_status`=`suspended`), backlight still correct. **Conclusion:**
SMI larb0's power domain is `MT6797_POWER_DOMAIN_MM` — the same domain
implicated in BUILD #79 — and eagerly resuming it at probe time (rather than
leaving it to whatever power-on sequencing the MM domain needs) reliably
triggers a hard-hang/crash. The open sub-problem is therefore not "pin the
larb active" but "safely power on the MM domain path (scpsys → SMI larb)
without hitting this hang class" — likely needs the same kind of guarded,
precondition-gated approach that fixed the DSI IRQ (enable/resume only after
some other state is confirmed ready, not unconditionally at probe). Current
state: stable baseline restored (backlight working, larb correctly
unpinned, pipeline stalled), tracked as the next concrete B-13 sub-task.

**Update 2026-07-07 (OVL→SMI-larb device link — safe fix for the pin-active
regression, boot.md BUILD #159/#161):** the "safely power on the MM domain
path" sub-problem flagged just above is resolved. Instead of pinning the SMI
larb active unconditionally at its own probe time (the #155 approach that
hard-hung the MM domain), `drm/0011-drm-mediatek-ovl-link-smi-larb-runtime-pm.patch`
has `mtk_disp_ovl_probe()` resolve its existing `"mediatek,larb"` DT phandle
and add a `device_link_add(dev, &larb_pdev->dev, DL_FLAG_STATELESS |
DL_FLAG_PM_RUNTIME)` (no `DL_FLAG_RPM_ACTIVE`) — this ties the larb's
runtime-PM state to OVL's own, so the larb only resumes when OVL is
runtime-resumed by the normal DRM atomic-commit path, not eagerly at
link-creation/probe time. Confirmed live over SSH on build #159: SMI larb and
smi-common both `active` (were permanently `suspended`), OVL0/OVL2L0 IRQ
counters incrementing (71/70, previously frozen at 0), no MM-domain hang, no
crash. **Lesson generalized:** for an SMI larb with no mainline M4U/IOMMU
driver bound, *when* its runtime-PM resume happens (tied to a consumer whose
own resume timing is already proven safe) matters more than *whether* it
happens at all.

Build #161 then stripped the now-obsolete GEMINI-DEBUG instrumentation (its
purpose — diagnosing this blocker — is done) and surfaced the next data
point with a clean serial trace:
```
[drm:mtk_dsi_host_attach] *ERROR* failed to add dsi_host component: -517
panel-solomon-ssd2092 1401c000.dsi.0: failed to attach DSI: -517
```
alongside `mediatek-drm mediatek-drm.1.auto: Waiting for disp-mutex driver
/mutex@1401f000`. `-517` is `-EPROBE_DEFER` — normally a benign, self-resolving
retry — but this has **not yet been confirmed** benign vs. a real block here,
since the serial capture always cuts off shortly after (B-15 mtu3/USB mux
switch) before any retry could be observed. **This is the first task for the
next session:** get a full post-boot `dmesg` over SSH on build #161 (not yet
done — a USB host-side enumeration issue on the Mac intervened and consumed
the rest of this session; root-caused as unrelated to the kernel, see boot.md
"USB gadget enumeration investigation, 2026-07-07" — every serial capture
throughout showed a completely normal, unhung boot) to determine whether the
DSI host eventually attaches or the `disp-mutex` wait blocks it permanently.

**Update 2026-07-08 (`-517` confirmed benign — B-13 closed; new blocker
opened as B-17):** got the live `journalctl -k -b` dump over SSH on build
#159 that the previous session needed (dmesg's own ring buffer had already
wrapped past the boot-time messages). Confirms the DSI host attach
**does** succeed on retry:
```
probe of 1401c000.dsi.0 returned 0 after 62276826 usecs
```
(~62s after the initial `-517`). `mtk_drm_bind` then completes, the panel
registers (`panel-solomon-ssd2092 1401c000.dsi.0: Solomon SSD2092 FHD DSI
panel registered`), and DRM creates `fb0: mediatekdrmfb`. So the
`disp-mutex` wait resolves and the deferred-probe retry is a normal,
harmless part of driver bring-up here — **not** a real block. **B-13, as
originally scoped (cpu0 hard-lock at DSI probe + this probe-defer
question), is now fully closed.**

Immediately after, a new failure appeared that actually explains the still-dark
screen: `mtk_mipi_tx_driver_init` (`phy_mtk_mipi_dsi_drv`, the MIPI DSI
D-PHY) returns **`-16`/`-EBUSY`** on probe. With no working D-PHY, DSI binds
logically (host+panel+fb all register) but can't physically clock data to
the glass, so every DRM atomic commit times out waiting for vblank/flip
completion — an infinite ~10s-period loop of `flip_done timed out` /
`commit wait timed out` across CRTC/PLANE/CONNECTOR as the fbdev helper
keeps retrying. Tracked as new blocker **B-17** (see entry below) — root
cause of the EBUSY not yet investigated (leading candidate: a clock,
regulator, or MMIO region the D-PHY driver requests is already held by
another driver/instance; cross-reference the vendor 3.18 `mtk_mipi_tx`/DSI
PHY source for a specific sequencing requirement).

**Update 2026-07-05 (evening):** severity upgraded — this is no longer just
"DRM never binds". With `CONFIG_COMMON_CLK_MT6797_MMSYS=y` (added to fix the
DSI engine-clk -517 defer), the kernel **hard-hangs silently at ~0.52s**
(identical final line/timestamp in `logs/2026-07-05-21-…` and `-23-…`;
minutes-long capture, no further output, no watchdog dump). Registering the
mm-domain clocks leads to an MM-domain register access with the domain
unpowered/unmanaged, wedging the bus. Until B-13 is fixed, the display
fragment is disabled: `configs/gemini-display.config` →
`gemini-display.config.disabled-b13`. See boot.md TENTH RESULT.

**Update 2026-07-06:** also the likely root cause of the Phase 4 SMP hang
(B-2 item 4) — PSCI `CPU_ON` instrumentation showed the Cortex-A72 cluster
(cpu8/9) hangs firmware-side on bringup, consistent with its power domain
never being enabled by the broken scpsys domain table. See boot.md "PSCI
CPU_ON diagnostic". Fixing B-13 is therefore expected to unblock **both**
display and full 10-core SMP, not just display.

**Update 2026-07-07:** the bare-metal diagnostic payload
(`baremetal/display-hang-test/`) built to distinguish hardware-lock vs
Linux-software causes is **parked without ever executing** — six packaging
variants all hang inside ATF/BL31 before `el3_exit` while a control reflash
of the known-good build boots clean (boot.md "B-13 bare-metal payload",
logs -114..-126; README "Known issue" has the variant table). Replaced by
an in-kernel equivalent:
`patches/v6.6/drm/0007-GEMINI-DEBUG-cpu0-irqsoff-poll-loop.patch` — cpu0
irqs-off raw-MMIO heartbeat + GICD pending/active dump (hooked at the end
of `mtk_drm_probe()`, which returns ~27ms before the hang).

**RESULT (build #129, boot.md "BUILD #129"): NOT a hardware lock.** With
the display config enabled, the hijacked cpu0 heartbeat ran continuously
for ~7.1s straight through the fatal ~2.6s window — cntpct advancing, GICD
pending pattern constant (no storm), and cpu0 still took the ATF watchdog
FIQ at the end. B-13 is therefore a software/GIC-state problem: interrupt
*delivery to* cpu0 dies (or cpu0 dies in something it only does when
allowed to proceed — prime suspect cpuidle/WFI entry), not the core or
bus. Status upgraded from "deferred, hypotheses exhausted" to **actively
tractable**. Next: (1) observer-mode variant — poll loop on cpu1, cpu0
boots normally, dump cpu0's GICR/GICD state each beat to catch what
changes at hang time; (2) `cpuidle.off=1` / `nohlt` test with display
enabled.

Discovered 2026-07-05 during first Phase 5 display bring-up hardware test
(`logs/2026-07-05-02-phase5-display-boot.log`). The full display pipeline
(`disp_ovl0` → `disp_rdma0` → `disp_color0` → `disp_ccorr0` → `disp_aal0` →
`disp_gamma0` → `disp_od0` → `disp_dither0` → `mutex` → `dsi0` → panel, plus
`mipi_tx0`) was enabled in the board DTS for the first time, and
`CONFIG_DRM_MEDIATEK`/`CONFIG_PHY_MTK_MIPI_DSI`/`CONFIG_DRM_PANEL_RENESAS_R63419`/
`CONFIG_MTK_MMSYS`/`CONFIG_MTK_CMDQ`/`CONFIG_BACKLIGHT_CLASS_DEVICE` forced
built-in (new fragment `configs/gemini-display.config` — required since the
`rdinit=/bin/sh` initramfs shell has no modprobe path, see B-7). Kernel and
DTB built and booted cleanly (no hang, no regression to the Phase 4
milestone), but the DRM driver never bound:

```
[    0.320587] mtk-scpsys: probe of 10006000.power-controller failed with error -22
[    0.370139] mediatek-drm mediatek-drm.1.auto: Failed to find disp-mutex node
...
[   10.738979] platform lcd-avee-regulator: deferred probe pending
[   10.739758] platform lcd-avdd-regulator: deferred probe pending
[   10.740509] platform 1401c000.dsi: deferred probe pending
```

**Root cause (confirmed by reading driver source in the VM, not guessed):**
`drivers/pmdomain/mediatek/mtk-scpsys.c`'s `scp_domain_data_mt6797[]` uses
C99 designated initializers indexed by the `MT6797_POWER_DOMAIN_*` enum
(`include/dt-bindings/power/mt6797-power.h`): `VDEC`=0, `VENC`=1, `ISP`=2,
`MM`=3, `AUDIO`=4, `MFG_ASYNC`=5, `MFG`=6, `MFG_CORE0..3`=7-10, `MJC`=11. Only
7 of these 12 slots have an initializer (`VDEC`, `VENC`, `ISP`, `MM`,
`AUDIO`, `MFG_ASYNC`, `MJC`) — the designated-initializer array's size is
fixed by the highest index used (`MJC`=11), so it's actually 12 elements,
and the 5 GPU-related slots (`MFG`, `MFG_CORE0`-`MFG_CORE3`) are silently
zero-filled (`.name = NULL`). `scpsys_probe()` → `init_scp()` loops
`for (i = 0; i < num; i++)` over **all 12** and calls
`devm_regulator_get_optional(&pdev->dev, data->name)` unconditionally. For
the 5 empty slots this passes `id == NULL` into `_regulator_get()`
(`drivers/regulator/core.c:2179`), which treats a NULL identifier as a hard
error (`pr_err("get() with no identifier\n"); return ERR_PTR(-EINVAL);`) —
**not** the "-ENODEV, no supply configured" path `devm_regulator_get_optional`
is meant to tolerate. `init_scp()` doesn't distinguish this from a real
error and aborts, so `scpsys_probe()` returns -EINVAL for the **whole
device**, not just the GPU domains. Since our `MM` domain (index 3, fully
populated, needed for the entire display path) lives on the same platform
device, every display component's `power-domains = <&scpsys
MT6797_POWER_DOMAIN_MM>` reference fails to resolve and every consumer sits
in permanent deferred probe.

This is a genuine upstream Linux 6.6 gap in MT6797 support, not a Gemini
board-DTS mistake — confirmed the DTS is unaffected: `mipi_tx0`/`dsi0`/all
`disp_*` nodes compile and show `status = "okay"` correctly in the built
DTB, and their DT wiring (`power-domains`, `clocks`, `mediatek,larb`) matches
the mt8173 reference pattern. The bug is entirely inside
`mtk-scpsys.c`'s MT6797 domain table + its all-domains probe loop.

**Not yet fixed** — a correct fix needs real SPM register offsets
(`ctl_offs`, `sta_mask`, `sram_pdn_bits`/`sram_pdn_ack_bits`) for the `MFG`/
`MFG_CORE0-3` domains, which are GPU (Mali T860, Panfrost) power gates that
are explicitly out of scope for Phase 5 (hardware.md: GPU work "Defer until
display works"). No verified register values for these are in the project
yet (B-5 gap: no full datasheet). Fabricating placeholder register offsets
for GPU-domain gating is exactly the kind of guess CLAUDE.md principle 7
(documented rationale, no guessing on hardware values) warns against —
wrong `ctl_offs` values here risk toggling live SPM state incorrectly.

**Two real fix paths, next session:**
1. **Correct:** source real MT6797 `MFG`/`MFG_CORE0-3` SPM register values
   (likely obtainable from the vendor 3.18 BSP's own SPM/scpsys driver in
   `drivers/misc/mediatek/base/power/mt6797/`, not yet checked) and add
   proper `scp_domain_data_mt6797[]` entries for indices 6-10.
2. **Safe workaround, less correct:** patch `init_scp()`'s probe loop (and
   the mirrored loop in `mtk_register_power_domains()`) to `continue` when
   `data->name == NULL`, i.e. skip unpopulated domain-table slots instead of
   treating them as fatal. This is a generically-applicable driver
   robustness fix (protects any future SoC with sparse domain tables, not
   Gemini-specific), and doesn't require GPU register values since it just
   stops the driver from tripping over its own data-table gap. Recommended
   starting point — unblocks `MM`/display without touching GPU power state
   at all.
- **Unblocks:** the actual kernel-driven display test (today's result was
  proof the DTS/build/config chain is right, not proof the panel can be
  driven — the LK bootsplash seen on screen is unrelated, confirmed by its
  timing: static and present from very early in boot, i.e. rendered by LK's
  own `logo`-partition splash code before Linux ever runs, not by our new
  DRM/panel patches).

**Update 2026-07-06 (fix re-tested, still not sufficient):** the "safe
workaround" patch above
(`patches/v6.6/pmdomain/0001-pmdomain-mediatek-skip-unpopulated-mt6797-domain-slots.patch`)
was committed 2026-07-04 but had never actually been flash-tested with
`configs/gemini-display.config` enabled until now. Re-enabled the display
fragment and retested (build #79/boot.md "BUILD #79"): boot progresses
further than the original 2026-07-05 discovery — `mediatek-drm` now gets as
far as adding component matches and the panel driver registers
(`panel-renesas-r63419 ... registered`) — but the kernel then hard-hangs and
the board enters a genuine watchdog reboot loop (5 cycles observed in one
capture, each dying at the identical line). An ATF `aee_wdt_dump` this cycle
symbolicated to `cpu_do_idle` on CPU1, which is a red herring: the
`inter-cpu-call interrupt is triggered` lines that precede it are ATF's
whole-system IPI broadcast for collecting a crash dump once some CPU's
watchdog trips, not evidence CPU1 is the stuck core. The real hang is
presumed to still be on the boot CPU inside the MM-domain power-on register
access itself, once a consumer actually touches it — the NULL-name skip fix
only prevents the scpsys *driver probe* from aborting early; it does not
supply working power-on register offsets/timing for the MM domain. **B-13 is
not resolved by this patch alone.** Device was recovered by re-flashing the
known-good `maxcpus=8`, no-display build (`logs/2026-07-06-77-maxcpus8/`) —
note the first re-flash attempt during this session silently failed to take
(capture still showed the old hung build); a second attempt succeeded and was
verified by checking the kernel banner in a fresh capture plus a live SSH
session. `configs/gemini-display.config` is left enabled in the repo since
the underlying scpsys probe fix is real forward progress, but it is not yet
safe to leave flashed on the device without further work on the actual
MM-domain power sequencing.

**Update 2026-07-06 (second infracfg block hypothesis, tested and falsified):**
cross-checking the vendor DTB (`docs/vendor-dtb/gemini_kali_boot.dts`, this
device's own flash) found MT6797 has **two distinct infracfg hardware
blocks**: `infracfg_ao@10001000` (what mainline's `mt6797.dtsi` models as the
`infrasys` node, used for infra clock gating) and a **second, separate**
`infracfg@10201000` node. The vendor's own `scpsys@10001000` node spans all
three physical regions (`0x10001000` AO infracfg, `0x10006000` SPM,
`0x10201000` this second block), strongly suggesting the real
`INFRA_TOPAXI_PROTECTEN`/`PROTECTSTA1` bus-protection registers
`scpsys_bus_protect_enable/disable()` needs live in the second block, not the
AO block mainline's `scpsys` phandle currently points at.

Added a new `syscon`-only DT node for the second block and repointed
`scpsys`'s `infracfg` phandle at it
(`patches/v6.6/dts/0010-arm64-dts-mediatek-add-mt6797-real-infracfg-node.patch`,
build #81). Flash-tested (`logs/2026-07-06-81-scpsys-b13-real-infracfg/`,
capture `logs/2026-07-06-82-scpsys-b13-real-infracfg-boot.log`): **no change
in observed behaviour** — boot reaches the identical point as the untested
build #79 (`panel-renesas-r63419 ... registered`), then hard-hangs with the
same ATF watchdog signature (`aee_wdt_dump: on cpu1` at 14.2s, `on cpu3` at
18.1s, then silence). The hypothesis is not confirmed by this result: either
(a) the second infracfg block isn't actually where bus protection lives
either (the vendor DTB's grouping of three regions under one `scpsys` node
doesn't necessarily mean all three are used by the *bus-protection* sub-
function specifically — SPM and AO infracfg alone could already satisfy
mainline's `scpsys` needs, and the 0x10201000 block could be for something
else the vendor driver also touches), or (b) the phandle target was right
but bus protection was never the actual hang cause -- the hang could equally
be inside `scpsys_power_on()`'s SRAM/power-on register sequencing itself
(`ctl_offs`/`sta_mask`/`sram_pdn_bits`) once the MM domain is genuinely
powered on and a consumer (the panel/DSI path, consistent with hanging right
after panel registration) touches its registers for the first time. The
DTS-only fix is not sufficient on its own; register-level confirmation via
the vendor's actual driver behaviour (e.g. instrumenting `scpsys_power_on`/
`scpsys_bus_protect_enable` with `dev_info` per-step, since the code path is
now provably reached but stalls somewhere inside it) is the next concrete
step, not further DTS-only guessing. Patch 0010 is retained (harmless, and
directionally justified by the vendor DTB evidence even if not sufficient by
itself) but does not close B-13.

**Update 2026-07-06 (per-step power-on trace — scpsys power-on EXONERATED,
hang relocated to DRM bind):** build #84
(`logs/2026-07-06-84-scpsys-b13-step-trace/`, banner `#14 ... 08:45:36`,
temporary patch
`patches/v6.6/pmdomain/0002-GEMINI-DEBUG-scpsys-power-on-step-trace.patch`)
added a `dev_info` before every step of `scpsys_power_on()`. Capture
(`logs/2026-07-06-85-scpsys-b13-step-trace-boot.log`, second boot in file):
**every domain — vdec, venc, isp, mm, audio, mfg_async, mjc — completes all
steps cleanly**, including MM's `sram_enable`, `bus_protect_disable` and
`done` at 0.3548s. Two key facts:

1. MM's ctl register already read `0xe0d` *before* the kernel touched it —
   PWR_ACK set, SRAM up: the vendor LK bootloader leaves the MM domain
   powered on for its splash screen. The kernel's power-on is a no-op ride
   on an already-live domain.
2. The hang therefore is **not in scpsys at all**. The last kernel line is
   still `panel-renesas-r63419 ... registered` (0.4569s) — the moment the
   final DRM component match completes and the component master binds. The
   stall is inside the mediatek-drm bind path, i.e. the first actual
   register access to the 0x14xxxxxx mmsys range (mmsys routing writes,
   ddp comp init) or a DMA/clock dependency of it.

Concrete confirmed gap found while investigating: DTS patch 0006 declares
`mediatek,mt6797-smi-larb`/`mediatek,mt6797-smi-common` nodes, but **no
driver implements those compatibles** — upstream `drivers/memory/mtk-smi.c`
has no MT6797 entries and no project patch adds them, so SMI never probes
and its clocks are never enabled. Whether the bind-time hang is (a) the
mmsys config register write path needing a clock nothing enables, or (b)
something touching the un-clocked SMI/larb, needs one more instrumented
build — next step: per-step trace of `mtk_drm_bind()` /
`mtk_drm_kms_init()` / `mtk_mmsys_ddp_connect()` to find the exact first
stalling register write. B-13's title (scpsys domain table) is now known to
be a mischaracterisation of the display hang; the scpsys probe fix was real
but the remaining blocker lives in the DRM/mmsys/SMI layer.

**Update 2026-07-06 (build #86/#87 — DRM bind also exonerated; hang pinned
to `mtk_dsi_probe()` tail):** the bind-path trace never printed a single
line — the component master bind never starts. Since the panel prints
"registered" only after `mipi_dsi_attach()` returns, the hang window is the
remainder of `mtk_dsi_probe()` after `mipi_dsi_host_register()`: clock
lookups → ioremap → `devm_phy_get` → `devm_request_irq`. The ATF dump PC
resolves to `cpu_do_idle` on cpu1 (idle victim); cpu0 (probe CPU) never
dumps — it is the wedged core. Leading hypothesis: `devm_request_irq`
unmasks the DSI IRQ while LK's splash has left the DSI engine live; a
stale/screaming interrupt wedges cpu0 in `mtk_dsi_irq()` (unclocked
`readl(DSI_INTSTA)`, unbounded `while (DSI_BUSY)` spin). Testing with build
#88 (`patches/v6.6/drm/0006-GEMINI-DEBUG-dsi-probe-tail-and-irq-trace.patch`).
See boot.md "BUILD #86/#87".

**Update 2026-07-06 (build #88/#89 — probe tail clean, IRQ-storm hypothesis
REFUTED; entire display stack exonerated):** every dsi probe-tail step
completed; the DSI IRQ fired exactly once (INTSTA=0x2 = CMD_DONE, handled
cleanly, no storm); the panel registered. Since `mipi_dsi_attach()` runs
before the panel's "registered" print and calls `mtk_dsi_host_attach()` →
`component_add`, the DRM master bind attempt had already happened and
deferred (no IOMMU) before that print — so scpsys (#84), DRM bind (#86) and
dsi probe/IRQ (#88) are all exonerated. cpu0 wedges with IRQs masked ~50ms
after the panel print in unmarked code (cpu1's ATF dump PC again resolves to
`cpu_do_idle` — idle bystander). Next: build #90 boots with `initcall_debug`
on the cmdline so the last `calling <fn>` line names the wedging function
directly. See boot.md "BUILD #88/#89".

**Update 2026-07-06 (build #92/#93 — wedging initcall identified:
`cacheinfo_sysfs_init`; culprit is an unresponsive secondary CPU):**
`initcall_debug ignore_loglevel` shows the display path completing and
returning (`probe of 1401c000.dsi.0 returned 0`, `r63419_driver_init
returned 0`), then the last line is `calling cacheinfo_sysfs_init` before
the 14s watchdog. That initcall's `cpuhp_setup_state()` waits on every
online CPU's hotplug thread in turn — cpu0 is blocked waiting, not wedged
itself. So the display build's real defect is that it silently wedges a
*secondary* CPU (cpu1–7) somewhere before 2.4s — scpsys domain writes or
display clock enables are prime suspects. Build #94
(`patches/v6.6/base/0001-GEMINI-DEBUG-cacheinfo-cpuhp-per-cpu-trace.patch`
+ `rcupdate.rcu_cpu_stall_timeout=6`) identifies which CPU. See boot.md
"BUILD #90–#93".

**Update 2026-07-06 (build #94/#95 — secondary-CPU hypothesis REFUTED; the
wedged CPU is cpu0 itself, unresponsive to IRQs):** the RCU stall report
fired at 8.4s and names **cpu0** as the stalled CPU, *detected by cpu4*
(cpus 1–7 are alive and healthy). Not one `cacheinfo_cpu_online` trace
print ran (strings confirmed in the packed kernel), so init blocks in
`cpuhp_setup_state()` waiting for the `cpuhp/0` thread — pinned to cpu0 —
which never runs because cpu0 stops taking interrupts at ~2.42s (RCU's 750
fqs attempts all failed). The remote task dump is useless (`__switch_to` /
`0x0` — no NMI on arm64 by default). So the display build kills interrupt
delivery/wakeup specifically for cpu0: GIC redistributor, cpu0's arch
timer, or a lost wakeup — plausibly a side effect of scpsys bus-protect
writes (the known wrong MT6797 bits) or a display clock change. Build #96
adds pseudo-NMI (`CONFIG_ARM64_PSEUDO_NMI=y` in
`configs/gemini-debug-b13.config` + `irqchip.gicv3_pseudo_nmi=1`, GIC is
v3) so the stall handler can NMI-backtrace cpu0 and show its real PC. See
boot.md "BUILD #94/#95".

**Update 2026-07-06 (build #96/#97 — pseudo-NMI REVERTED, regressed boot):**
`CONFIG_ARM64_PSEUDO_NMI=y` + `irqchip.gicv3_pseudo_nmi=1` broke boot
*earlier* than the bug it was meant to diagnose: total silence from
`el3_exit` (4.37s) to the ATF watchdog (14.38s), not even the earlycon
banner — this device's ATF/GIC evidently doesn't tolerate pseudo-NMI's
early priority-mask setup. Reverted (`configs/gemini-debug-b13.config`
removed, cmdline flag dropped). cpu1's ATF dump is the same
`cpu_do_idle`/`arch_cpu_idle` bystander as every prior build — no new data.
**Next:** an IPI heartbeat probe (kthread on cpu1 pinging cpu0 via
`smp_call_function_single` every ~50ms from early boot) to pinpoint exactly
when cpu0 stops acking IPIs, without touching NMI/GIC priority masking. See
boot.md "BUILD #96/#97".

**Update 2026-07-06 (build #98/#99 — cpu0's death window pinpointed to
2.48–2.54s; cpu0 loses ALL interrupt responsiveness, not one code path):**
the IPI heartbeat ran clean every 60ms from 0.72s to 2.4799s (seq 30), then
missed by 2.5399s (seq 31) — death lands inside `cacheinfo_sysfs_init`
(called 2.4237s) but well after it starts, so cpu0 is alive when that
initcall begins. Crucially, this heartbeat uses a *different* IPI mechanism
(`smp_call_function_single`) than the `cpuhp/0` thread wakeup cacheinfo
needs, and both fail in the same window — so cpu0 is losing the ability to
take **any** interrupt, not failing a specific code path. This also shifts
timing suspicion from the scpsys domain-power writes (done by 2.12s, now
~360ms earlier) toward the DSI/panel probe (2.412–2.419s, only 60–120ms
before death) as the more temporally-proximate trigger, though scpsys
remains the leading root-cause candidate for *why*. **Next:** tighten the
heartbeat to 10ms and add a periodic cpu0 GICR_WAKER read (GICv3
redistributor sleep-state register) to test whether the display build is
corrupting the GIC redistributor for cpu0 directly. See boot.md "BUILD
#98/#99".

**Update 2026-07-06 (build #100/#101 — GICR_WAKER refuted; death window now
20ms, 2.513s-2.533s):** `gicr0_waker=0x0` on every reading right up to the
final MISS — cpu0's redistributor never sleeps, so the "display build
corrupts/sleeps the GIC redistributor" hypothesis is refuted. With the GIC
confirmed healthy, cpu0 losing both a raw IRQ-context IPI callback and the
`cpuhp/0` kernel-thread dispatch (`kernel/cpu.c`
`cpuhp_invoke_ap_callback`/`__cpuhp_kick_ap`) at the same moment now points
to cpu0 either stuck in genuine WFI/idle without waking (cpuidle/PSCI
CPU_SUSPEND bug) or actually running/spinning with interrupts effectively
undeliverable (masked-IRQ-forever bug), rather than a GIC hardware fault.
**Next:** add `idle_cpu(0)` to the heartbeat (cheap scheduler-state read,
no IPI) to distinguish the two. See boot.md "BUILD #100/#101".

**Update 2026-07-06 (build #102/#103 — WFI-never-wakes REFUTED; cpu0 wakes
normally then hard-locks within 20ms, before reaching cacheinfo):** cpu0
goes idle at 2.4235s (matching `cacheinfo_sysfs_init`'s dispatch almost
exactly), sits idle ~100ms (unusually long for a routine wakeup), then
**wakes normally** at 2.523s (`idle_cpu(0)` flips back to busy) — the wake
mechanism itself works. Within 20ms of waking it's already unresponsive to
IPIs, and still shows `idle_cpu0=0` (busy, not asleep) at the miss — so this
is not a stuck-in-WFI bug; cpu0 hard-locks while actively running, before
its `cacheinfo_cpu_online()` entry trace ever fires (confirmed still
absent). The ~100ms idle-to-wake delay is itself abnormal and suggests
cpu0's wake path (broadcast/local arch timer) was already disturbed before
the hard lock. **Next:** arm an hrtimer pinned directly to cpu0 (independent
of the cross-CPU IPI heartbeat) to test whether cpu0's own local-timer
interrupt survives past the point where cross-CPU IPIs stop — this
distinguishes a fully halted CPU from an SGI/IPI-delivery-specific fault.
See boot.md "BUILD #102/#103".

**Update 2026-07-06 (build #104/#105 — cpu0's own local timer also dies;
build #106/#107 — SMI larb0/smi_common gating fix has NO effect; B-13
formally DEFERRED):** the pinned-hrtimer test confirmed cpu0's *own* local
timer interrupt dies at the same point as the cross-CPU IPI misses — this is
a genuine full hard lock of cpu0 (all interrupt sources, not an
SGI/IPI-delivery-specific fault). Last real evidence of forward progress is
still `calling cacheinfo_sysfs_init+0x0/0x40 @ 1`; nothing printk-reachable
runs after it.

Vendor-kernel forensics (extracted from `kali_boot.img`'s embedded 3.18
kernel image) suggested the display pipeline gates
`CG_MM_SMI_COMMON`/`DISP0_SMI_LARB0` clocks separately from scpsys's own
`CLK_MM`, and mainline had no MT6797 SMI compatibles at all
(`drivers/memory/mtk-smi.c`) so those clocks were never claimed by any
driver. Added `mediatek,mt6797-smi-larb`/`-common` (reusing MT6795/Helio X10
ops verbatim — no new logic) and enabled the corresponding `larb0`/
`smi_common` DTS nodes. Build #106, capture
`logs/2026-07-06-107-smi-mt6797-fix-boot.log`: DTB and vmlinux confirm the
fix compiled in correctly, but **the hang is bit-for-bit identical** — same
last initcall, same local-timer tick count at death, same heartbeat-miss
timing, same ATF watchdog dump timing — and neither SMI device ever shows a
`probe of ... returned` trace line (every other platform device does),
i.e. no observable bind attempt at all. The SMI hypothesis is falsified.

An ARM64 hardware lockup detector (`CONFIG_ARM64_PSEUDO_NMI`, needed to
NMI-backtrace the wedged cpu0) was already tried and reverted earlier
(build #96/#97): it regresses boot *earlier* than this bug (silence
immediately after `el3_exit`, before earlycon even prints), so this
hardware's ATF/GIC combination does not tolerate pseudo-NMI's priority-mask
setup. Retrying it requires independently validating ATF support first —
not a quick diagnostic.

**Conclusion: every register/driver-level hypothesis sourced from either the
vendor kernel or mainline's own display stack has now been tested — scpsys
power-on sequence, DRM component bind, DSI probe tail, and SMI bus-master
gating — and none moved the hang. The only remaining diagnostic (NMI-based
backtrace) is independently blocked by an ATF incompatibility. B-13 is
formally deferred** per CLAUDE.md principle 5 (bootability first, display
explicitly optional). Reusing the vendor `dispsys`/DDP framework wholesale
was considered and rejected: it's a large 3.18-era subsystem (mtkfb/ion/CMDQ
APIs with no 6.6 equivalent) whose *sequence* we already proved doesn't
matter here (scpsys and SMI sequence-parity tests both changed nothing), so
a port would most likely hit the identical hard lock while adding
significant vendor-code maintenance burden — against CLAUDE.md principle 3.
Revisit only if new evidence emerges (e.g. upstream MT6797 display support
lands, or a future non-NMI diagnostic surfaces cpu0's actual PC at the
lock). See boot.md "BUILD #106/#107".

**Update 2026-07-06 (new evidence, PINNED for later — vendor Halium kernel
source located, SMI/M4U IOMMU-bypass gap identified as untested hypothesis):**
the actual community kernel source for this device was located at
`/Volumes/extdata/github/gemini-android-kernel-3.18` (`dguidipc`'s Halium
kernel, confirmed by the `Linux version 3.18.41+ (dguidi@nowhere)` banner
matching `/Volumes/extdata/scratch/debian`'s extracted kernel). Cross-checked
its `drivers/clk/mediatek/clk-mt6797-pg.c` (the real MTCMOS/scpsys-equivalent
driver, DIS domain at `SPM_REG(0x030c)`) against mainline's `mtk-scpsys.c`
sequence — **bit-identical**, third independent confirmation that scpsys
register sequencing is not the bug.

New lead not yet tested: `drivers/misc/mediatek/video/mt6797/dispsys/ddp_drv.c`
(`disp_probe_1()`, ~line 784) unconditionally writes `0x0` to
`DISP_REG_SMI_LARB0_MMU_EN`/`..._LARB5_MMU_EN` (`larb_base + 0xfc0`) to force
SMI-larb IOMMU bypass whenever M4U support isn't compiled in. Mainline has
**no MT6797 IOMMU driver at all** (checked `drivers/iommu/mtk_iommu.c` and
`mtk_iommu_v1.c` — no compatible string; our `mt6797.dtsi` has no `iommu`/
`m4u` node either), and the `mtk-smi` larb ops we reused from MT6795
(`mtk_smi_larb_config_port_mt8173`, since `mediatek,mt6795-smi-larb` maps to
`&mtk_smi_larb_mt8173` in `drivers/memory/mtk-smi.c`) only writes the
`MMU_EN` register from `mtk_smi_larb_bind()` — called by the component
framework **only when an IOMMU master binds to the larb**, which never
happens here. So on this hardware the larb's MMU_EN register is left at
whatever the power-on-reset default is (untranslated DMA through an
unconfigured/enabled M4U path is plausible), and the larb's own `.probe()`
likely parks in `-EPROBE_DEFER` forever waiting on a companion IOMMU that
will never arrive — independently consistent with build #106/#107's
observation that neither SMI device ever printed a `probe of ... returned`
line. This would explain the *delayed* hang signature: DSI/DDP register
writes (non-DMA MMIO) succeed and print cleanly, but a hang surfaces shortly
after, timing-wise consistent with the point the display hardware would
first issue a real DMA fetch through the larb.

**Deliberately not pursued yet — pinned for a future session.** Proposed
fix, when resumed: a small patch forcing `larb0_base+0xfc0 = 0` and
`larb5_base+0xfc0 = 0` (IOMMU bypass) before the DRM/DDP pipeline can issue
any DMA, mirroring the vendor's unconditional bypass write — either as a
fallback path in `mtk-smi`'s larb probe when no IOMMU master ever binds, or
as a board-specific quirk. This is a genuinely new, unfalsified hypothesis,
distinct from the already-exhausted scpsys/DSI-probe-tail/SMI-clock-gating
tests. Untested — no build/capture evidence for or against it yet.

**Update 2026-07-06 (implemented and built — build #108):** rather than a
board-specific quirk, fixed this generically in `mtk-smi.c` itself: added a
`mmu_bypass` field to `struct mtk_smi_larb`, defaulting `larb->mmu` to point
at it (zeroed) in `mtk_smi_larb_probe()`. Previously `larb->mmu` stayed
**NULL** whenever no `mtk_iommu` master ever binds (always true for MT6797,
which has no mainline IOMMU driver at all) — and `mtk_smi_larb_resume()`
unconditionally calls `config_port()`, which dereferences `*larb->mmu`,
i.e. this was a latent NULL-pointer-deref bug on any IOMMU-less SoC using
this larb ops table, not just a missing bypass write. The fix makes the
zero-value default double as the vendor's forced bypass behavior; a real
IOMMU binding still overwrites the pointer with the live per-port mask, so
no other SoC's behavior changes. Patch:
`patches/v6.6/memory/0002-memory-mtk-smi-default-mmu-bypass-when-no-iommu-bound.patch`.
Build #108 (`logs/2026-07-06-108-smi-mmu-bypass/`, banner `#27 SMP PREEMPT
Mon Jul 6 11:28:49 UTC 2026`, `ALLOW_DEBUG=1`) built clean and packed —
**not yet flashed/captured**.

Corroborating evidence found while re-checking the vendor Halium source
(`/Volumes/extdata/github/gemini-android-kernel-3.18`): its M4U driver
(`drivers/misc/mediatek/m4u/mt6797/m4u_hw.c`) has named functions
`m4u_enable_error_hang()`/`m4u_disable_error_hang()` toggling a
`F_MMU_CTRL_INT_HANG_en` bit in the M4U core's own `REG_MMU_CTRL_REG`, plus
a `m4u_dump_reg_for_smi_hang_issue()` debug helper — i.e. MediaTek's own
engineers have a named "SMI hang" failure mode where an unconfigured/
misconfigured M4U turns a translation fault into a literal bus hang instead
of a recoverable interrupt. This is independent corroboration of the
general failure class (M4U/SMI misconfiguration → bus hang, not just a
crash), though it's a separate register in the M4U *core* block, not the
per-larb `MMU_EN` bit build #108 targets. **Fallback note for later:** if
build #108's larb-level bypass doesn't resolve the hang, check whether the
M4U core's own `REG_MMU_CTRL_REG`/`INT_HANG_en` needs equivalent handling —
mainline never touches it either, since there's no MT6797 `mtk_iommu`
driver to own it.

**Next action:** flash build #108 to both `boot` and `boot2`, capture, and
compare against the bit-identical #106/#107 baseline (same last initcall,
same local-timer tick count, same heartbeat-miss timing) to see if the hang
signature changes at all.

**Update 2026-07-06 (build #108 flashed and captured twice — NO CHANGE,
B-13 remains deferred):** two boots captured from the same flashed image
(`logs/2026-07-06-109-smi-mmu-bypass-boot.log`, both banner `#27`). First
boot hung earlier/differently (before panel registration); a repeat capture
of the *same* image reproduced the #106/#107 baseline exactly (panel
registers, `cacheinfo_sysfs_init` runs, heartbeat MISS within a few jiffies
of baseline) — so the first boot's variance is run-to-run jitter, not a
fix effect. In both captures the SMI larb/common devices still never show
a `probe of ... returned` line, meaning the code path the fix touches
(`config_port()`) very likely never executed either time. **The fix is
kept** (`patches/v6.6/memory/0002-...patch` — it's a genuine latent
NULL-pointer-deref fix for any IOMMU-less SoC reusing this larb ops table,
harmless elsewhere) **but it does not resolve B-13.** See boot.md
"BUILD #108/#109" for full detail.

This closes out the last concrete, evidence-based hypothesis from the
vendor-source cross-check. The open question is now more fundamental than
IOMMU bypass: **why do the SMI larb/common devices never complete probe at
all**, for or against, across every build tested so far. No new hypothesis
is queued. B-13 remains formally deferred per CLAUDE.md principle 5
(bootability first, display optional) and the earlier
build #96/#97 pseudo-NMI ATF incompatibility. Device should be recovered to
`logs/2026-07-06-77-maxcpus8/new_kali_boot.img` (no display, known-good)
after this test.

**Update 2026-07-06 (vendor-console test — confirmed LK hardcodes
`printk.disable_uart=1`, no usable vendor dmesg obtainable this way):**
separately from the fix work above, tried to get a comparable *working*
display bring-up trace by capturing the vendor 3.18 kernel's own dmesg
over the same UART, to diff against our mainline failure logs. The one
full vendor boot capture on file
(`logs/2026-07-04-08-vendor-full-boot.log`, a visually-confirmed successful
boot to the Android desktop with working display) shows zero output past
`el3_exit` — already established as a "Pivotal Result" 2026-07-04 (silence
after `el3_exit` is not a failure signal for either kernel on this UART).
Root cause traced this session: the vendor 3.18 kernel's cmdline carries
`printk.disable_uart=1`, appended by the LK bootloader itself — not present
in the boot.img header's own cmdline field (`bootopt=64S3,32N2,64N2
log_buf_len=4M`) nor in the DTB's `bootargs` (checked
`docs/vendor-dtb/gemini_kali_boot.dts` line 11 — no `atag,printk-disable-uart`
property either).

Tested empirically with a new tool, `scripts/patch-vendor-cmdline.py`
(patches only the Android boot.img header cmdline field, byte-identical
kernel+ramdisk otherwise — confirmed via sha256): flashed
`OUTPUT/vendor-uart-test.img` (header cmdline appended with
`printk.disable_uart=0 ignore_loglevel`) to both `boot`/`boot2`, captured
twice (`logs/2026-07-06-111-vendor-uart-test-boot.log`, two power cycles,
`boot_reason=1` then `boot_reason=4`). LK's own boot log confirms it *did*
pick up the header override (`[LK_BOOT] Android Boot IMG Hdr - Command
Line: ...printk.disable_uart=0 ignore_loglevel`), but the final merged
cmdline handed to the kernel has LK's own `printk.disable_uart=1` appended
**after** it (`...printk.disable_uart=0 ignore_loglevel
androidboot.veritymode=enforcing printk.disable_uart=1 bootprof...`) — the
later occurrence wins, so the override is clobbered every time. Both boots
end at `el3_exit` with nothing further, identical to the untouched
baseline.

**Conclusion:** LK unconditionally enforces `printk.disable_uart=1` for
`buildvariant=user` regardless of boot.img header content. Getting a real
vendor-kernel dmesg trace would require patching LK's own binary (a
proprietary, unsourced blob — out of scope per CLAUDE.md's upstream-first
principle, and high-risk to the boot chain for a deferred/optional Phase 5
item) or finding a different debug channel entirely (e.g. `pstore`/
`last_kmsg` on a data partition, or `adb logcat` if the ramdisk's USB
gadget supports it). No further action queued; B-13 remains deferred with
no vendor-side comparison log available. **Device left flashed with
`vendor-uart-test.img` on both `boot`/`boot2` per explicit instruction —
not yet recovered to the known-good mainline `maxcpus=8` build.**

---

## 🟡 B-14 — Software reboot does not reset the SoC (hangs after `reboot: Restarting system`)

**Opened:** 2026-07-05. **Severity: low** — hard power-cycle works; costs
convenience, not progress. Not a Phase 4/5 gate.

**Evidence:** boot.md EIGHTEENTH RESULT
(`logs/2026-07-05-39-reboot-test-boot.log`). A clean systemd shutdown ran to
completion; the final line is `reboot: Restarting system`
(`machine_restart()`), then nothing — no preloader/LK output, manual
power-cycle required. The mtk-wdt, left armed by systemd-shutdown as a
backstop (`watchdog did not stop!`), also never fired.

**Candidate mechanisms (undistinguished):**
1. Vendor ATF's PSCI `SYSTEM_RESET` hangs given the SoC state our 6.6 boot
   leaves behind (`maxcpus=1`, `clk_ignore_unused`, no scpsys domains) —
   plausibly the same PSCI/SPM oddity behind the B-list SMP secondary-CPU
   hang.
2. The mainline mtk-wdt `restart_handler` (toprgu `WDT_SWRST`) is what
   actually ran and it fails/deasserts on MT6797 — it may also explain the
   silent watchdog (the handler reprograms `WDT_MODE` first).

**Next diagnostic step:** on a live system check `/sys/kernel/reboot` (or
boot once with `initcall_debug`/restart-handler tracing) to learn whether
PSCI or mtk-wdt owns the restart; then test the other path via the `reboot=`
cmdline parameter. Revisit alongside the `maxcpus=1` SMP fix — if PSCI
`CPU_ON` is broken, PSCI `SYSTEM_RESET` being broken too would point at a
single ATF-interface cause.

---

## 🟢 B-15 — RESOLVED: apparent mtu3/T-PHY "hang" was the documented UART/USB mux switching, not a driver bug

**Opened & resolved same day: 2026-07-05.** No hardware or driver fix
required — this is a methodology note, kept as a blocker entry because it
cost ~13 build iterations (#40–#52) before being correctly diagnosed.

**Symptom:** every build from #40 onward appeared to hang (silent console,
eventual watchdog reset) at the same point — the first SIF register touch in
mainline `mtk-tphy`'s U2 PHY init, immediately after clearing
`FORCE_UART_EN`/`FORCE_SUSPENDM` in the shared PHY control register.

**Investigation path (see boot.md TWENTY-FIRST through TWENTY-FIFTH RESULT):**
missing bus clock → traced to first SIF read → IPPC power state → port
PDN/HOST_SEL → PMIC MT6351 rails (VUSB33/VA10) all ruled out in turn by
builds #40–#47, each with targeted register dumps that showed the hardware
state was correct at every step.

**Root cause:** `FORCE_UART_EN` is a literal hardware mux-select bit — the
Gemini's left USB-C port shares one physical differential pair between the
UART console and USB2 D+/D− (documented in CLAUDE.md Phase 8 note since
build #40, but not connected to this symptom until build #52). Mainline
`mtk-tphy` correctly clears this bit as part of normal U2 PHY bring-up. Doing
so switches the mux away from the console mid-boot, so all serial output
after that line vanishes — indistinguishable from a hang if you assume the
console keeps working. Build #52
(`logs/2026-07-05-58-ssusb-mux-recovery-test`) proved this by re-setting
`FORCE_UART_EN` after the "hang" point and observing the debug line reappear
250ms later, unchanged.

**Fix:** none needed — reverted all debug instrumentation for a clean build
(#53, `logs/2026-07-05-60-ssusb-clean-no-debug`). Verified working via the
single-cable-swap protocol (serial *or* direct-to-Mac USB-C, never both):
gadget enumerates as RNDIS on the Mac, gets an IP, ping and SSH succeed. See
boot.md TWENTY-SIXTH RESULT.

**Lesson for future USB/mux debugging on this platform:** if a boot
"hangs" immediately after a T-PHY/mux-adjacent register write, first check
whether it's actually a console-mux transition (test by reconnecting via the
non-serial path) before spending cycles on power/clock/PMIC forensics.

---

## 🟡 B-16 — Cortex-A72 cluster (CPU8/CPU9) PSCI `CPU_ON` hang: separate from B-13, root cause unknown

**Opened 2026-07-06** (split out from item 4 under B-2/Phase 3, which had
speculatively lumped this in with B-13).

**Symptom:** with no `maxcpus` cmdline limit, CPU0–7 (both Cortex-A53
clusters) bring up cleanly in ~35ms, but the PSCI `CPU_ON` SMC issued for
CPU8 (first Cortex-A72 "big" core) never returns. ~14s later ATF's own
watchdog fires (`aee_wdt_dump`, `Kernel WDT not ready`) and the board
reboots. This is an ATF (BL31) firmware hang, not a Linux-side defect — the
boot CPU blocks inside the SMC instruction itself. See boot.md "PSCI CPU_ON
diagnostic".

**Workaround in place:** `configs/gemini-cmdline.config` uses `maxcpus=8`,
which avoids the A72 cluster entirely and boots all 8 A53 cores cleanly
(validated `logs/2026-07-06-77-maxcpus8/`). This is the current baseline;
full 10-core SMP is not required for bootability.

**Originally hypothesized** to share B-13's root cause (both symptoms being
"a power domain never comes up"). **Disproven 2026-07-06** (boot.md "BUILD
#11", `logs/2026-07-06-82-cpu8-scpsys-retest/`): before testing, checked the
actual MT6797 scpsys domain table
(`drivers/pmdomain/mediatek/mtk-scpsys.c`, `scp_domain_data_mt6797[]`) and
found it defines no CPU-cluster/MP power-domain entry at all — only
`VDEC`/`VENC`/`ISP`/`MM`/`AUDIO`/`MFG_ASYNC`/`MJC`. So the scpsys driver
(and its NULL-name probe-abort fix, B-13) has no code path that could
influence A72 cluster power-on. Confirmed empirically: built with the B-13
fix applied, display fragment excluded (to isolate the variable), and no
`maxcpus` limit — the resulting boot hung with the byte-for-byte identical
signature as before the fix existed
(`logs/2026-07-06-83-cpu8-scpsys-retest-boot.log`, 8 reboot cycles).

**Status: root cause narrowed 2026-07-06** (previously "unknown") via read-only
analysis of a third-party vendor kernel image — the Planet Computers Gemini
Debian/Halium build (`debian_boot.img`, 3.18.41, downloaded by the user to
`/Volumes/extdata/scratch/debian/`, not part of this repo). That kernel's
`strings` output retains full debug source paths (built by
`dguidi@nowhere`, `/home/dguidi/Desktop/Kernel/kernel-3.18/...`), which is how
this was extracted — no source tree itself was recovered, only build-time
path/log strings baked into the binary.

**Finding:** the vendor 3.18 kernel does not bring up the A72 cluster via a
plain unconditional PSCI `CPU_ON` at boot the way our mainline `psci.c` does
for every `possible` CPU. It has an entire vendor-only subsystem for this,
absent from mainline in every respect (confirmed: no `mcucfg`/`idvfs` nodes in
mainline's `mt6797.dtsi`, none in our `mt6797-gemini-pda.dts` — see "PSCI
CPU_ON diagnostic" in boot.md):

- `drivers/misc/mediatek/base/power/mt6797/mt_hotplug_strategy_{main,algo,cpu,ops_mt6797}.c`
  — a load-based governor that decides *when* to online/offline the A72
  cores; the big cluster is not simply "on" from boot.
- `drivers/misc/mediatek/base/power/mt6797/mt_idvfs.c` — "IDVFS" (Intelligent
  DVFS): sets up an **SRAM LDO + PLL for the big cluster over I2C6** before
  the cluster can run. Log strings recovered from the binary confirm this is
  a hard precondition, not best-effort: `"[mt_idvfs] FAILED TO PREPARE I2C
  CLOCK (%u). iDVFS only 750MHz."`, `"[mt_idvfs] SRAM LDO setting = %u(x100mv)
  success."`, `"[mt_idvfs] Error: SRAM LDO volte = %umv, out of range
  500mv~1200mv."`
- `mt_cpufreq.c` / `mt_cpufreq_hybrid.c` — cluster-specific DVFS built on top
  of the above.
- A **CPU-HVFS hardware sequencer**, kicked by a `swctrl` register write, that
  actually powers a cluster on — separate from and prior to any PSCI
  `CPU_ON`: `"[CPUHVFS] (%u) [%08x] cluster%u on, pause = 0x%x, swctrl =
  0x%x (0x%x)"`, plus a `cspm_cluster_notify_on` symbol.

**Refined hypothesis:** mainline's generic `psci_cpu_boot()` just issues the
SMC and assumes ATF handles everything. On this SoC, ATF's `CPU_ON` for an A72
core plausibly expects the "big cluster ready" precondition (voltage/PLL
settled) — normally driven from Linux by `mt_idvfs`/CPU-HVFS `swctrl` — to
already be true. Our kernel never drives any of that, so the SMC blocks
forever waiting on a state nothing ever sets. This would mean the hang is not
an ATF firmware bug but firmware correctly waiting on a real precondition —
narrowing the fix path from "maybe impossible without vendor ATF source" to
"port (or minimally reimplement) the `mt_idvfs`/CPU-HVFS pre-hotplug voltage
sequencing," though this is not yet confirmed against real register-level
behavior on this hardware and no code has been written yet.

**Not yet done:** no register addresses, I2C6/PMIC-wrap sequence details, or
CPU-HVFS `swctrl` offsets have been extracted — only log-string evidence that
these subsystems exist and gate cluster power-on. Next step if this is
pursued: locate the actual register writes (vendor kernel binary
disassembly, or the vendor DTB's `mcucfg`/`ptp3_idvfs` `reg`/`clock` values
already on file in `docs/vendor-dtb/gemini_kali_boot.dts`) before attempting
any kernel-side sequencing code.

**Recovery:** re-flash `logs/2026-07-06-77-maxcpus8/new_kali_boot.img` (sha256
`4643f685358efdaca7db5ac12e5ab8721f35c081ece18821801b8de46dc28078`) to both
`boot` and `boot2` if a full-SMP test build leaves the device in a reboot
loop. Verify recovery with a fresh capture showing the `#8` kernel banner
before relying on the device being back to a good state.

---


## 🟢 B-31 — Novatek touch firmware never reaches NORMAL_RUN (RESOLVED 2026-08-20)

**RESOLVED the same evening it was opened.** The cause was the DSI's horizontal
blanking, not anything on the input side.

This is a Novatek NT36672 **TDDI** part: the touch controller shares the
panel's silicon and power domain, and its sensor scan and calibration run
during horizontal blanking. `mtk_dsi_config_vdo_timing()` reserves
`data_phy_cycles * lanes` — 200 bytes here — out of the porches for LP→HS
transitions; LK reserves nothing. Both light the panel, which is why the
difference was written off (in the #45 gate work, hours earlier, by me) as
"not a defect". It is not a defect for the display. It starves the touch side
of its quiet window.

| | HSA_WC | HBP_WC | HFP_WC | result |
|---|---|---|---|---|
| mainline | 0x14 | 0x27 | 0x22 | 0xA0/0xA1 cycling, fw_ver valid 69/120 |
| LK | 0x1C | 0x94 | 0x74 | **0xA3 NORMAL_RUN**, stable, 120/120, contacts decode |

Isolating one at a time: **HFP_WC is necessary in every passing combination**
and needs margin from one of the other two. The front porch is the quiet window
immediately after active video — where a TDDI part scans.

Fix: `patches/v6.6/drm/0019-drm-mediatek-dsi-mt6797-lk-horizontal-word-counts.patch`,
same idiom as the PHY_TIMCON override in `drm/0015`. Verified on hardware
(`boot-touchfix2`, `873e79a5`, `6.6.0-dirty #16`): `reset_complete = 0xA3` at
boot with no userspace poke, panel gate PASS 13/13, touch drives the cursor.

Second bug found on top of it: the cursor moved but landed wrong, because the
kernel driver already reports landscape `0..2160 x 0..1080` and the libinput
`TransformationMatrix` rotated it a second time. It must be identity.

**The one thing that still stands from the investigation below:** touch shares
the panel's power domain and vanishes from the bus when the display blanks, so
suspend/resume has to re-init the controller (issue #39), and any measurement
taken with the display asleep is measuring a powered-down block.

The diagnostic history is kept below, including the eight hypotheses that were
ruled out — several of them mine, and the one I argued away.

---

## (history) B-31 — the investigation, before the cause was found

**Opened 2026-08-20 late.** This supersedes B-9 ("touchscreen chip identity
unknown") — the identity question is closed, and what is left is a different
and more specific defect.

### What is settled

The controller is a **Novatek NT36772-class** part at I2C 0x62 (reports and
firmware info at 0x01), on i2c-3. Its trim ID, read with the vendor's own
sequence (`nvt_bootloader_reset` → `nvt_sw_reset_idle` → write 0x00 0x35 →
page 0x01F6 → read 6 bytes at 0x4E):

```
trim ID: 00 00 03 72 66 03   ->  NT36772 map, EVENT_BUF_ADDR = 0x11E00
```

which is the value `gemini-nt36xxx.c` already defaulted to. **The memory map
was never the bug.** `FWINFO` at 0x78 on that page reads
`05 fa 12 20 04 38 08 70`: fw_ver 0x05 with complement 0xFA (the vendor's
`buf[1]+buf[2] == 0xFF` checksum passes), 18x32 sensor, 1080x2160. A real
firmware image is present and its info block is intact.

### The defect

The firmware **restarts about every 43 ms and never leaves initialisation.**
`RESET_COMPLETE` (0x60) cycles between `RESET_STATE_INIT` (0xA0),
`RESET_STATE_REK` (0xA1) and 0x00, and never reaches `REK_FINISH` (0xA2) or
`NORMAL_RUN` (0xA3). `HANDSHAKING` (0x51) reads 0x00 on every sample. The point
buffer at register 0x00 is a fixed pattern, byte-identical across every read —
uninitialised memory, which is what a firmware that never enters its main loop
leaves behind.

It is genuinely periodic, not noise. Paired reads of the same static register:

| gap between reads | agree |
|---|---|
| 0 ms | 98% |
| 5 ms | 71% |
| 20 ms | **20%** |
| 50 ms | 72% |

Adjacent transactions agree; transactions half a period apart disagree; a full
period apart they agree again. No per-transaction corruption can do that.

### Ruled out, each by measurement

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Wrong `EVENT_BUF_ADDR` | no | trim ID read; 0x11E00 confirmed |
| Missing firmware download | no | `novatek_ts_fw.bin` is in no partition of the stock image |
| Flaky I2C link | no | the paired-read table above |
| Transfer shape or length | no | 4 shapes, 10 lengths, identical result |
| CTP_RST mis-driven | no | 3 assert/release timings; duty cycle unchanged to the sample |
| Event buffer not writable | no | writes stick 57/60 |
| Missing HOST_READY handshake | no | `HOST_CMD=0x00` + `HANDSHAKING=0xBB` sent blind and phase-locked; no effect |
| Backlight | no | brightness 0 changes nothing |

### The one hard new constraint

**Touch shares the panel's power domain.** With `xset dpms force off` the
controller does not merely stop reporting, it **vanishes from the bus** —
150/150 I/O errors — and returns when the display does. Consequences:

- Screen blanking is a one-way trip for touch until suspend/resume re-inits the
  controller (issue #39). Blanking is disabled for now.
- Any panel-gate or touch measurement taken with the display asleep measures a
  powered-down block. This already produced one false alarm: a gate run where
  every DSI register read 0x00000000 looked like a catastrophic regression and
  was simply a 10-minute idle timeout.

### Where to look next

**Our DSI init table is NOT the problem — checked, and it came out clean.** The
vendor `init_setting[]` in `aeon_nt36672_fhd_dsi_vdo_x600_xinli.c` is guarded
by `#if 1 / #else / #endif`; the live branch has **exactly 183 rows**, which is
exactly what our panel driver pushes (`init sequence complete: 183 commands in
156 ms`). The table is faithful to the row. That was the leading suspect and it
is out.

**The better hypothesis, and it links two measurements that were made for
unrelated reasons.** This is a Novatek TDDI part, so the touch scan and its
calibration happen *during display blanking*. And our horizontal blanking is
much shorter than LK's, because mainline reserves `data_phy_cycles * lanes` —
200 bytes on this panel — out of the porches:

| | HSA_WC | HBP_WC | HFP_WC |
|---|---|---|---|
| ours (mainline's derivation) | 0x14 | 0x27 | 0x22 |
| LK | 0x1C | 0x94 | 0x74 |

A touch side that never gets its quiet window is exactly a calibration that
never completes — which is precisely the observed `RESET_STATE_REK` that never
becomes `REK_FINISH`. Note this also explains why the cycling needs the DSI up
and is unaffected by CTP_RST, the backlight, the handshake, or the page.

**Testable without a rebuild:** write LK's word counts into the live DSI block
with `busybox devmem` at `0x1401C050/54/58` and re-measure the touch duty
cycle. These are the values LK itself programs and the panel demonstrably runs
on them, so it is not an arbitrary poke, and the watchdog now recovers the
device if the display wedges.

Second candidate, if that comes out negative: supply. The vendor DTB powers
touch from MT6351 VLDO28; `/sys/class/regulator` on our kernel has no touch
consumer at all, so whatever LK left is what it gets.

Third: read `RESET_COMPLETE` on the vendor kernel on this same unit, where
touch works. 0xA3 there and cycling here would confirm the whole class.

Second candidate: supply. The vendor DTB powers touch from MT6351 VLDO28;
`/sys/class/regulator` on our kernel has no touch consumer at all, so whatever
LK left is what it gets.

Scripts: `scripts/nvt-trim-probe.py` and `04-docs/touch-probes/`. They drive
the controller from userspace over `/dev/i2c-3`, which turns a
build-flash-boot cycle into a 30-second read — worth reaching for before any
kernel change here.

---

## 🔴 B-28 — MediaTek download/vendor mode: entered easily, exit NOT understood

**RETRACTION.** An earlier version of this entry claimed download mode was a
controllable state that `mtk reset` plus a port-dark cycle could exit, and
marked tracker #49's remaining criteria met. **That was wrong on every count**
and is corrected here. The claims were written from a single unrepeated
observation without checking the mechanism.

### What is actually established

**Entering it is easy and repeatable.** A reboot with the hub port powered, and
`adb reboot bootloader`, both land the device in a MediaTek vendor mode:
`0e8d:2000` then `0e8d:2008`, one vendor-specific interface (class ff/ff/00,
bulk in + bulk out + interrupt in), product and manufacturer strings both
"Android", serial `0123456789ABCDEF`.

**Holding the port dark does NOT prevent it.** Tested directly: port off →
`systemctl reboot` over Wi-Fi → port on 75 s later → the device still ended up
parked. So the "cable present at power-on causes the park" model is at best
incomplete, and #49's criterion about avoiding the park by holding the port
dark is **NOT met**.

**A port-power cycle does NOT clear it.** Tested in isolation: 120 s dark, then
on — still `0e8d:2008`.

**mtkclient cannot talk to it.** With debug logging it never connects, over raw
USB or over a bound `/dev/ttyUSB0`; it sits at "Waiting for PreLoader VCOM".
My earlier claim that `mtk reset` recovered the device is false — mtkclient
never established a connection at all.

**It does not speak the preloader protocol.** The classic MediaTek handshake
(0xA0→0x5F, 0x0A→0xF5, 0x50→0xAF, 0x05→0xFA) sent raw to the bound tty gets
**no response to the first byte**. So this is not a BROM/preloader waiting for
a host, whatever the PID suggests.

That last point plus the "Android" product string and the Android adb serial
suggests this is more likely **META mode** — the MediaTek factory test mode LK
supports (`meta com type`, `atag,meta`, `/meta_init.rc` all appear in the
extracted LK) — reached via the misc/para boot flag that `adb reboot
bootloader` sets. Not confirmed.

### The one unexplained observation

The device *did* transition from this state to a booted Android once, at
~16:20, after a sequence of (failed) mtk reset + 80 s port-dark + port on. It
has not repeated. A preloader timeout would explain it, but the device has now
sat parked for well over half an hour without booting, which argues against a
simple timeout — and suggests the mode reached via `adb reboot bootloader` may
be persistent where the earlier one was not.

### Operating rules until this is understood

- **Do not run `adb reboot bootloader`.** It is what put the device here, and
  LK's fastboot was never reached.
- **Do not software-reboot the experimental system** while its exit path is
  unknown.
- Treat this state as needing a physical clear: power held ~10 s, and if a
  normal power-on lands straight back in it, Esc + silver + power for 15-20 s
  to clear the boot flag.

### Useful thing that did come out of it

`scripts/lab-setup/53-gemini-mtk-preloader.rules` binds usbserial's generic
driver to the MediaTek download-mode IDs so `/dev/ttyUSB*` appears at all —
without it mtkclient can never see the device even in a state where it would
otherwise work. That is worth keeping regardless.

## 🟢 B-29 / B-30 — RESOLVED 2026-08-20 (late): the loop closes

**The decisive measurement was finally run, and it passed.** Everything below in
B-29 and B-30 stands as the diagnostic history; this is the outcome.

**Experiment.** Device booted and healthy on `boot-touch-page.img` (`6aafdd16`,
`6.6.0-dirty #14`) in p1, BCB armed at `boot-recovery` and *sticky*
(`/etc/gemini-bcb-sticky` present, so the disarm service leaves it), systemd
petting `/dev/watchdog0` at 30 s, `panic_timeout=0` so a panic hangs forever.
Fired `echo c > /proc/sysrq-trigger` over a detached SSH at **21:33:04**.

**Result: it came back by itself at 21:35:37 — about 2 min 33 s, zero human
action.** Timeline matches the mechanism exactly: ~31 s of watchdog countdown,
then LK, then a ~75 s boot, then the network.

**Proof it was a real reset and not a warm continuation:** the fresh boot's own
dmesg shows the driver finding LK's configuration *again* —

```
[    0.624325] mtk-wdt 10007000.watchdog: WDT_MODE 0x0000005d -> 0x00000011
               (reset is now internal and immediate)
[    0.626040] mtk-wdt 10007000.watchdog: Watchdog enabled (timeout=31 sec)
```

`WDT_MODE` was 0x5d at probe, which is LK's value. The bootloader ran. Uptime
after recovery was 0 minutes.

**So the fix is the pair, not either half.** Clearing `IRQ_EN | DUAL_EN` alone
was verified 2026-08-20 midday and did *not* make a reboot work — that failure
is recorded below and was correct. `EXRST_EN` (bit 2) was the missing piece: it
routes the reset to the PMIC, which on this board powers the device off instead
of restarting it. Clearing all three at probe (0x5d → 0x11) makes both the
timeout path and the `WDT_SWRST` path reset the SoC internally.

**Consequences, replacing the operating rules the earlier entries set:**

- An unattended hang is **no longer one-shot**. A kernel that hangs after probe
  costs ~2.5 minutes, not a physical power cycle.
- B-25's kexec ban still stands — this changes nothing about kexec, and flash +
  cold boot remains the way to enter a kernel.
- The remaining genuinely-fatal case is a kernel that hangs **before** the
  watchdog driver probes at 0.62 s. That is still a power cycle, and it is why
  "never put an unproven kernel in p1 while the BCB points there" survives as a
  rule even now.

---

## 🔴 B-30 — a SOFTWARE reboot never comes back — SAME ROOT CAUSE AS B-29

**Root-caused 2026-08-20.** B-29 and B-30 are one bug wearing two hats, and the
one-line kernel fix for B-29 should fix both.

`mtk_wdt_restart()` is registered as the system restart handler
(`watchdog_set_restart_priority(..., 128)`), so `reboot` on this device does not
go through PSCI — it issues a **watchdog software reset**:

```c
while (1) {
        writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
        mdelay(5);
}
```

With `WDT_MODE`'s `IRQ_EN | DUAL_EN` set — inherited from LK and never cleared,
see B-29 — that software reset raises an **interrupt** rather than resetting the
SoC. The kernel is already tearing down for reboot, so nothing services it, and
the machine simply stops. Same broken mechanism as the timeout path: expiry
interrupts a CPU that is no longer listening.

**Decisive evidence, and it is what the panel bought us.** After a failed
reboot the panel is COMPLETELY DARK — no kernel console *and no LK splash*.
LK draws its splash within a moment of any real reset, so its absence means the
bootloader never ran at all. The failure is therefore before the kernel: not a
hang, a failure to restart. Nothing else in the lab could have told us that.

**Consistent with every observation:**

| Reboot | Watchdog config in effect | Came back? |
|---|---|---|
| from the experimental system (x4) | LK's IRQ+DUAL, uncorrected | **never** |
| `adb reboot` from Android | the vendor 3.18 kernel's own | **yes** |
| cold power cycle | not used at all | **always** |

Every software reboot from the experimental system has failed; the only one that
worked was issued from Android, whose vendor kernel programs the watchdog
itself.

**Fix:** the same patch as B-29 —
`patches/v6.6/misc/0001-watchdog-mtk-wdt-clear-bootloader-irq-dual-mode.patch`,
clearing the bits at probe. Flashed to p1 as `boot-wdtfix.img` (`e314debe`).

**PREDICTION TESTED 2026-08-20, AND IT FAILED.** The fix does what it claims —
`mtk-wdt 10007000.watchdog: cleared bootloader IRQ/DUAL mode: expiry is now a
reset` at 0.706 s, and `WDT_MODE` reads 0x15 with IRQ_EN/DUAL_EN clear — but a
software reboot issued from that kernel **still did not come back**, and the
panel was dark again (no LK splash). So the interrupt-mode bits were a real
defect, and they were not the reason `reboot` fails.

**Next candidate: `WDT_MODE_EXRST_EN` (bit 2), still set (0x15 = EN | EXRST_EN |
AUTO_START).** That bit routes the watchdog's reset to an *external* pin — the
PMIC — instead of resetting internally. If that path is not wired or not
honoured on this board, the reset request goes nowhere, which matches the
symptom exactly: the SoC simply stops. The driver already supports clearing it
via the `mediatek,disable-extrst` DT property, which this DT does not set, so
LK's choice survives.

**Cheap decisive test, no rebuild needed** — from a booted system:
```
busybox devmem 0x10007000 32 0x22000011   # EN | AUTO_START, EXRST_EN cleared
busybox devmem 0x10007014 32 0x1209       # WDT_SWRST: reset now
```
If the device comes back, `mediatek,disable-extrst` in the DT is the fix. If it
does not, the next candidate is `WDT_MODE_CNT_SEL` (bit 8, reset-by-TOPRGU),
which this compatible does not set.

**Cross-check available for free:** stock Android reboots reliably on this
hardware, so whatever it leaves in `WDT_MODE` is a known-good configuration.
Reading 0x10007000 from Android answers this outright.

## 🔴 B-30 (original notes) — the tally that led here

**Opened 2026-08-20. This, not any individual driver bug, is what blocks the
hands-free lab**, because every unattended cycle needs a restart and restarts
are unreliable.

Tally from one session, all with the same hardware and cabling:

| Reboot | Method | Came back? |
|---|---|---|
| 16:05 | `systemctl reboot`, port dark | no |
| 17:12 | BCB + `adb reboot` | **yes** |
| 18:06 | BCB + `reboot` | no |
| 18:26 | BCB + `reboot` | no |
| every cold power cycle | power button | **yes, every time** |

A failed one presents identically each time: no USB connect bit at the hub
(`0100 power`), no Wi-Fi, dark panel. Indistinguishable from a powered-off
device, which is how it was misread more than once today.

**Not yet root-caused.** Candidates: the mtu3 gadget failing to re-initialise
without a true power cycle (long-standing on this hardware, see the kexec
notes), or the SoC not fully resetting on a warm restart. Distinguishing them
needs the serial console, since by definition there is no other channel.

**Consequence:** until this is understood, do NOT chain unattended
flash-and-reboot cycles. Each software reboot is roughly a coin flip, and a
lost one costs a physical power cycle. The pieces that make a hands-free lab
(software slot selection via p1+BCB, flashing, offline image editing, remote
eyes, state classification) all work — the loop still does not close, because
the device cannot reliably restart itself.

## 🔴 B-29 — the RGU watchdog INTERRUPTS instead of resetting: a hang never self-recovers

**Opened 2026-08-20. This is the blocker for the whole hands-free lab**, and it
was found by finally running the test rather than reasoning about it.

**The experiment.** Device booted and healthy, watchdog armed
(`WDT_MODE=0x0000005D`), systemd petting at 30 s, `panic_timeout=0` so a panic
hangs forever rather than rebooting. Triggered a deliberate kernel panic via
`sysrq-trigger`. Then watched for 490 s.

**It never reset.** No re-enumeration, panel dark, nothing. The device needed a
physical power cycle.

**Cause.** Decoding `WDT_MODE=0x5D` against the bit definitions in
`drivers/watchdog/mtk_wdt.c`:

| bit | name | value | meaning |
|---|---|---|---|
| 0 | `WDT_MODE_EN` | 1 | enabled |
| 2 | `WDT_MODE_EXRST_EN` | 1 | drives the external reset pin |
| 3 | `WDT_MODE_IRQ_EN` | **1** | **expiry raises an INTERRUPT, not a reset** |
| 4 | `WDT_MODE_AUTO_START` | 1 | |
| 6 | `WDT_MODE_DUAL_EN` | **1** | **two-stage: interrupt first, reset only after** |

A panicked kernel has stopped servicing interrupts, so the interrupt stage goes
nowhere and the reset never arrives. **An armed watchdog that interrupts a dead
kernel is decoration.** Every "leave the watchdog armed so a hang recovers"
rule in this project has been resting on a mechanism that does not fire.

**Root cause found in the driver.** `mtk_wdt_init()` adopts a watchdog the
bootloader left running — it sets `WDOG_HW_RUNNING` and programs the timeout —
but never touches `IRQ_EN`/`DUAL_EN`, so LK's choice survives. And because
`WDOG_HW_RUNNING` is set, the core never calls `->start`, so
`mtk_wdt_start()`'s own clearing of those bits never runs, not even when
userspace opens `/dev/watchdog`.

**Fix:** `patches/v6.6/misc/0001-watchdog-mtk-wdt-clear-bootloader-irq-dual-mode.patch`
clears them at probe, ~1 s into boot. Staged in `boot-wdtfix.img`
(`e314debe`), NOT yet verified on hardware.

**The first attempt at this was wrong and is worth recording:** a userspace
service (`gemini-wdt-hardreset`) that cleared the bits after boot. It works
when it runs — WDT_MODE went 0x5D to 0x15 — but it is useless for the case that
actually matters, because a boot that never reaches userspace never runs it.
The fix has to be in the kernel, before the thing it protects against.

**Until this is verified on hardware, treat every unattended experiment as
one-shot:** a hang costs a physical power cycle, so the payload in the
software-selectable slot must be one that has already been observed reaching
userspace. This also explains B-26's 8-minute non-reset and, most likely, why
several "it booted but never came back" episodes today needed hands.

## 🔴 B-26 — a booted kernel with no userspace never resets: the watchdog pets itself

**Opened 2026-08-20.** The hardware watchdog cannot rescue an unattended hang
in the one case that matters most, and it took losing the device to see it.

**Evidence.** A `kexec` into the known-good kernel produced a system that
booted far enough for the built-in g_ether gadget to enumerate (host log:
`Product: RNDIS/Ethernet Gadget`, `Manufacturer: Linux 6.6.0-dirty with mtu3`,
one second after the old kernel's gadget disconnected) but never reached
userspace: the link carrier stayed down (`ip link set usb0 up` is
gemini-usb-net's job), Wi-Fi never associated, and the panel went from lit to
dark. It then sat in that state for **8 minutes with no reset**, with the RGU
watchdog armed the whole time.

**Cause.** `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y` makes the watchdog core
adopt the running RGU that LK left armed and pet it from a kernel worker until
userspace opens `/dev/watchdog`. With `CONFIG_WATCHDOG_OPEN_TIMEOUT=0` that
wait has **no deadline** — so a kernel that boots but never starts userspace
keeps its own watchdog fed indefinitely. The safety net feeds the thing it is
supposed to catch.

`gemini-watchdog.service` and systemd's `RuntimeWatchdogSec` only cover the
window *after* PID 1 is running. This is the window before it.

**Fix (staged, untested on hardware):** `watchdog.open_timeout=120` in
`configs/gemini-cmdline.config`. Userspace then has 120 s to claim the device —
systemd claims it within about two seconds of PID 1 starting — and if it never
does, the core stops petting and the RGU resets. Turns "dead device" into
"another boot attempt".

**Related trap, same root:** `CONFIG_CMDLINE_FORCE=y`, so the compiled-in
command line is the only one that takes effect. What LK passes is discarded —
and so is `kexec --command-line`. A parameter not in
`configs/gemini-cmdline.config` is not set, however it was supplied.

## 🔴 B-25 — kexec is not a safe autonomous reboot on this hardware

**Opened 2026-08-20.** Superseding the more optimistic earlier note that kexec
gives "a FULLY AUTONOMOUS reboot".

A kexec into the **known-good, currently-running** kernel (same Image and DTB
extracted from the flashed `boot-gpustack4.img`; the local rebuild of that DTB
is byte-identical, so the inputs were right) failed to reach userspace. This
was a deliberate null test — the same kernel that was running a moment earlier
— so the failure is kexec itself, not the payload.

Then it got worse: the wedged gadget could not be recovered from the host. The
hub port shows `power connect` but never `enable`; the device answers
`-110 device descriptor read` and `-62 device not accepting address`. A 4 s
link-down did not clear it and neither did 45 s. The device-side mtu3
controller is wedged and, because this hub does not switch VBUS (B-27), the
host cannot present a true unplug. **Recovery required a physical power cycle.**

**Consequence for the hands-free lab:** kexec cannot be the mechanism for
entering a kernel unattended. Prefer flash + cold boot, which is also what the
project's own operating notes said and what this test ignored. kexec remains
useful only when a human can power-cycle.

## 🟡 B-27 — the lab hub advertises per-port power switching but does not switch VBUS

**Opened 2026-08-20.** The Anker hub's two Realtek RTS5411 chips report `ppps`
and the port status word clears its POWER bit, and the device does leave the
bus — but the 5 V rail is hardwired. Measured with Android logging its own
charger across a 12 s "power off": ChargerVoltage 5043 → 5024 mV, current 442 →
446 mA, status `Charging` throughout, while the device simultaneously vanished
from the bus.

So `gemini-port.py` is a **data replug, not a power cycle**. It re-enumerates a
healthy device fine (0.9 s), but it cannot clear a wedged device-side UDC
(B-25) because the device never sees VBUS drop. A hub with genuine VBUS
switching would fix that specific case; nothing in software will.

Side effect worth keeping: the device charges while "unplugged".

**Update 2026-08-21 — the cable-at-boot download-mode park did not reproduce.**
Two unplanned hardware watchdog resets happened tonight with the cable attached
— one from a panfrost probe wedging the SoC, one from a display measurement
reading an unpowered register block — and **both came back to a normal boot**
with no human action (once into Android because the BCB had been consumed, once
into the experimental system). That is not proof the trap is gone; it is two
counter-examples on the current LK/BCB arrangement, and it means the gap
per-port VBUS switching was partly meant to close is narrower than #49 assumed.
The gap that remains is real and unfixable in software: a wedged device-side UDC
(B-25) clears only when the device sees VBUS drop, and on this hub it never
does.

## 🟢 B-24 — RESOLVED 2026-08-21, but NOT root-caused: the hang was made harmless, not explained

**Read this before the history below.** The panel displays the kernel console
on a cold boot, the system reaches userspace with no watchdog reset, the panel
gate reports PASS 13/13, and the LXQt desktop runs on the glass. The init
sequence completes in **156 ms for 183 commands** with no command slow enough
to trip the >= 200 ms warning. The hang does not reproduce.

**What is not known is which command was stalling.** `panel/0009` gave the init
sequence a deadline (`init_budget_ms`, default on) so a stalled transfer aborts
the sequence instead of wedging DRM bind, and `drm/0013` forces command mode at
poweron. Between them the symptom disappeared. Nobody ever read the last index
before the stall, which is what the plan called for and what would have named
the cause.

So: the failure mode is bounded rather than understood. If it returns, the
instrumentation to name it is already in the tree and disabled by default —
`panel_gemini_fhd.init_verbose=1` logs every command with its duration, and
`init_budget_ms=0` removes the deadline so the stall is reproducible again.
Marked resolved because the acceptance criteria of #45 are met on hardware; the
first criterion, "the stalling command or condition is identified", is not, and
saying so is more useful than ticking it.

## (history) B-24 — NT36672 panel retarget hangs the boot at DRM bind (watchdog loop)

**Opened 2026-08-20.** The NT36672 panel retarget (`panel/0007` + `panel/0008`,
fork `33d641c`) **does light the physical panel** — the operator confirmed
kernel text on the glass, a first for this port, which closes the identity half
of B-17. But the kernel then **hangs during display bring-up** and the RGU
watchdog resets the device in a loop.

**Evidence:** on-screen output stops at the `mediatek-drm ... bound 1401xxxxx`
cluster (~0.86 s) with nothing after. Host-side USB shows the SoC re-enumerating
on a fixed cadence — `MT65xx Preloader` at 09:52:24 and again at **09:52:54,
exactly 30 s apart**, matching the RGU watchdog's 31 s timeout. The RNDIS gadget
appears briefly (09:46:00) when a boot gets far enough, then dies with the next
reset. This also explains a long run of `-110`/`-62` enumeration failures that
were misread as cable/mtu3 flakiness.

**Suspect:** the 183-command init table pushed in `mtk_dsi_host_transfer()`.
Most likely a transfer that never completes (or a long series of timeouts)
inside `drm_panel_prepare()`, which runs under the CRTC enable path. Note the
init runs in command mode (via `drm/0013`) before the switch back to burst.

**Next steps:** (1) bisect the table — send only the page-select + sleep-out +
display-on subset first; (2) instrument `ssd2092_init_sequence()` with a
per-command `dev_info` and read the last one printed; (3) check whether any
command's payload exceeds the DSI FIFO / needs the CMDQ path; (4) consider a
bounded timeout so a stalled transfer cannot wedge bind.

**PROCESS FAILURE (mine) — do not repeat:** the new kernel was flashed to
**both** boot2 and boot3, leaving no known-good experimental slot; recovery
required booting boot1 Android and restoring `boot-gpustack4.img`
(`4a912fae…`) to p30/p31 over adb. **Flash one slot at a time and keep the
other known-good.** Android on boot1 remains the deep fallback and was
untouched throughout.

## 🔴 B-47 — eight cores at maximum takes the machine down, and the battery sags 620 mV first

### ANSWERED 2026-08-22, same evening: it is a total-power limit, not a frequency limit

Netconsole armed and proven to deliver first, every step announced to
`/dev/kmsg` **before** it was taken, so the step that kills the machine still
names itself. Two experiments, the second run twice.

**Staircase** — all eight cores loaded, ceiling raised one OPP pair at a time,
20 s per step:

| LL / L | vbat | result |
|---|---|---|
| 624 / 650 MHz | 4.164 V | held |
| 806 / 832 | 4.164 V | held |
| 1014 / 1092 | 4.164 V | held |
| 1222 / 1209 | 4.164 V | held |
| **1391 / 1495** | **4.044 V** | **held — the highest proven all-core point** |
| **1495 / 1755** | — | **DEAD, in under 20 s** |

**Core count at the very top** — both clusters pinned at 1547/2002 MHz, cores
added in cluster-balanced pairs. Run twice, same answer both times:

| cores | vbat | result |
|---|---|---|
| 2 | 4.164 V (flat) | survived |
| 4 | 4.144 V | survived |
| 6 | 3.90 V / 3.80 V | **survived** |
| 8 | — | not attempted; the staircase already showed 8 dies below this |

**So maximum frequency is fine on up to six cores.** What kills this machine is
all eight at once near the top of the tables. That is a total-power ceiling,
and it means a flat frequency cap is the wrong shape of fix in principle —
though it is the only one available in stock cpufreq, see below.

**The death is silent, and that is the result.** netconsole's last line is the
announcement of the fatal step and then nothing at all: no regulator
complaint, no thermal event, no oops, no panic. B-47 asked for exactly this
distinction and named the third case the signature of a supply collapse. A
kernel that stops mid-instruction without a word did not have time to log.

Two supporting observations point the same way. The charger thermal zone never
moved off 38 °C through any of it, so nothing suggests heat. And the second
death was reclaimed by the watchdog in ~30 s, where the first left the device
parked in the preloader needing hands — consistent with how completely the rail
collapsed each time, and a reminder that recovery is not guaranteed.

### AND THE FIRST CAP WAS WRONG, for a reason worth keeping

The obvious mitigation was to cap at 1391/1495 MHz — the highest point that had
*survived* a 20 s hold. Installed, and then given the workload it exists to
survive: eight cores for 120 s. **It died too, at ~110 s.** The battery trace is
the whole explanation:

```
4.164 -> 4.004 -> 3.964 -> 3.924 -> 3.864 -> 3.764 -> dead
```

**This is an energy limit, not only an instantaneous one.** The pack drains
faster than a 2.2 W hub port refills it (B-42), the voltage walks down under
sustained load, and the rail lets go when it gets low enough. Frequency sets how
fast that walk is, not whether it happens. "Survived 20 s" was never evidence of
safety, and a test shorter than the depletion time could not have shown it.

### The cap that holds, and the criterion that chose it

Re-read the staircase with the right question. Through the first four steps the
pack sat at **exactly 4.164 V — not one 20 mV ADC step of movement** — across
624/650, 806/832, 1014/1092 and 1222/1209, some 80 s of cumulative eight-core
load. The first movement of any kind was at 1391/1495. So the criterion is not
"survived" but **"no measurable drain"**: below that line the charger keeps up
and the load is sustainable rather than merely endurable.

**Mitigation, installed and accepted:** `gemini-cpufreq-cap`
(`device-config/sbin/`, plus a systemd unit) pins `scaling_max_freq` at
**1222 MHz / 1209 MHz**. Acceptance test — eight cores for **240 s**, twice the
duration that exposed the first cap's mistake:

```
t=30..150s   vbat 4124000, flat, not one ADC step
t=180s       vbat 4164000     <- rising: net charging under full load
t=240s       vbat 4124000
SURVIVED. ended at 4.164 V, above the 4.144 V it started at
```

It is userspace, so lifting it for a burst on a few cores is two `echo`s and no
reflash, and the kernel keeps the full capability it has demonstrably got — six
cores at the very top of both tables is proven twice. It defaults on because
`make -j8` is exactly the fatal shape.

**The honest cost:** 1209 MHz on the big-little cluster is marginally *below*
the 1274 MHz the bootloader used to leave it at. Under schedutil that ceiling
only binds when all eight cores are genuinely busy, which is the case this
exists to survive.

**The lead worth chasing before accepting any of this as permanent.** The device
is on an Anker hub port measured at **~445 mA**, about 2.2 W — `gemini-port.py`'s
own header records that measurement — against a load of several watts. That the
binding constraint turned out to be *depletion* rather than peak draw makes this
far more likely to be the hub's ceiling than the phone's. **Retest on a proper
charger before concluding the silicon cannot do it.** One physical change could
return the whole top of both tables.

Still open: per-cluster bisection (the staircase moved both clusters together,
so it is not known which one's contribution mattered), and the absence of any
CPU thermal sensor or throttling, which is a real gap whatever caused this.


**Opened 2026-08-22, on `#51`, the first kernel with working CPU DVFS.**

The DVFS itself is sound (B-40, and the whole of `04-docs/STATE-2026-08-22b.md`).
This is about what happens at the top of the table under real load.

**The experiment.** Two phases on `#51`:

| phase | what | result |
|---|---|---|
| 1 | ~1040 LL + ~1120 L OPP transitions, both clusters, CPUs otherwise idle | **clean**, PLLs correct afterwards |
| 2 | all eight cores pinned at 1547 / 2002 MHz, Vproc 1.20 V | **died between 15 s and 30 s** |

Phase 1 matters as much as phase 2: **over two thousand frequency transitions
with no fault**, which is what says the clock/regulator path is not the
problem.

**What was seen before it went.** At t=15 s, the one sample that got out:

```
temp = 38000 mC        (charger sensor -- NOT a die sensor, see below)
vbat = 3524000 uV      down from 4144000 uV, "Full", before the load
LLclk = 1547000000     Lclk = 2002000000        both correct
```

A **620 mV sag in under fifteen seconds**, and then the machine was gone. It
did not reboot: it was found parked in the MediaTek preloader (`0e8d:2008`),
which is where this device lands after a power-loss reset with a host
attached, and it needed the physical Esc+silver+power recovery.

**The hypothesis, stated as a hypothesis.** This looks like power delivery, not
DVFS logic: eight A53s at 1.55/2.0 GHz and 1.20 V is by a wide margin the
largest current this machine has ever been asked for, the charger supplies
roughly 2.2 W against a load of several, and B-42 already records that this
device draws faster than it charges. But **it is not proven**, and the reason
it is not proven is a plain instrumentation failure on my part: netconsole was
not armed for the run. The handoff said in as many words to decide these
questions from netconsole, and I ran the single riskiest experiment of the
session without it.

**What is NOT eliminated.** A die thermal shutdown. There is no CPU
temperature sensor on this machine at all -- the only thermal zone is
`bq25890-charger-0` and the only cooling device is the GPU's devfreq -- so
`CPUFREQ_IS_COOLING_DEV` registers a cooling device that nothing will ever
bind to a trip point. MT6797 has an on-die thermal controller and mainline's
`mtk_thermal` has no entry for it. **This SoC currently has no CPU thermal
throttling of any kind**, which is a gap worth closing regardless of what
killed it here.

Also not eliminated: the CCI. It is still at the ~630 MHz the bootloader chose
while the CPUs run at three times that, and it shares VPROC1 with them.

**What unblocks it.**

1. Arm netconsole, then repeat. The distinction that matters is whether the
   last thing the kernel says is a regulator/PMIC complaint, a thermal event,
   or nothing at all -- the third being the signature of a supply collapse.
2. A staircase rather than a step: all eight cores loaded, `scaling_max_freq`
   raised one OPP at a time, watching `voltage_now`. That finds the actual
   limit instead of asserting one.
3. Until then, **cap `scaling_max_freq` in userspace**, not in the OPP table.
   The kernel should keep the full capability it has demonstrably got; a
   frequency ceiling is policy and belongs where policy can be changed without
   a reflash. Each occurrence of this fault costs a physical key-combination
   recovery, which is the real reason to default it safe.

**Do not read this as "the OPP tables are too aggressive."** Every voltage in
them is the vendor's own static, pre-EEM figure for this speed bin -- i.e. the
conservative ones, higher than the values Android actually runs after
calibration. If 1547/2002 MHz is not sustainable on this hardware, the reason
is the battery or the case, not the table.

---

## 🔴 B-45 — i2c6's address phase works and its data phase does not, which is why the DA9214 "isn't there"

### UPDATE 2026-08-21 (late): both candidates are dead, and so is a third

A boot1 Android trip read the vendor's own device tree and live pin state, and
the symptom was re-measured more carefully on `#43`/`#44`. **Do not spend
another session on the two candidates below — they are eliminated.**

*The symptom, sharper than it was.* Every address on this bus NACKs correctly —
0x08, 0x20, 0x40, 0x50, 0x55, 0x60, 0x67, 0x6c, 0x77 all fail — **except
0x68-0x6b, which ACK and also accept byte writes.** So a slave really is there
and our master really is working; what fails is only the read data phase, which
returns a constant 0x00 at all four addresses, forty reads out of forty.

One caution about the instrument: `i2cdetect -q` (SMBus quick write) finds
*nothing at all* on this bus. That looks like proof of an empty bus and is not —
plain byte writes to 0x68-0x6b succeed. A zero-length write is simply a
transfer type this path does not do. It is the same trap as B-43's, in the
opposite direction.

*Eliminated:*

| candidate | verdict | evidence |
|---|---|---|
| **clock-div** (#57's second) | **dead** | vendor's `i2c@1100e000` has `clock-div = <0x0a>`, same as ours; so does every other i2c node in the vendor DTB |
| **pinmux** (#57's first) | **dead** | vendor has GPIO151/152 in **mode 1** = SCL6_0/SDA6_0, exactly what `i2c6_pins_a` selects; our own mode registers read `0x1` for both |
| **no pull-ups / open-drain** (dts/0043's stated reason) | **retracted** | both pads read **DIN = 1** on our kernel (GPIO DI `0x10005240` bits 23, 24) — idle-high, like the working i2c7 pads beside them |
| **controller timing generally** | **dead** | i2c7 has the same driver data, same `clock-div`, same 156 MHz source clock, and reads its RT5735 ten times out of ten |
| **bus speed** (new, from the trip) | **tried on `#44`, retracted** | vendor runs this controller at **3400000** (0x0033E140, HS mode; `I2C_HS_FLAG`, `timing = 3400` in the vendor `da9214.c`) against our inherited 400000. It does not fix it and makes it *worse* — at 3.4 MHz the reads that returned 0x00 mostly fail outright. Reverted on `#45`. |

*The clue that is left, and the next hypothesis.* Reads are **not uniformly
dead**: 0x00 nearly always, but occasionally a real-looking byte — `0x0b` and
`0xd6` on `#42`, `0x05` on `#44`. A bus that loses most bits but not all is not
an unpowered slave and not a wrong mux. It looks like **contention**.

And there is a specific reason to suspect it here: this is the one controller
the vendor DTB marks **`mediatek,appm_used`**, and mainline `i2c-mt65xx` knows
nothing about that property. If a hardware power-management engine also masters
this bus — and this SoC demonstrably runs a DVFS co-processor that owns CPU
rails (B-40's CPUHVFS) — then our transfers are colliding with its. **That is
untested.** What would test it: instrument the controller's arbitration-lost
and NACK status bits on a failed read, rather than only observing the byte.

*One more thing the A72 work should not wait on.* #55's cpufreq half needs this
bus. Its **A72 half does not** — whether cluster 2 can be brought up at all is a
CPUHVFS/firmware question (B-40), and nothing here changes it.


**Opened 2026-08-21.** This bus is what stands between the machine and its own
clock speed, so it is worth more attention than an I2C bug usually gets.

**The measurement, on `#42`:**

```
i2cdetect -y -r 2   ->  68 69 6a 6b     identical on three consecutive scans
i2cget -y 2 0x68 <reg> b                0x00, or intermittent "Read failed"
five identical plain reads of 0x6a      0x0b  0x00  0x00  0x00  0xd6
i2cset -y 2 0x68 0x00 0x82; read back   0x00
```

**The address phase of this bus is perfectly reliable and the data phase is
noise.** That split is specific: in the address phase the master drives SDA and
the slave only pulls it low to acknowledge, and that works every single time; in
a read's data phase the slave drives SDA, and nothing it puts there arrives
intact. Writes cannot be confirmed either — `PAGE_CON` never reads back what was
written, though a broken read is enough to explain that on its own.

**What it settles.** B-43's `Unsupported device id = 0x0`, and the conclusion
drawn from it that there is no DA9214 on this board, are artefacts of a broken
bus. Stock Android on boot1 has the part bound to its own `da9214` driver at
this exact bus and address (B-40's update). The chip is there; we cannot talk
to it. **A negative probe result on a bus that has never carried a working
transaction is not a measurement of the silicon.**

**What was tried and did not work.** `mediatek,use-push-pull`, which the vendor
device tree carries on `i2c@1100e000` and ours lacked. Landed as `dts/0043`
because matching the vendor costs nothing and this bus has one master and one
slave, but the symptom is bit-for-bit the same with and without it. It is not
the cause.

**The two candidates left, both device-free to investigate.**

1. **Pinmux.** We take `i2c6_pins_a` from `mt6797.dtsi` — SDA=GPIO152,
   SCL=GPIO151. The vendor's board DT sets **no pinctrl at all** on this
   controller, so its pins are configured somewhere else and may not be these.
   Note a wrong SDA with a right SCL would look very like this if the real SDA
   still floats to the slave.
2. **Clock.** Our `clock-div = <10>` is the DTSI default applied to every bus;
   the vendor's value for this controller was not captured on the boot1 trip.
   `parent_clk /= clk_src_div` in `i2c-mt65xx.c`, so a wrong divider puts SCL
   somewhere other than 400 kHz — survivable for address decode, not for
   sampling data.

**Why this is worth doing.** The DA9214's two bucks cannot be identified until
the bus works; the CPU regulator cannot be declared until they are; and cpufreq
cannot bind until the regulator exists. Meanwhile the A53s run at 897 and
1274 MHz against maxima of 1547 and 2002 (B-40). **This bus is standing between
the machine and roughly 1.7x on its little cluster.**

## 🟢 B-44 — the display runs behind the M4U (RESOLVED 2026-08-21)

**Opened and closed 2026-08-21.** Three bugs, all of the same kind: a register
address, a register-block configuration and a register bit-field, each inherited
from a different MediaTek SoC by a "modelled on" assumption, each contradicted by
the vendor register map for mt6797.

**Resolved on `#40`**: `iommus` on OVL0 / OVL0-2L / RDMA0, `MMU_EN = 0x7`,
`CTRL_REG = 0x20`, zero `flip_done` timeouts, OVL0 fetching from a mapped IOVA,
vblank IRQ running, and the LXQt desktop photographed correct on the glass.

### The one that took three kernels: MMU_CTRL_REG's bit-fields

`mtk_iommu_hw_init()` picks between two REG_MMU_CTRL_REG constants. mt6797_data
carried `TF_PORT_TO_ADDR_MT8173`, so it got MT8173's:

```
F_MMU_TF_PROT_TO_PROGRAM_ADDR_MT8173	(2 << 5)
F_MMU_PREFETCH_RT_REPLACE_MOD		BIT(4)
```

`drivers/misc/mediatek/m4u/mt6797/m4u_reg.h` says this chip is not laid out that
way:

```
#define F_MMU_CTRL_INT_HANG_EN(en)      F_BIT_VAL(en, 6)
#define F_MMU_CTRL_TF_PROTECT_SEL(en)   F_VAL(en, 5, 4)
```

So MT8173's constant sets **INT_HANG_EN** and leaves TF_PROTECT_SEL at 0, and
the prefetch bit lands in the bottom of the selector. Read back on `#37` and
`#38`: `0x10205110 = 0x50` — TF_PROTECT_SEL = 1, INT_HANG_EN = 1. The vendor's
`m4u_reg_init()` asks for TF_PROTECT_SEL = **2**, INT_HANG_EN = **0**.

That field decides what happens to a transaction that faults: redirected to the
protect address so the master completes, or stopped. Ours stopped it.

**The failure, measured with three prints in one boot (`#38`):**

```
[1.001] disp-ovl0: GEMDBG dma=0xfe000000 domain=dma phys=0x103200000
[1.006] 14020000.larb: SMIDBG MMU_EN <- 0x7
[1.006] mtk-iommu: fault type=0x5 iova=0x7e1e9000 pa=0x0 (larb=0 port=0) read
        ...ten of them, and then nothing, ever again
```

LK leaves DISP_OVL0 scanning out **its own** framebuffer at physical
`0x7dfb0000` — still sitting in `OVL_ADDR` at that moment, because
`mtk_ovl_layer_config()` had not run even once (zero `OVLDBG` lines). The
instant the larb starts translating, that scanout's in-flight reads become
unmapped IOVAs. Note the faulting address **changes every boot** and is not the
buffer DRM allocated: it is wherever inside LK's framebuffer the OVL happened to
be reading. The kernel's own buffer was fine all along — `0xfe000000`, domain
`dma`, mapped to `0x103200000`.

With the fault handling misconfigured the OVL's internal RDMA never got its data
back. `OVL_STA` kept `RUN = 1` with an `RDMA_IDLE` bit clear for the rest of the
boot, `INTSTA` showed `ABNORMAL_SOF` (bit 13, per the vendor's
`ddp_reg_ovl.h`), and no frame-done interrupt was ever raised. mt6797 has no
CMDQ and `shadow_register = false`, so `mtk_crtc_ddp_config()` — the only writer
of the plane registers — runs *solely* from that interrupt. Hence `OVL_SRC_CON`
frozen at 0, no layers, and `flip_done timed out` every ten seconds forever.

**So "zero IOMMU faults" in the first draft of this entry was wrong and is
retracted.** There were faults on `#36` too; nothing printed them because the
larb's MMU_EN was still going to the wrong register then (below), so nothing
reached the IOMMU. Two different bugs produced the same reassuring log, one after
the other.

Fixed in `iommu/0002` by dropping the flag — its only other job, the fault-ID
decode, already falls through to the layout mt6797 uses — and by masking the
field before setting it rather than OR-ing into it.

### Also fixed: mt6797's SMI common is not mt6795's

`memory/0001` pointed the mt6797 smi-common compatible at
`mtk_smi_common_mt6795`. Both fields it carries are wrong for this SoC, and both
only ever reach hardware when the display goes behind the M4U — because that is
the first time this device runtime-resumes smi-common at all. Verified on `#34`
with the desktop up: `0x14022100..0x1402211c`, `0x220`, `0x230`, `0x234`,
`0x238` and `0x300` **all read 0x00000000**.

- `bus_sel`. `smi_configuration.c` (built `-DSMI_EV` for `CONFIG_ARCH_MT6797`)
  writes `0x220 = 0x1554` — larb0 to MMU0, larbs 1..6 to MMU1. mt6795 asks for
  `F_MMU1_LARB(0) = 0x1`, the opposite for larb0. With only larb0 instantiated,
  `0x1554` and the mainline default `0` are the same thing here; `0x1` is the
  one value that is not.
- The init table's offsets. mt6795 writes `SMI_L1_ARB` at `0x200`; on mt6797
  `smi_reg.h` puts `SMI_L1LEN` at `0x100` and `L1ARB0..6` at `0x104..0x11c`, and
  `0x200` is absent from the SoC's smi-common map entirely.

`memory/0004` gives mt6797 its own plat data with neither. The vendor's twelve
real values are transcribed in the comment and deliberately **not** applied:
they are arbitration and FIFO tuning, the display works without them, and
landing twelve unmeasured writes in the kernel that first makes the display
translate would make the result uninterpretable. Start there if bandwidth or
underruns show up.

### And, from the first round: mt6797's SMI_LARB_MMU_EN is at 0xfc0

`memory/0001` added the mt6797 larb by pointing its compatible at
`mtk_smi_larb_mt8173`, "modelled on mt6795". The one field that structure
carries is the register `config_port` writes the per-port MMU enables to, and
mt8173's is **0xf00**. The vendor M4U register map for this exact SoC
(`drivers/misc/mediatek/m4u/mt6797/m4u_reg.h`) says:

```
#define SMI_LARB_MMU_EN  (0xfc0)
#define SMI_LARB_SEC_EN  (0xfc4)
```

0xfc0 is the offset mainline names `MT8167_SMI_LARB_MMU_EN`; the mt8167 and
mt8173 `larb_gen` entries differ in nothing else. Fixed in `memory/0003` with a
dedicated mt6797 entry.

**Verified in hardware, which is the point:**

```
0x14020fc0 (mt6797 SMI_LARB_MMU_EN) = 0x00000007   <- ports 0,1,2 translating
0x14020f00 (mt8173 offset, wrong)   = 0x00000000
```

**The failure this produced is one to remember, because the log lies.** With the
enables going to the wrong register every port stayed in bypass, so once the
display was attached to a domain the OVL got IOVAs and treated them as physical
addresses. The owner saw a garbled panel; the kernel logged a clean bind, zero
IOMMU faults and zero contiguity errors. **A clean dmesg was not evidence of a
working IOMMU — it was evidence that nothing ever reached the IOMMU.**

### What was written here as "not fixed", and what it actually was

The `#36` symptom — `flip_done timed out`, `OVL_SRC_CON = 0x00000000`, CRTC
`enable=1 active=1`, backlight on — was recorded here as possibly "the same
shape as B-17/#20", i.e. an atomic-KMS race that the IOMMU merely widened. It
was not. It was MMU_CTRL_REG, described above, and #20 is untouched by this.
The entry also said "still zero IOMMU faults, so this is not a translation
failure". Both halves were wrong: there were faults, they simply could not be
seen, and it *was* a translation failure — of LK's leftover scanout, not of
anything the kernel allocated.

### Still open, and worth doing: the kernel inherits a live scanout

The handful of faults that remain at boot (3 on `#39`, 10 on `#40` — it depends
on how far into a frame the OVL had got) are LK's scanout being cut off
mid-fetch when MMU_EN goes to 0x7. They are now harmless — the M4U redirects
them and the OVL carries on — but they should not happen. Nothing in the bring-up
stops the bootloader's scanout before the display's ports start translating;
`drm/0012` quiesces the OVL's *interrupt* at probe and leaves its layers running.
Filed as its own follow-up rather than fixed here, because fixing it in the same
kernel as MMU_CTRL_REG would have hidden which one mattered.

### Process failures, both mine

1. **I marked `#35` good on a gate PASS while its display was broken.** The
   panel gate is thirteen register and DCS checks; it says nothing about what
   reaches the glass. It passed 13/13 with a garbled panel, exactly as it
   should have.
2. **The webcam had been re-aimed away from the device**, so `gemini-eyes.py`
   photographed an empty scene and I read a black frame as evidence. The
   project's own rule -- the photograph is the only honest instrument for the
   glass -- is worth nothing when the camera is pointed elsewhere, and nothing
   in the tooling notices. **Check the frame contains the device before
   believing it.** (Re-aimed by the owner 2026-08-21.)

### Where to pick this up

`memory/0003` and `dts/0042` and `drm/0014` are all in the series and correct as
far as they go; only the last of them is unsafe to boot with. The next question
is why the OVL stops signalling frame-done once it is translating -- start by
checking whether the SMI **common** configuration (`mtk_smi_common_mt6795`,
adopted for mt6797 by the same "modelled on" assumption that got the larb offset
wrong) is right for this SoC.

## 🟢 B-43 — the MT6797 M4U probes; and there is NO DA9214 on this board

**Opened and half-resolved 2026-08-21, on kernel `#34`.** Two results from one
flash, one good and one that invalidates a plan.

### The IOMMU works (B-39's structural half)

```
mtk-iommu 10205000.iommu: bound 14020000.larb (ops mtk_smi_larb_component_ops)
/sys/class/iommu/mtk-iommu.0x0000000010205000
```

The M4U registers and binds larb0, and the panel is untouched: **panel gate PASS
13/13**, sddm up, `card0`/`DSI-1` present. Inert exactly as designed, because no
display component has an `iommus` property yet. `#34` is in p1, marked good.

**A probe-ordering fact that Step B will hit.** The boot log reads:

```
[0.837] mediatek-drm: no IOMMU, using contiguous CMA buffers
[0.886] mtk-iommu 10205000.iommu: bound 14020000.larb
```

mediatek-drm binds **50 ms before the IOMMU exists**. Mainline handles this by
returning `-EPROBE_DEFER` from `mtk_drm_bind()` when `iommu_present()` is false;
our `drm/0010` patch turned that into a warning, correctly, for a world with no
M4U at all. **That patch now has to become conditional** -- otherwise adding
`iommus` to the display changes nothing, because DRM will have already bound
without it. Wiring the display without fixing the deferral would look like the
IOMMU "not helping".

### ~~There is no DA9214~~ — RETRACTED 2026-08-21 (evening). There is one.

**This section is wrong and is superseded by B-40's update.** Stock Android on
boot1 has `/sys/bus/i2c/devices/6-0068` named `vproc_buck`, bound to
`bus/i2c/drivers/da9214`, with `mediatek,vproc_buck` as its DT compatible. The
part is on i2c6 at 0x68 exactly as the original claim said.

What the `Unsupported device id = 0x0` below actually showed is that mainline's
`da9211` driver could not read DEVICE_ID through its paged regmap on the first
traffic our kernel had ever put on that bus — i2c6 was enabled for the first
time in `#34`. That is a fact about the access, not about the silicon.

**The lesson here is the reverse of the one this file drew from it.** "Vendor
code for an SoC is not evidence about a particular board built from it" is
true, and it was applied to a case where the vendor code happened to be right —
a negative probe result was allowed to overturn two independent sources without
anyone asking why the probe might be lying. A measurement that disagrees with
the documentation still has to be a *correct* measurement.

The original text follows.

### There is no DA9214, and the A72 regulator plan built on one is void

`echo da9214 0x68 > .../i2c-2/new_device` with the driver loaded:

```
da9211 2-0068: Unsupported device id = 0x0.
```

The driver reads DEVICE_ID through its own paged regmap and gets **0x0**; a
DA9214 returns `0x22`. Raw dumps agree -- every readable register on that
address is zero. `i2cdetect` does show ACKs at 0x68-0x6b, so something is on the
wire, but it is not this part.

**So the earlier claim that "VPROC2 comes from a Dialog DA9214 at 0x68 on bus 6,
and mainline already supports it" is wrong and is retracted.** It came from
`drivers/misc/mediatek/base/power/mt6797/mt_cpufreq.c`, which is MediaTek
*reference-platform* code and does not describe Planet's board. The general
lesson is the one this file keeps relearning: vendor code for an SoC is not
evidence about a particular device built from it.

**What the board actually has, measured:** `i2c7` (`/dev/i2c-3`) carries one
external buck at **0x1c**, bound to `fan53555-regulator` and named `rt5735` --
and it is **VGPU** (enabled, 862.5 mV), not VPROC. The DTS labels that address
`vproc: regulator@1c`, which is simply wrong; the node is `status = "disabled"`
so it has never mattered. The live `vproc` is still `vproc_fixed`, a
`regulator-fixed` asserting a flat 1.000 V with nothing behind it.

**So VPROC and VPROC2 are both unaccounted for on this board.** Whether there is
a second CPU rail at all, or whether all three clusters share one, is now an
open question and the next thing to establish. Do not assume a separate VPROC2
exists.

### Side effect worth knowing: enabling i2c6 renumbered every bus

Linux numbers `/dev/i2c-N` by probe order, not by DT label. Adding i2c6 inserted
it and shifted the rest:

| /dev | controller | was |
|---|---|---|
| i2c-2 | `i2c@1100e000` (i2c6) | *new* |
| i2c-3 | `i2c@11010000` (i2c7, VGPU buck) | i2c-2 |
| i2c-5 | `i2c@11014000` (i2c3, **touchscreen**) | i2c-3 |

Anything with a hard-coded bus number is now pointing at the wrong device -- the
`/root/nvt-*` touchscreen probes in particular.

## 🔴 B-42 — the device cannot charge faster than 500 mA, which is less than it uses

**Opened 2026-08-21, after the battery went flat and the machine would not
boot.** This is why, and it is not "the battery is old".

`bq25890-charger-0` reports `POWER_SUPPLY_INPUT_CURRENT_LIMIT=500000` **on every
supply**, including a proper wall charger. 500 mA is a USB SDP default: nothing
is doing charger-type detection, so the driver never asks for more. A running
desktop on eight A53s with the backlight at 200/255 draws more than that. **The
machine therefore discharges while plugged in**, which is exactly what happened
across a day of hard resets and GPU load.

**Measured, and the fix is one sysfs write** (`input_current_limit` is
writable):

| input limit | cell voltage |
|---|---|
| 500 mA (as found) | **3.204 V** — near cutoff |
| 1.0 A, +20 s | 3.384 V |
| 1.0 A, +45 s | 3.404 V |
| 1.5 A, +70 s | 3.424 V |
| 1.5 A, +95 s | **3.444 V** |

240 mV in a minute and a half, against a supply that had been "charging" for
much longer at 500 mA and losing ground. The charger was always able to deliver
it; the kernel simply never asked.

**CORRECTION 2026-08-21 (later): the "naive fix" was right and my clever one
was dangerous. Two things below were wrong and both were mine.**

*Wrong thing 1: the sign of `current_now`.* The bq25890 driver **negates** it
(`val->intval = ret * -50000;`), so a **negative reading means current flowing
INTO the battery**. `-440000` is charging at 440 mA. Reading it the other way
made a healthy charging device look like it was dying, and produced a confident
claim that it "never gains charge while running" — which the next measurement
immediately contradicted.

*What the corrected reading actually shows,* at a 1.5 A ceiling:

| load | charge current into the cell |
|---|---|
| desktop up, backlight 200 | **440 mA** |
| backlight dimmed to 10 | 590 mA |
| desktop stopped | 660 mA |
| backlight off, no desktop | 670 mA |

So the system draws roughly 0.8-1.1 A, and the machine charges fine once the
input limit exceeds that. Voltage rose 3.784 -> 3.864 V during the test.

*Wrong thing 2: backing the input limit off is unsafe here, and I did it twice.*
The first guard ramped up and stepped back down whenever the cell stopped
gaining, reasoning that asking a weak supply for more than it can give is
dangerous. **It is not** — the bq25890 handles that in hardware: VINDPM and the
input current optimiser reduce the charger's own input current when VBUS sags,
so asking a 500 mA port for 1.5 A yields 500 mA and a slightly droopy rail.

Asking for too **little** is the dangerous direction, because the deficit comes
out of the battery. Stepping the limit down to 500 mA as part of an
internal-resistance measurement **browned the machine out on the spot**, on a
nearly-empty cell — the exact failure this blocker was opened about, walked into
by the person who wrote it down.

*Consequences, now implemented:* the guard sets the ceiling once and leaves it;
there is no ramp, no backoff, and **no measurement is permitted to reduce the
input current**. Which also rules out the obvious way to measure internal
resistance on this device — the load step that would reveal it is the load step
that kills it.

**Why the naive fix is wrong.** Writing 1.5 A unconditionally at boot would be
actively harmful on the lab hub, whose port is a 500 mA source — the owner
plugged the device into USB with a nearly flat battery and it **browned out
instantly**. The limit has to follow what is actually connected. The bq25890
has the hardware for this (D+/D- input-source detection, plus VINDPM and the
input current optimiser, which back off automatically when VBUS sags); none of
it is wired up in our DT/driver path.

**So there are two fixes and they are not the same:**

1. **Short term, userspace:** something that raises the limit when the cell is
   low and backs off if the rail sags. Must not be a fixed 1.5 A.
2. **Right answer, kernel:** wire up charger-type detection so the driver asks
   for what the supply can give. This is the fix; it needs DT/driver work.

**Related, and the reason this was invisible:** there is no fuel gauge at all —
`/sys/class/power_supply/bq25890-charger-0/capacity` is empty, so nothing ever
reports a percentage. The only battery reading available is `voltage_now`, and
nobody was watching it. **Treat 3.3 V as the point to stop working and charge.**

## 🟡 B-41 — touch is rotated twice: the DT rotates it AND X rotates it again

**Opened 2026-08-21**, reported by the owner as "touch is not in the right place
when I use my finger" and pinned in one question: **touching the visible
top-left corner puts the pointer in the top-right.** That is one extra
90-degree rotation, and only one thing produces exactly that.

**Both layers rotate.**

- `mt6797-gemini-pda.dts` gives the touchscreen node **`touchscreen-swapped-x-y`
  and `touchscreen-inverted-y`**, and `gemini-nt36xxx.c` honours both
  (`swap_xy`/`invert_y`, read via `device_property_read_bool`). So the driver
  already emits landscape coordinates matching the rotated X screen.
- `/etc/X11/xorg.conf.d/30-touchscreen.conf` then applied
  `TransformationMatrix "0 -1 1 1 0 0 0 0 1"` on top.

The matrix maps normalised `(x, y)` to `(1-y, x)`. A finger on the visible
top-left makes the driver correctly report `(0, 0)`; the matrix turns that into
`(1, 0)`, i.e. screen x=2160, y=0. Top-right. Exactly the reported symptom.

**The comment was the giveaway.** That file said "the touchscreen reports in the
panel's native portrait coordinates" — a statement the device tree had made
false. Nobody re-read it against the DT because the sentence sounded like a
fact about the hardware.

**Why it survived so long: nothing checks WHERE a touch lands.** "Touch drives
the cursor" was true throughout, and that is the identical shape of mistake as
the Bluetooth mouse that delivered every event perfectly while the drawn arrow
never moved (B-37). The layer everyone looked at was working.

**Fixed for now in userspace**: `30-touchscreen.conf` is identity, and
`scripts/gemini-desktop-setup.sh` carries it plus the coupling warning.

**The real defect is the coupling, and it is not fixed.** The rotation is
expressible in two places and neither knows about the other. Whoever changes
one must change the other in the same commit:

| owner | device tree | Xorg |
|---|---|---|
| kernel (today) | `swapped-x-y` + `inverted-y` | identity |
| X (better end state) | neither property | `0 -1 1 1 0 0 0 0 1` |

**X owning it is the better end state, and this matters beyond tidiness.** A
Wayland compositor applies its own output transform to touch input, so
`output DSI-1 transform 90` under sway will double-rotate against a
kernel that has already rotated — the same bug, in a stack where there is no
xorg.conf to correct it. Moving the rotation out of the DT needs a kernel
rebuild and flash, so it is a follow-up rather than something a userspace
script may do.

**Prediction worth testing when sway is next run:** touch under sway is
currently wrong in exactly this way, and nobody has looked.

## 🟡 B-40 — the two Cortex-A72 cores never come up, and there is no cpufreq at all

### 2026-08-22 (FINAL+6): ATF narrates itself at last, and the PCM *does* release MP2's bus protection

**The ring watcher works.** A kthread on cpu0, started before the SMC, dumping
physical `0x7FF40000` to netconsole every 5 s. For the first time this firmware
has told us what it is doing while it does it:

```
ATF| [ATF](7)[392.043432]big armpll = 26000 Khz, retry = 5942.
ATF| [ATF](7)[409.626879]big armpll = 26000 Khz, retry = 30581.
```

#### Two corrections to FINAL+3, both from ATF's own mouth

* **The frequency retry is UNBOUNDED.** FINAL+3 stated, from the disassembly,
  that "ATF gets exactly two attempts before its own assertion rejects the
  retry PCW". It reached **retry 30581** and was still climbing. Whatever I
  read into the assert ordering at `0x3858` vs `0x3974`, it does not stop the
  loop. Treat `power_on_cl3`'s frequency retry as infinite.
* **ATF measures 26000 kHz, not 749988.** The stage-2 rehearsal that read
  749988 ran with the PCM **not** running. With the PCM running and cluster B
  `CLUSTER_EN` set but `SW_PAUSE`d, the co-processor parks cluster B's clock at
  26 MHz, and ATF can never satisfy either window. Measured alongside:
  `MP2 = 0x00010123` (so `PWR_CLK_DIS` is **clear** — not a gating problem),
  `MUXSEL = 0x54/0x55`, `CKDIV = 8`. The frequency is the PCM's to give.

#### And the finding that matters: the PCM releases MP2's bus protection

FINAL+5 left poll 5 as the wall, with `STA1 & 0x444` immovable — unaffected by
an EN edge and unaffected by clearing `PWR_ISO`/`PWR_CLK_DIS`/`SRAM_CKISO`. With
the PCM running, the watcher caught it moving on its own:

```
tick 2:  STA1 = 0000f444
tick 2:  STA1 = 0000f404      <- bit 6 released
tick 4:  STA1 = 0000f400      <- bit 2 released too
```

**Two of MP2's three protection bits released, with ATF still stuck upstream in
the frequency loop and nowhere near its own bus-release step.** Nothing else was
driving them. That is the PCM doing what the SPMC alone never does, and it is
the first direct evidence that CPUHVFS is load-bearing for this cluster rather
than a notification after the fact.

#### The ring also holds the working reference sequence

The same dump contains BL31's boot-time narration of the A53 core power-ons —
the path that succeeds:

```
... power on CPU5 ...
MEM_PWR_ACK=1
Delay for PWR_ACK / PWR_ACK_2nd / memory power ready
little_spark2_setldo sparkvretcntrl=3f
Little power on:0x1F   then   Little power on:0x3F
mt_on_1, entry 10103c
plat_affinst_on_finish: enable_scu()
plat_affinst_on_finish: plat_cci_enable()
```

`MEM_PWR_ACK` and "Delay for memory power ready" are exactly the memory-power
step MP2 never completes. This is a reference trace for the working case and it
is worth mining before anything else is guessed at.

#### Next

Unpause cluster B — `cspm-probe stage 4` with seeds taken from the live rails
(`v_*` is the DA9214 register code verbatim: BUCKA read `0x4a`, so
`v_ll=v_l=v_cci=0x4a`) — so the PCM gives cluster B a real frequency, and only
then `a72-psci stage 3 skip_cputop=1`. If 26 MHz becomes ~750 MHz, ATF leaves
the retry loop and arrives at a bus-release that is already two thirds done.

**Lab:** the run was left to the dead-man rather than rescued by hand, on
purpose. The fix was one register write away, but reaching it needs an ssh
login, and a login while an SMC is outstanding is what cost the device earlier
today. The intervention belongs in the watcher kthread, not in a shell.

---

### 2026-08-22 (FINAL+5): the blocking poll is NAMED — and it closes the loop back to CPUHVFS

**`tools/a72-psci` stage 5 works, and it identifies the exact poll ATF hangs
on.** On a boot where everything upstream succeeds:

```
SPMC ack:            CPU_PWR_STATUS bit17 SET after 0 us   0x102222a0 00400100 -> 00ba0100
ATF frequency check: abist(37) = 749988 kHz   pass1[742500..757500] *** PASS ***
power_on_big polls:  bit7 SET after 0 us; core SPMC 0x10222430 bit17 SET after 0 us
                     VERDICT: PSCI CPU_ON will NOT spin in power_on_big
poll 5:              STA1 & 0x444  *** TIMEOUT — ATF WOULD SPIN HERE ***  (=0000f444)
```

So of ATF's eight unbounded polls, seven are satisfiable and **poll 5 is the
wall**: `INFRACFG 0x1000123c & 0x444` never clears.

#### The bits are MP2's, confirmed out of ATF itself

ATF releases `0x911` (bits 0,4,8,11) for another cluster in the same register,
and `0x444` is bits 2,6,10 — the third of three striped cluster groups. MP0's
and MP1's bits are **absent** from `STA1` because those clusters run; MP2's are
present. `0x444` is MP2's TOPAXI bus protection.

#### It is not an edge problem, and it is not ISO/CLK

Two experiments, both with a dead-man reset armed:

* **Edge.** `PROTECTEN` (`0x10001234`) reads `0x00000000` while `STA1` reports
  MP2 engaged, so ATF's `&= ~0x444` is a no-op with no transition to make —
  the same shape as the `SRAM_PDN` edge above. Engaging `0x444` and releasing
  it again gives it a real edge: `EN 0 -> 0x444 -> 0`, and **`STA1` stays
  `0x0000F444` through 40 polls.** Not an edge problem.
* **Isolation and clock.** With the domain coarsely powered, clearing
  `PWR_ISO`, then `PWR_CLK_DIS`, then `SRAM_CKISO` by hand walks MP2 from
  `0x00010137` to `0x00010105` — and **`STA1` does not move at any step.**

#### What that leaves, and why it is the same wall as always

MP2 at `0x00010105` against a running MP0 at `0x0009004d` is still missing
`PWR_ON_2ND`, `SRAM_ISOINT_B`, and still has **`SRAM_PDN` set**. The SRAM is
down. A cluster whose SRAM is down has an idle bus by construction, so its
protection status has nothing to release — which is consistent with MP0/MP1's
bits being clear precisely because those clusters are alive.

So the chain is: **bus protection cannot release until MP2 is genuinely
sequenced; MP2 is not sequenced because its SRAM never comes up; and the SRAM
sequencing on this cluster is the SPMC's job, which is what CPUHVFS/the PCM was
hypothesised to drive.** The SPMC acks the coarse power switch and does nothing
further.

**How to apply.** The next experiment is the combination nothing has run:
`tools/cspm-probe` stages 1-3 (documented safe — the PCM loads, verifies and
kicks with every cluster PAUSED, and the A53s and i2c6 are undisturbed), and
*then* `a72-psci` stage 5. The question it answers is narrow and falsifiable:
with the PCM running, does the SPMC complete MP2's sequence — `SRAM_PDN`
clearing and `STA1 & 0x444` dropping — where it currently stops at the coarse
switch? Note FINAL+2 recorded that with the PCM running the manual `PWR_ON` is
refused, so the order matters: PCM first, then the ATF-side power-on.

---

### 2026-08-22 (FINAL+4): the SPMC ack is NOT reliable at 1180 mV, and a failure latches for the whole boot

**Retracting a claim I made in FINAL+3 and inherited from the handoff:**
*"The SPMC ack is no longer intermittent. At `vproc2_mv=1180` it asserted in
0 us on the first attempt, every run."* That was three runs inside **one boot**.
On the next boot, with VPROC2 verified at 1180 mV by reading the chip
(`i2cget -f -y 2 0x68 0xd9` = `0xd8`, and `BUCKB_CONT` = `0x01`), the ack never
came:

```
attempt 1..8: CPU_PWR_STATUS bit17 TIMEOUT after 20000 us
              MP2=00010137  MCUCFG 0x102222a0=0e500100
the SPMC never acked
```

Eight attempts, each with ATF's own escape between them (drop `PWR_ON`, wait
for the domain down, cycle `B_EXT_BUCK_ISO` 100 us / 240 us). A second module
load, three minutes later, with the rail still up, reproduced it exactly.

**So the ack is a per-boot property, and a failure latches.** B-40 recorded the
latched value as `0x042001xx`; this is a third family, `0x0e500100`, with bit 17
clear and the whole `0x102224xx` PLL/iDVFS sub-block reading `0x00000000`. Two
different latched states now, neither clearable without a reboot — so **there is
exactly one attempt per boot**, and any experiment that costs an attempt has to
be worth a reboot.

**What differed between the boot that acked and the one that did not** is not
the voltage. The successful boot never did prerequisites-and-power-on in one
module load; it ran prerequisites alone (stage 1), left the rail up, and powered
on **2.5 minutes later** from a second load. The failing boot collapsed that into
one load with a 45 s settle. Two other things moved with it and are not yet
separated: CONSYS/Wi-Fi came up *during* the failing run's settle window
(`gemini-net-up` fires its func-on at ~120 s uptime), and BUCKA sat at 1040 mV
rather than the 1100 mV of the successful boot.

**How to apply.** Do not spend an attempt on a machine that is still booting.
Wait for `uptime >= 240 s` so the CONSYS bring-up is finished, then run the
prerequisites and the power-on as two separate loads with a gap. And read
`0x102222a0` *at rest*, before any `PWR_ON` — `a72-psci` step 6 does this now,
and nothing had ever recorded it, so "the SoC came up in a bad state" has never
been distinguished from "our power-on put it there".

---

### 2026-08-22 (FINAL+3): the frequency gate is not a wall — it is ARMCAXPLL2, and it moves

**Two of this blocker's load-bearing claims are wrong, and the one that mattered
most was an instrument fault.**

B-40 recorded, as settled: *"ATF's `CPU_ON` is unsafe by measurement: abist
source 37 is pinned at `629992 kHz` alongside seven other sources, so it is not
wired to the A72 clock here; 629992 satisfies neither the 742500±15000 nor the
495000±10000 window, so `power_on_cl3` would retry forever."* That sentence is
the only reason PSCI was ruled out and the CPUHVFS port was named as the last
remaining route.

#### Fault 1: the meter returns the previous measurement

`CLK26CALI_1` (`0x10000224`) latches some time **after** the trigger bit clears,
so a single-shot read returns the *previous* run's count. Every "source N" in
that sweep of 32–47 was really source N−1. Measure twice and keep the second
and the sweep is a different sweep.

#### Fault 2: a passive sweep cannot tell "unwired" from "genuinely 630 MHz"

The causal test can. With cluster B unpowered, move `ARMCAXPLL2`
(MCUMIXED `0x1001a224`) and watch source 37:

```
ARMCAXPLL2 = 630 MHz   ->  src36 = 629992   src37 = 629992
ARMCAXPLL2 = 500 MHz   ->  src36 = 499992   src37 = 499992
ARMCAXPLL2 = 750 MHz   ->  src36 = 749988   src37 = 749988
```

**Source 37 is the A72 cluster clock. It follows ARMCAXPLL2 one for one.** It
was never pinned; nobody had ever moved it. The other "pinned" sources are
unconnected inputs whose counter simply never updates — which is exactly what
made them read the same stale number as their neighbour under fault 1.

`ARMCAXPLL2_CON1` reads `0xc10c1d89`, and the vendor's own `_cpu_freq_calc`
arithmetic on it gives **629.99 MHz** — the value the meter reports, to 0.007%.
Two independent instruments agreeing was available all along.

#### With that fixed, every precondition ATF checks is satisfiable

`tools/a72-psci`, stage 2, on the live machine:

```
prerequisites (vendor cpu_power_on_buck): VPROC2 1180 mV, EXT_BUCK_ISO clear
ARMCAXPLL2 -> 750000 kHz (pcw 0x0e6c4e)
CPUTOP power-on: CPU_PWR_STATUS bit17 SET after 0 us   (first attempt, no retry)
                 MCUCFG 0x102222a0 bit17 SET after 0 us  (= 0x00ba0120)
ATF's five assertions:
  0x102224a0 = 00ff1100  want 00ff1100  ok
  0x102224a4 = b9b13b14  want b9b13b14  ok
  0x102224ac = 01b10100  want 01b10100  ok
  0x102224b0 = 00af00af  want 00af00af  ok
  0x102224b4 = 00000010  want 00000010  ok
rehearsing ATF's clock section (PLL 3-step, MUXSEL |= 1, CKDIV = 8):
  abist at ATF's decision point: BIG(37) = 749988 kHz
  ATF's check on 749988: pass1[742500..757500] *** PASS ***
```

So `power_on_cl3` **can** get past its frequency loop. The route back is PSCI,
not a CPUHVFS port.

#### Three more things this run settled

* **The `0x102224xx` block is dark until the cluster domain is powered.** With
  VPROC2 up *and* `CPU_EXT_BUCK_ISO` clear it still reads all-zero, while its
  neighbour `0x102222b0` returns real values through the same secure accessor.
  It wakes only after `PWR_ON` + the SPMC ack. So ATF's five assertions can only
  be evaluated where ATF evaluates them — on the far side of its two power-good
  polls. (The handoff's "VPROC2 on **and** ISO cleared is enough" is too weak.)
* **The SPMC ack is no longer intermittent.** At `vproc2_mv=1180` it asserted in
  **0 µs on the first attempt**, three runs out of three, with none of ATF's
  isolation-cycling retries needed.
* **ATF gets exactly two attempts at the frequency check, not infinitely many.**
  The retry path writes `0xA6800000` (~500 MHz) over `0x102224a4` — and the
  *next* iteration's own assertion demands `0xB9B13B14` there. So iteration 3
  asserts rather than spins. The unbounded loops in `power_on_cl3` are its two
  power-good polls and its CSPM semaphore poll, not the frequency retry.

#### The failure mode of a spinning SMC, and the instrument it needs

`stage 3` fires raw PSCI `CPU_ON` from a thread pinned to cpu7, deliberately
*not* through `cpu_up()`, so that a spin at EL3 costs one CPU instead of the
machine. It did spin. And the machine still went unusable within ~3 minutes,
for a reason worth recording:

```
watchdog: BUG: soft lockup - CPU#1 stuck for 48s! [sshd-auth:6605]
Call trace:
 smp_call_function_many_cond+0x158/0x3d0
 kick_all_cpus_sync+0x44/0x70
 bpf_int_jit_compile+0x1dc/0x598
 bpf_prog_create_from_user+0x10c/0x1ac
 do_seccomp+0x134/0xac8
```

**Every new ssh login installs a seccomp filter, which JITs, which calls
`kick_all_cpus_sync()`, which IPIs the CPU stuck at EL3.** So "one CPU is lost
but the machine lives" is true for about as long as it takes to try to log in —
and logging in is exactly what you want to do next. A reader started *after*
the hang can never run.

The fix is in `tools/a72-psci`: a `kthread` bound to cpu0, started **before**
the SMC, that dumps ATF's log ring (physical `0x7FF40000`) to the kernel log
every 5 s. Stage 3 never returns from `module_init` when ATF spins, so the
module is never freed and the thread keeps its code. The ring is confirmed
readable and live — its header is
`base=0x7ff40100 size=0x00029f00 wptr=0x7ff41a05` and it holds BL31's own boot
narration in plain text.

Recovery was the documented one and it did **not** work — see the correction
at the end of this entry.

#### Every unbounded poll in ATF's path, and which are now measured

All read out of `tee.img`: `power_on_cl3` at file offset `0x371c`,
`power_on_big` at `0x46c4`. These are the only places a `CPU_ON` can hang.

| # | poll | status |
|---|---|---|
| 1 | `CPU_PWR_STATUS` bit 17 (SPM `0x10006188`) | pre-satisfied by stage 2, acks in 0 µs |
| 2 | MCUCFG `0x102222a0` bit 17 | pre-satisfied, 0 µs |
| 3 | CSPM semaphore `0x11015448` bit 0 | stages 3/5 report it and free it if held |
| 4 | the frequency retry | opened by `armpll2_khz=750000` |
| 5 | INFRACFG `0x1000123c & 0x444 == 0` after `0x10001234 &= ~0x444` | stage 5 rehearses it |
| 6 | CCI `0x1039000c` bit 0 after enabling snoop/DVM on slave interface 5 (`0x10396000`) | stage 5 with `rehearse_cci=1` |
| 7 | `CPU_PWR_STATUS` bit (15−cpu), in `power_on_big` | stage 5 rehearses it |
| 8 | core SPMC `0x10222430 + idx*4` bit 17 | stage 5 rehearses it |

**5 and 6 are the leading suspects**, precisely because they sit *after* the
gate that was shut until ARMCAXPLL2 was moved — nothing had ever reached them.

Two more facts from the same reading:

* **ATF's assert handler (`0xf104`) prints and then spins**: `bl 0xee88` (its
  printf) → `bl 0x10720` → `b .`. So even a failed assertion is an unbounded
  spin — but it *narrates itself into the log ring first*, which is what makes
  the ring watcher worth having.
* **ATF writes its OWN warm-boot entry to `0x10222290 + idx*8`** — `str w1,[x2]`
  with `x1` from a BL31 global (the SRAM trampoline `0x0010103c`), **not** the
  entry passed to `CPU_ON`. Every non-PSCI attempt in this blocker released a
  just-reset A72 at a DRAM address with MMU and caches off and the cluster
  outside the coherency domain. That is a much better explanation of "the core
  executes nothing" than anything tried so far, and it means PSCI may be the
  only route that can work.
* **`power_on_big` bails on a pre-powered core.** It reads `MP2_CPUn_PWR_CON`
  first and, with `PWR_ON` set, prints *"The required Big core:%d was powered
  on"* and returns — no boot address, no reset release. Pre-powering the
  **core** is a no-op; pre-powering the **cluster** is fine and is what removes
  polls 1 and 2.

#### Correction: `uhubctl` did NOT recover this, and the board is down

The handoff's *"hangs recover with no human, reliably"* holds for a hung
**kernel** — the RGU stops being petted and resets the board, and the port
cycle is only there to keep the preloader from parking. It does not hold here.
The kernel was alive on its remaining CPUs and systemd went on petting, so
there was no reset to ride; the hub does not switch VBUS (B-27), so a port
cycle is a data replug; and the device has a battery, so nothing the host can
do removes its power. Ten-minute and twenty-minute holds, and eight replug
cycles, left it enumerated on USB with carrier up and answering nothing — no
ssh, no ping, no ARP, no netconsole, and not on the Wi-Fi LAN either. **It
needs the power button.**

---

### 2026-08-22 (FINAL+2): the unpause is fixed, and the PCM owns MP2's sequencer

**The instant death was the seeding, and it is fixed.** Writing the real
current state instead of `V_CURR(0)|VS_CURR(0)` makes the unpause survivable:

```
LL  hwsta = 0x000f4d00  (opp 15 -> fw 0, vproc 0x4d, vsram 0x0f)
B   hwsta = 0x000b5800  (opp 15 -> fw 0, vproc 0x58, vsram 0x0b)
...
+300 ms  MP2=0x00010133  B=0x000050f0  FSM=0x00648640
final MP2_CPUSYS_PWR_CON = 0x00010133
```

Cluster B unpaused (`SW_PAUSE` clear, `CLUSTER_EN` set), firmware cycling, and
**the machine lives**. The encodings were checked against live registers:
`VOLT_TO_EXTBUCK_VAL` reproduces the DA9214 code we read, `VOLT_TO_PMIC_VAL`
reproduces the on-die LDO vosel. `cspm-probe` now takes them as module params
(`v_ll/v_l/v_b/v_cci`, `vs_*`, `f_*`) so they can be computed from the live
rails and eyeballed before the co-processor is ever let go.

**But unpausing does not power the cluster** — MP2 stays at `0x00010133`. The
PCM participates in cluster power-on; it does not initiate it.

#### And once the PCM runs, it owns the SPMC

With the PCM running — **paused or unpaused, it makes no difference** — our
manual `PWR_ON` is refused outright. `0x102222a0` reads `0x04200120` from the
very first attempt and bit 17 never comes, through all 8 of ATF's
isolation-cycling retries. Without the PCM the same sequence acks first try and
takes the cluster to `0x0001004d`.

So the two halves are mutually exclusive as currently driven:

| | SPMC ack | SRAM/ISO sequenced | cores fetch |
|---|---|---|---|
| PCM dead, manual/ATF sequence | **yes** | no | no |
| PCM running, manual sequence | **no** | — | no |

That is not a contradiction, it is the interlock: on Android the kernel drives
`cpu_power_on_buck()` → PSCI `CPU_ON` → ATF, and CPUHVFS is notified through
`cpuhvfs_notify_cluster_on()` as part of the hotplug callbacks. Poking
`CLUSTER_EN` in `SW_RSV2` is not that protocol.

#### PSCI CPU_ON with the PCM running: still hangs

Tried, because it is exactly Android's arrangement and the earlier "unsafe"
call had been made with the PCM dead — which is the variable that ought to make
ATF's frequency check pass. Prerequisites in place, PCM running with cluster B
enabled and paused, `0x102222a0 = 0x00480100` (the good family), then
`echo 1 > cpu8/online`. **The machine hung**, consistent with ATF's unbounded
frequency-retry loop; the ATF ring is zeroed on the next boot so its own
narration of the spin was not recoverable.

#### The recovery is hands-free and reliable

Every hang and every download-mode park this session was recovered with **no
human**:

    03-tools/uhubctl/uhubctl -l 1-10 -p 1 -a off
    # hold down 60-90 s, across the watchdog reset
    03-tools/uhubctl/uhubctl -l 1-10 -p 1 -a on

Back in ~55 s with 8 CPUs, every time. `gemini-state.py` still claims
DOWNLOAD_MODE "needs hands"; it does not. (This does **not** rescue a board
that is actually powered off — nothing on the port at all is still the button.)

#### What is left

Port the real cluster-on protocol out of `mt_cpufreq_hybrid.c` —
`cpuhvfs_notify_cluster_on()` / `cpuhvfs_notify_cluster_off()` and the
semaphore interlock — and call it from the CPU hotplug path around PSCI
`CPU_ON`, rather than writing `CLUSTER_EN` by hand. The i2c6 semaphore
(`cspm_get_semaphore(SEMA_I2C_DRV)` = pause the PCM around every transfer on
the `mediatek,appm_used` bus) has to go into `i2c-mt65xx` at the same time, or
the CPU regulator and the co-processor will collide.

---

### 2026-08-22 (FINAL+1): CPUHVFS RUNS. And unpausing it takes the machine down instantly.

**The DVFS co-processor is running on Linux on this device for the first time.**
`tools/cspm-probe` was already a complete staged port from an earlier session;
it was written, and then set aside on the wrong conclusion that CSPM was not
the blocker. It is, and it works:

```
stage 1: IM fetch done after 749 us
         IM VERIFY OK: all 2025 words read back correctly
stage 2: after kick: FSM_STA=0x00650640 PCM_TIMER=0x0000092c
         PCM_TIMER advancing 0x1175D -> 0x19865
```

`FSM_STA = 0x00650640` is **exactly Android's value**, and the PCM timer runs.
The firmware is `pcm_dvfs_v0.1_160131_02`, 2025 words — the variant this
blocker's earlier correction identified as the shipped one.

**And the A53s survive it.** With every cluster kicked PAUSED, cpufreq still
transitions (897000 -> 1014000 on request) and i2c6 is intact. So the
co-processor can be loaded and started without disturbing the running system.

#### Stage 3 is fine. Stage 4 is not.

`stage=3` sets `CLUSTER_EN` on cluster B (`SW_RSV2` 0x30f0 -> 0x70f0) and
nothing moves, correctly — the cluster is still `SW_PAUSE`d, and a paused
cluster is one the firmware will not act on.

`stage=4` clears the pause. **The machine dies instantly, three times out of
three.** netconsole's last line is cut off mid-`printk`:

```
cspm-probe: about to CLEAR SW_PAUSE on cluster B only. LL/L stay paused.
cspm-probe:   LL=0x000030f0 L=0x000030f0 B=0x000070f0
cspm-probe:   <cut>
```

No oops, no call trace, no watchdog delay — the SoC stops between two printk
calls. That is a rail event, not a software hang, and it is consistent with
the co-processor taking over the CPU rails over i2c6 the moment it is unpaused.

#### Why, and what has to be right before trying again

Two things in the kick are wrong, and the second is the dangerous one.

1. `writel(0, csram + OFFS_PAUSE_SRC)` clears the **global** pause source
   (`PSF_PAUSE_INIT`), not just cluster B's. The vendor sets
   `pause_src_map |= PSF_PAUSE_INIT` and clears bits individually.

2. **The current-state words are seeded with a lie.** The probe writes
   `hwsta_reg[i] = F_CURR(opp_sw_to_fw(NUM_CPU_OPP-1)) | V_CURR(0) | VS_CURR(0)`
   — telling the firmware every cluster is **at 0 V**. The vendor writes the
   real values:

   ```c
   cspm_write(hwsta_reg[i], F_CURR(opp_sw_to_fw(sta->opp[i])) |
                            V_CURR(sta->volt[i]) | VS_CURR(sta->vsram[i]));
   ```

   An unpaused firmware that believes the current voltage is 0 will drive the
   PMIC to "correct" it. `VPROC` is BUCKA — **LL, L and CCI, the clusters this
   kernel is running on**.

   The index direction is *not* the bug, checked: `opp_sw_to_fw(i) = 15 - i`
   with software 0 = highest and firmware 0 = lowest, so the probe's
   `SW_F_DES(opp_sw_to_fw(NUM_CPU_OPP-1))` is the lowest frequency, not the
   highest.

**Do not just retry stage 4.** The failure mode is a wrong voltage on the live
CPU rail, written by a co-processor, straight to the DA9214 — around the
kernel's regulator and its DT limits. Under-volting resets the box (seen);
over-volting is a hardware risk that no amount of watchdog will undo. The next
attempt needs `sta->opp/volt/vsram` seeded from what the rails actually are, a
per-cluster pause map, and the i2c6 semaphore
(`cpuhvfs_get_dvfsp_semaphore(SEMA_I2C_DRV)`) wired into `i2c-mt65xx` first.

#### Lab: download mode has a software escape after all

Two of the three stage-4 wedges came back **parked in download mode**
(`0e8d:2000` preloader, then `0e8d:0003` BROM) — B-27's trap. `gemini-state.py`
says this "needs hands". **It does not, at least not always:**
`uhubctl -l 1-10 -p 1 -a off`, hold it down ~60-90 s across the reset, then
`-a on`, and the device boots normally — the preloader never sees a host, so it
never parks. That is worth having in the tooling.

---

### 2026-08-22 (FINAL): the A72 blocker is CPUHVFS, and this time it is measured

**CSPM — the DVFS co-processor — is not running on our system at all.** Read
directly, side by side with Android:

| register | ours | Android |
|---|---|---|
| `PCM_TIMER_OUT` (0x11015150) | `0x00000000`, **not advancing** | `0x003637fc` |
| `PCM_REG15` (0x1101513c) | `0x00000000` | `0x00000182` |
| `PCM_FSM_STA` (0x11015178) | `0x00048490` | `0x00650640` |
| `SW_RSV0..6` (0x11015608+) | all `0xBABEBABE` | `0x5ff0 0x2ff0 0x26f0 ...` |

`0xBABEBABE` is the reset pattern — those words have never been written. **This
directly contradicts the earlier entry's "CSPM runs correctly (`FSM_STA
0x650640`, bit-identical to Android)".** It does not; ours is `0x00048490` and
its timer is dead. Android's values came from
`/sys/kernel/debug/cpuhvfs/dvfsp_reg` on a boot1 trip, ours from `devmem`.

**Why that is the blocker, and why the ATF reading above was only half the
story.** ATF's `power_on_cl3`/`power_on_big` really do only assert `PWR_ON` and
poll — and that part *works*: with the prerequisites right, `CPU_PWR_STATUS`
bit 17 asserts, `0x102222a0` goes to `0x00ba01xx`, the whole `0x102224xx` block
comes alive, and all five of ATF's assertions match. But that is the **coarse**
power switch only. Everything after it — clearing `PWR_ISO`, `PWR_CLK_DIS`,
`SRAM_CKISO`, `SRAM_PDN`, and sequencing the SRAM — is the **PCM firmware's**
job on this cluster, which is what Android's
`[CPUHVFS] cluster2 on, swctrl = 0x25f0` is. With ATF's exact sequence and
nothing else, `MP2_CPU0_PWR_CON` sits at `0x00010037`: powered, but still
isolated, clock-disabled and SRAM down. A running A53 core reads `0x0001004d`.

So **B-40's original CPUHVFS hypothesis was right**, and this session's
"refutation" of it was drawn from ATF's code alone, which only ever shows the
coarse half.

#### What is now proven, and what it cost to prove

* **The SRAM ack instrument is sound.** Validated on a known-good domain: with
  cpu7 offlined, driving `MP1_CPU3_PWR_CON` SRAM_PDN 1->0 clears
  `SRAM_PDN_ACK` in 0 polls and 0->1 sets it in 0 polls. On MP2 the same edge,
  with the cluster powered and the SPMC acking, never produces an ack — at any
  SRAM LDO voltage from 500 to 1200 mV, and with every single bit of
  `0x102222b0[31:12]` flipped one at a time. **The SRAM LDO is not the gate.**
* **VPROC2's voltage matters.** At the 1000 mV LK leaves, the SPMC ack is
  *intermittent* — same inputs, different outcome, and a failure latches
  (`0x102222a0` sticks at `0x042001xx` until a reboot; ATF's own retry, which
  cycles `B_EXT_BUCK_ISO`, does not clear it). At **1180 mV it acked first try,
  every time.** `vproc2_mv=` in `tools/a72-bringup` sets it.
* **The rail needs to settle.** ~45 s between enabling VPROC2 and asserting
  `PWR_ON` was the difference between acking and not, before the voltage fix.
* **`0x10222700` (big spark / "sparkvretcntrl", the SRAM retention control)
  reads 0 from cold and only accepts a write once the cluster is powered.**
  Earlier runs saw `0x3f` there and concluded "already set" — only because a
  previous run in the same boot had written it. It is now written as soon as
  the write can land.
* **Do not run the A53 MTCMOS tables on MP2.** ATF never touches MP2's
  ISO/CLK/SRAM bits; on this cluster they belong to the SPMC, and driving them
  by hand fights the sequencer. `core_mode=0` (the default) is ATF's sequence;
  `core_mode=1` is the A53 table, kept only for comparison.
* **ATF's `CPU_ON` is still unsafe**, and now by measurement rather than
  suspicion: its frequency check reads abist source 37, and a full sweep of
  sources 32-47 (with ATF's own `CLK_DBG_CFG` mask) shows 37 pinned at
  `629992 kHz` along with 32/33/36/38/39/40/44 — it is not wired to the A72
  clock here. 629992 satisfies neither the 742500+-15000 nor the 495000+-10000
  window, so `power_on_cl3` would retry forever.

#### The next step is the CPUHVFS port, and it is the last one

`mt_cpufreq_hybrid.c` (2512 lines) plus `mt_cpufreq_hybrid_fw.h`
(`dvfs_binary[]`) in `07-kernel/ubports-3.18/.../mt6797/`. The minimum is: map
CSPM (0x11015000) and CSRAM (0x0012a000, 12K), reset the PCM, load the firmware
image, kick it (`__cspm_kick_im_to_fetch()` hands the IM a **physical** address,
so placement and EMI MPU permissions are part of the job), then drive the
cluster-2 swctrl word.

**The hazard has not changed and is now the main risk:** the vendor's i2c driver
takes `cpuhvfs_get_dvfsp_semaphore(SEMA_I2C_DRV)` before every transfer on the
`mediatek,appm_used` bus because the co-processor masters it too, and mainline's
`i2c-mt65xx` knows nothing about that. i2c6 carries the CPU regulator and is in
the path of **every** A53 DVFS transition since `#51`. Plan the semaphore before
starting CSPM, not after.

---

### 2026-08-22 (latest): ATF's real MTCMOS tables, and a self-inflicted stop

Two findings and one mistake, all from continuing the session above.

#### ATF's power-on is table-driven, and it is not the sequence we copied

`power_on_little_cl()` and `power_on_little()` in `plat/mt6797/power.c` are
lists of single-bit writes to one PWR_CON register, with polls and delays hung
off particular step indices. Both tables are now extracted from the running
firmware -- the cluster one is built inline on the stack at 0x3f74, the core
one is a 2x14-word rodata table at file offset **0x123b0**. (Note: the
adrp+add rodata correction for *this* build is **+0xA40**; the -0x5C0 recorded
below is the older `trustzone.bin`'s constant. Solve it per binary.)

**Cluster** (`power_on_little_cl`, on `SPM+0x210 + cluster*4`):

```
PWR_RST_B=0 ; PWR_CLK_DIS=1
PWR_ON=1        udelay(1)
PWR_ON_2ND=1    wait PWR_ON && PWR_ON_2ND read back ; udelay(100)
PWR_ISO=0
SRAM_PDN=0      wait SRAM_PDN_ACK == 0 ; udelay(500)
SRAM_ISOINT_B=1 udelay(1)
SRAM_CKISO=0 ; PWR_CLK_DIS=0 ; PWR_RST_B=1
```

**Core** (`power_on_little`, on `SPM+0x220 + cpu*4`):

```
SRAM_SLEEP_B=0 ; PWR_RST_B=0 ; PWR_CLK_DIS=1
SRAM_PDN=1      wait SRAM_PDN_ACK == 1     <-- the SRAM is cycled DOWN first
PD_SLPB_CLAMP=0
SRAM_PDN=0      wait SRAM_PDN_ACK == 0
PWR_ON=1        udelay(1); wait CPU_PWR_STATUS bit(15-cpu)
PWR_ON_2ND=1    udelay(1); wait CPU_PWR_STATUS_2ND bit(15-cpu)
SRAM_SLEEP_B=1 ; SRAM_ISOINT_B=1 ; PWR_ISO=0 ; udelay(1)
SRAM_CKISO=0 ; PWR_CLK_DIS=0 ; PWR_RST_B=1
```

then `little_spark2_setldo(cpu)` and a per-core enable bit.

Two things this changes:

1. **The core sequence drives `SRAM_PDN` high first and waits for the ack to
   become 1**, then low and waits for 0. This port has been going straight to
   0, which gives the SRAM controller no edge -- so every "SRAM_PDN_ACK
   cleared after 0 us" this blocker has recorded measured nothing.
2. **The cluster sequence never touches `SRAM_SLEEP_B` at all.** The missing
   bit 19 that the section below calls a remaining anomaly is not part of
   powering a cluster on. MP0 has it set because whoever brought MP0 up did
   something else. **That lead is dead too.**

#### Do not assert PWR_RST_B on MP2

Measured: running the cluster table's step 0 (`PWR_RST_B = 0`) on MP2
**permanently kills the 0x102224xx sub-block** -- the big cluster's PLL and
iDVFS registers read `0x00000000` afterwards and stop accepting writes, and
they do not come back when reset is released. `BIGIDVFSENABLE` still returns 0
into the void. The little clusters survive it, so whatever ATF re-initialises
for them lives somewhere we have not found.

`cpu_power_on_buck()` has already taken MP2 out of reset, so
`tools/a72-bringup`'s `atf_cluster_on()` starts at step 2 and never re-asserts
it. **Recovering from this needs a reboot.**

#### The MP2 MISC block is a different register map, so the boot address is right

MP0's is at 0x10220000, MP1's at 0x10220200, MP2's at 0x10222200 (ATF's own
`enable_scu()` switch picks 0x1022002c / 0x1022022c / 0x1022222c). But the
blocks are not parallel: MP0/MP1 carry `0a0a0a0a`-style A53 config words at
+0x04..+0x28 that MP2 does not, and MP2's +0x20..+0x3c reads `0xFFFFFFFF` and
**refuses a distinctive write**. Those offsets are simply unimplemented for an
A72 cluster. So `0x10222238` is *not* an A72 boot-address register despite
MP0's +0x038 being one, and ATF's `0x10222290 + idx*8` is correct.

#### The mistake: `reboot -f` cost the device

To get a clean state after the PWR_RST_B damage I ran `reboot -f`. The device
has not enumerated since. This is a **known, documented trap** and the warning
is in this repo, at the top of
`release/headless-overlay/usr/local/sbin/gemini-reboot`:

> `systemctl reboot` does not [come back]. Measured repeatedly on 2026-08-20:
> every attempt left the machine indistinguishable from dead -- no USB, no
> network, dark panel -- **recoverable only with the power button.**

`gemini-reboot` exists precisely because of this: it syncs, remounts read-only,
sets `WDT_MODE = 0x22000011` (enabled, **EXRST_EN clear**, because that bit
routes the reset to the PMIC which powers the board off instead of restarting
it) and then hits `WDT_SWRST`.

`gemini-state.py` says `STATE: ABSENT`. `uhubctl` cannot help: the hub
advertises per-port power switching but **does not switch VBUS** (B-27), so a
port cycle is a data replug, not a power cycle.

**RULE: on this device the only reboot is `gemini-reboot`.** Never `reboot`,
`reboot -f`, `systemctl reboot`, or `shutdown -r`.

---

### 2026-08-22 (late): the ATF was disassembled, and it moves the blocker

**The two A72 cores are still not online.** But four of the previous entry's
load-bearing conclusions are now refuted by reading the firmware itself, and
the cluster gets much further than it ever has: MP2 is powered, clocked,
its MCUCFG block is alive, iDVFS is enabled, and `MP2_CPU0_PWR_CON` reaches
`0x0001004d` — bit-identical to a running A53 core. The cores still do not
fetch. The remaining gap is narrow and different.

Tool: `tools/a72-bringup/` (stages 0–6; stage 1 writes nothing).
Captures: `04-docs/captures/a72-bringup-2026-08-22/`.

#### The firmware is readable, and it is not the one we were reasoning about

`02-firmware/flash-set/tee.img` **is** the on-device ATF — its 98304 bytes are
byte-identical to the head of `01-backups/tee1.bin`, and the banner matches
what the running firmware prints: `BL3-1: v1.0(debug):7f8e0c2`, built
2019-05-08. It is a **debug build**, so it carries assert text and INFO format
strings. `02-firmware/flash-set/trustzone.bin` is a *different, older* build
(`fa2508c`, 2018-10-31) and is **not** what runs — do not reverse that one.

Disassembly (`aarch64-linux-objdump -b binary -m aarch64`, 512-byte header
stripped) is saved next to the captures. Note that `adrp`+`add` targets need a
**-0x5C0** correction to land on the right rodata offsets; that constant was
solved by matching one assert triple, not assumed.

#### 1. There is a general secure register accessor into MCUCFG, and it works

```
0xC200035F  IDVFS_READ (addr)       -> *(u32 *)addr
0xC200035E  IDVFS_WRITE(addr, val)  -> *(u32 *)addr = val
```

Both are guarded by exactly one check — `(addr & 0xFFFFC000) == 0x10220000` —
and return -3 otherwise. **This entry previously recorded "IDVFS_READ returns 0
for every address tried" from two probes, `0x10006218` and `0x1001a204`. Both
are outside that window.** The measurement was about the guard, not the
service. Inside the window it is unrestricted read *and write* to all of
MCUCFG from EL1, and `IDVFS_WRITE` had never been tried at all.

Proof, same address two ways, on the running machine:

| address | non-secure `ioremap` | SMC |
|---|---|---|
| `0x102222a0` | `00000000` | `00ba0100` |
| `0x102224a0` | `00000000` | `00ff1101` |

#### 2. `BIGIDVFSSRAMLDOSET` does exactly what it says. It is not a stub.

ATF's handler is a plain register write with **no EEM/PTP dependency anywhere
in it**:

```c
w = mmio_read(0x1020666C) & 0xffff;          /* the eFuse cal */
mmio_write(0x102222B4, w ? w : 0x7777);
vosel = (mv_x100 <= 70000) ? 0x8f1
      : (mv_x100 <= 90000) ? 0x8f2
      : 0x8f0 | (3 + (mv_x100 - 90000) / 2500);
mmio_write(0x102222B0, (mmio_read(0x102222B0) & 0xFFFFF000) | vosel);
```

Measured: `0x102222b0` goes `00411fc9 -> 004118fb`, which is
`(old & ~0xfff) | 0x8f0 | 0xb` to the bit, and `0x102222b4` reads `0000998d` —
**exactly Android's `LDO_Cal/eFuse = 0x998d`**. "Returns success and does
nothing" was an artefact of having no read-back. **So the whole EEM/PTP lead
that this entry recommended as "the thread to pull next" is dead.**

#### 3. The MP2 MCUCFG block is powered through the external-buck isolation cell

This is the finding that unlocked everything else. At rest, every register in
`0x10222xxx` reads zero **even through the secure accessor**, while the MP0 and
MP1 blocks return real values through the same call:

| state | `0x1022222c` | `0x102222a0` | `0x102222b0` | `0x102222b4` |
|---|---|---|---|---|
| at rest | 0 | 0 | 0 | 0 |
| VPROC2 on, still isolated | 0 | 0 | 0 | 0 |
| **VPROC2 on + `CPU_EXT_BUCK_ISO` cleared** | `ffffffff` | `00400100` | `00411fc9` | `0000998d` |

So the big cluster's config block lives on VPROC2 behind `B_EXT_BUCK_ISO`
(SPM+0x290 bit 1). Until both are done, every read of it is meaningless — which
is why so many earlier readings of this region were zeros.

#### 4. ATF's cluster power-on, transcribed, and where it really hangs

```
power_on_cl3():                                    /* plat/mt6797/power.c */
    INFO("%s before top:%x c0:%x c1:%x", ...)
    MP2_CPUSYS_PWR_CON |= PWR_ON                   /* SPM+0x218 bit 2 */
    udelay(2)
    while (!(CPU_PWR_STATUS & BIT(17))) ;          /* SPM+0x188  <-- the spin */
    while (!(mcucfg(0x102222A0) & BIT(17))) ;
    assert(mcucfg(0x102224A0) == 0x00FF1100)
    assert(mcucfg(0x102224A4) == 0xB9B13B14)
    assert(mcucfg(0x102224AC) == 0x01B10100)
    assert(mcucfg(0x102224B0) == 0x00AF00AF)
    assert(mcucfg(0x102224B4) == 0x00000010)
    MP2_CPUSYS_PWR_CON &= ~PWR_CLK_DIS
    mcucfg(0x102224A0) = 0x00FF0100 / 0x00FF0101 / 0x00FF1101   /* big PLL */
    CSPM(0x11015000) = 0x0B160001 ; take sema 0x11015448
    MCUMIXED 0x1001A270 |= 1 ; 0x1001A274 = (x & ~0x1f) | 8
    <freq meter on abist source 37>  expect ~750000 kHz, else RETRY (unbounded)

power_on_big(cpu):
    if (big_on == 0) power_on_cl3()
    mcucfg(0x10222208) = 0x000F0000
    mcucfg(0x10222290 + idx*8) = entry ; (+4) = 0     /* the core's reset vector */
    MP2_CPUn_PWR_CON &= ~PWR_RST_B ; |= PWR_ON        /* SPM+0x240 + idx*4 */
    while (!(CPU_PWR_STATUS & BIT(15 - cpu))) ;
    while (!(mcucfg(0x10222430 + idx*4) & BIT(17))) ;
    MP2_CPUn_PWR_CON |= PWR_RST_B
    big_spark2_setldo(0, 0)                            /* mcucfg(0x10222700) = 0x3f */
```

**`CPU_PWR_STATUS` bit 17 is MP2_CPUTOP**, established by measurement, not by
a header: bits [21:16] read `0x3d` on a machine where MP2 is the only dark
cluster — bit 17 is the only clear one.

Two things follow. The core-level bits are `BIT(15 - cpu)`, so cpu8 = bit 7 and
cpu9 = bit 6 (which confirms this entry's earlier `MP2_CPU0..3 = 7..4`). And
**the reset vector for a big core is MCUCFG `0x10222290 + idx*8`, written
*after* the poll that hangs** — so in every previous attempt the A72 had no
entry point, and `MP2_CPU0_PWR_CON` (SPM+0x240) was never driven at all. The
"the A72 core executes nothing" verdict below was measuring a core that was
never told where to start and never individually powered.

#### 5. Driven by hand, the cluster comes all the way up

Running the prerequisites and then the sequence from Linux, with no PSCI call
anywhere (`tools/a72-bringup` stage 5/6):

```
SPMC ack (bit17)           OK after 0 us
MP2=00010137  MCUCFG 0x102222a0=00ba0100     (bit 17 set)
0x102224a0=00ff1100   <- ATF's asserted value, exactly
0x102224a4=b9b13b14   <- ATF's asserted value, exactly
0x102224ac=01b10100   0x102224b0=00af00af   0x102224b4=00000010
```

**All five of ATF's assertions hold.** Both of its polls would pass. Walking the
MTCMOS bits the way `spm_mtcmos_ctrl_cpusys0()` does for MP0 then gets:

```
MP2_CPUSYS_PWR_CON  = 0001004d   (MP0 running = 0009004d)
MP2_CPU0_PWR_CON    = 0001004d   (every running A53 core = 0001004d)
core SPMC 0x10222430 = 00bb0100  (bit 17 set)
```

The core register is **bit-identical to a running A53 core**. Note the SPMC ack
is *not* the whole state: with bit 17 asserted, PWR_CON still read `0x00010127`
until the software sequence was walked.

`MP3_CPUSYS_PWR_CON` — a cluster this SoC does not have — reads `0x0001004D`.
That is the same value this entry previously celebrated reaching on MP2, so
that number on its own proves nothing.

#### 6. The clock is real, and it is not the one ATF expects

The vendor's own freq meter (`_mt_get_cpu_freq_idvfs`, abist sources 34/35/36/37
= LL/L/CCI/Big) was validated against clocks we already know before its answer
about cluster B was believed:

```
LL=624000  L=1209000  CCI=629992  BIG=629992 kHz
```

LL and L are real cpufreq points, so the instrument works. **Cluster B measures
630 MHz — the same as CCI — while its own PLL is programmed for 750 MHz**
(`0x102224a4 = 0xb9b13b14` -> `0x39b13b14 * 26 / 2^24 = 1501 MHz`, posdiv 2).
So `ARMPLLDIV_MUXSEL[1:0]` is not what switches this cluster, which agrees with
the vendor's own per-cluster table (`MT_CPU_DVFS_B` has no ARMPLL).

`BIGIDVFSENABLE` was then issued the way the vendor does it, after configuring
the iDVFSAPB i2c bridge at `0x11017000` first so the co-processor does not
collide with our regulator on i2c6. It works:

```
BIGIDVFSENABLE(0x0010a203, 100000, 120000) -> 0
0x10222470 = 0010a203     <- Android's control word, now live
0x102224c8 = 0f00466c     <- was 0
```

and ATF's own log ring agrees: `iDVFS enable start.` /
`IDVFS_PLLINDEX = 0xf627, rg_armpll_sdm_pcw_r = 0x39b13b14, IDVFS_SWREQ = 0x1e000`
/ `iDVFS enable success.` Cluster B still measures 630 MHz afterwards.

#### 7. ATF logs to a ring buffer we can read from our own kernel

`/proc/atf_log` on Android is just a ring in reserved DRAM, and **LK puts
`tee_reserved_mem` in our device tree too**: physical **`0x7FF40000`, 0x40000
bytes**, 256-byte control header then the ring. `busybox devmem` reads the
header; the body needs an `mmap` of `/dev/mem` (a plain `read()` returns
zeros). This is ATF narrating itself, live, and it survives into our system.

It is also how the little cores' reset vector was found:

```
... power on CPU5 ...
little_spark2_setldo sparkvretcntrl=3f
Little power on:0x3F
mt_on_1, entry 10103c
```

`0x0010103c` is ATF's warm-boot entry in **on-chip SRAM**, and MP0's boot
address register `0x10220038` holds exactly that on this machine.

#### 8. What is actually still wrong

With all of the above in place, the core does not fetch. Two independent
witnesses, both of which were checked for their ability to say otherwise:

* a magic word written to an **uncached** `dma_alloc_coherent` mailbox (the
  earlier probe used a cacheable page cleaned only to the PoU, which a
  cache-off core outside the coherency domain need never see) — stays zero;
* `SLEEP_TIMER_STA` bit 10, `MP2_CPU0_STANDBYWFI` — stays **1**. The stub was
  changed from `wfi` to a branch-to-self precisely so this bit would have to
  move if the core ran a single instruction. It does not.

Tried and did not help: pointing the core at our DRAM stub; pointing it at
`0x0010103c`, the entry the A53s actually use; `0x10222208 = 0x000F0000`
(AA64nAA32); toggling `SRAM_SLEEP_B` for an edge; `big_spark2_setldo` (already
`0x3f`, and MP0/MP1 read `0x3f3f3f3f`).

Two concrete anomalies remain, and they are the leads:

1. **`SRAM_SLEEP_B_ACK` (bit 19) never asserts on the cluster.** MP0 and MP1
   both have it (`0x0009004d`); MP2 reaches `0x0001004d` and stops. Every
   running *core* reads `0x0001004d` with bit 19 clear, so this is specifically
   the cluster's L2 SRAM.
2. **`0x10222220`–`0x1022223c` reads `0xffffffff` and drops writes**, while
   `0x10222200`–`0x1022221c` reads real values through the same call. That
   range contains MP2's `AXI_CONFIG` (`0x1022222c`, ACINACTM bit 4) — ATF's own
   cluster switch is `0x1022002c` / `0x1022022c` / `0x1022222c`. An all-ones
   sub-block next to a working one is an unclocked or ungated slave, and
   ACINACTM stuck at 1 would keep the cluster out of the coherency domain.

#### 9. The obvious next experiment, and why it was not run

**Call PSCI `CPU_ON` now.** Every precondition ATF checks is verifiably
satisfied — both polls pass, all five asserts match, and `MP2_CPU0_PWR_CON` is
left powered down so `power_on_big` will not refuse with "was powered on". ATF
also knows the correct reset vector and sets up its own per-CPU context, which
hand-driving cannot.

The reason it was left for a session with someone watching: `power_on_cl3`
ends in a **frequency check with an unbounded retry loop** — it wants
750000 ±15000 kHz on the first pass and 500000 ±10000 on later ones, and
cluster B measures 630 MHz. If it never matches, ATF spins at EL3 with
interrupts masked and the machine is lost until
`03-tools/uhubctl/uhubctl -l 1-10 -p 1 -a cycle -d 8` (leave the port `-a on`).

#### 10. Housekeeping, and one thing that bit

`tools/a72-bringup` **stage 0 restores everything** — cores down, cluster down,
`B_EXT_BUCK_ISO` re-asserted, VPROC2 off. This matters: B-47 is an energy
limit, and leaving VPROC2 up costs current continuously.

**Running stage 0 took the machine down** at the cluster power-down step
(`cluster SRAM_PDN_ACK TIMEOUT`, netconsole stops there). The watchdog
reclaimed it and it came back on its own in about a minute, healthy, at `#51`
with 8 CPUs and cpufreq capped — a reset also restores VPROC2 and the isolation
cell to their defaults, so the resting state is safe either way. But the
cluster power-**down** path is not right yet and should not be trusted; prefer
a reboot to undo an A72 experiment.

---


### GEMIAN ANSWERS IT: no Linux on this device has ever run the A72s

Booted the stock Gemian Reference kernel and asked it directly. **It brings up
five CPUs and never touches the A72 cluster:**

```
Brought up 5 CPUs
SMP: Total of 5 processors activated (130.00 BogoMIPS)
[IRQ] @CPU8: smp affinity is set, but gic reg is not set
[IRQ] @CPU9: smp affinity is set, but gic reg is not set
```

`online` = `4-7` (plus cpu0 as HPS cycles), **zero cores reporting `0xd08`**, and
forcing `cpu8/online` returns rc=0 while the CPU stays offline — silently
vetoed, and notably **without the hang our kernel gets**.

**This settles the question that motivated the whole detour.** Gemian carries
`cpu_power_on_buck()` byte-identical, with `CONFIG_CL2_BUCK_CTRL` hardcoded on
and `CONFIG_NR_CPUS=10` — and it still does not bring the A72s up. So the buck
and isolation work, which this session ported and proved, was never going to be
sufficient on its own. **Android is the only system on this device that runs
those cores.**

**And our port already beats the Reference slot.** `#51` runs **8** CPUs;
stock Gemian runs **5**.

| system | kernel | CPUs online | A72s |
|---|---|---|---|
| Android (boot1) | 3.18.79 vendor | **10** | **2** |
| Gemian (Reference) | 3.18.41 | 5 | 0 |
| ours, `#51` | mainline 6.6 | **8** | 0 |

So the bar was never "match Gemian". The A72s are an unsolved problem for Linux
on this hardware generally, not a regression in this port — which also means
there is no working Linux implementation to copy, and the EEM/PTP-and-iDVFS
hypothesis is the remaining lead rather than one option among several.

**One caveat on evidence quality:** Gemian's `/proc/config.gz` reports
`# CONFIG_MTK_HYBRID_CPU_DVFS is not set` while `/sys/kernel/debug/cpuhvfs`
exists on the running system. Those cannot both describe the same kernel, so
that config is a generic stub and **nothing should be concluded from it**. The
boot-log CPU count is the reliable measurement.

### HOW TO BOOT GEMIAN WITHOUT THE KEY CHORD, and how to get back

The silver chord is unreliable. It is not needed:

1. Stage the current experimental image where Gemian can see it —
   **`/dev/mmcblk1p29` is mounted at `/host` in our system and is Gemian's
   root**, so `scp <img> root@device:/host/restore.img` lands at `/` under
   Gemian.
2. `gemini-slot.py flash recovery 01-backups/boot2-stock-gemian-*.img -y`
3. Reboot. LK's BCB sends it to p1, which now holds Gemian.
4. Reach it at **`ssh gemini@10.15.19.82`** — same gadget IP, and the host key
   differs from ours so `-o StrictHostKeyChecking=no` is required. Under Gemian
   the eMMC is **`mmcblk0`**, not `mmcblk1`.
5. To return: `dd` the staged image back to `/dev/mmcblk0p1`, verify by
   readback, reboot. The BCB is still armed, so it lands on the restored image
   directly — no Android round trip.

**The trap to plan for:** the BCB is sticky and Gemian has no
`gemini-bcb-disarm`, so it loops back into Gemian on every boot. Staging the
restore image in `/host` **before** flashing is what makes that safe. Gemian's
`gemini` user is in `sudo` but the NOPASSWD drop-in recorded in
`STATE-2026-08-17` has since been removed, so the account password is required
— have it to hand before starting.

---

### FINAL for this session: the A72 core does not execute, and the diagnosis is coherent

The last open question was whether the cores actually start once the domain
looks powered. They do not, and this time the instrument was proven able to say
otherwise before it was believed.

The A72's entry point is a hand-assembled stub (checked against objdump) that
writes a magic word to a mailbox and then parks:

```
movz x0,#lo ; movk x0,#hi,lsl#16 ; movz w1,#0x72a7 ; movk w1,#0xa72a,lsl#16
str w1,[x0] ; dsb sy ; 1: wfi ; b 1b
```

With MP2 hand-powered to 0x0001004d, a park page at PA 0x44c01000 and a mailbox
at PA 0x44c3e000: **the mailbox stayed 0x00000000 for 600 ms.** The A72 fetched
and executed nothing.

**Two instrument failures had to be fixed before that result meant anything,
and both are the same mistake this project keeps making.**

* The first mailbox was `CSPM_SW_RSV15`. A kernel store of 0 left it reading
  `0xbabebabe` — it is not writable, so the A72 could not have written it
  either, and the "never executed" verdict it produced said nothing. The probe
  now writes and reads back the mailbox first and refuses to report if that
  fails.
* The park page was being allocated with plain `GFP_KERNEL` and landed at PA
  `0x10a41c000`, **above 4 GB**, so the entry point itself was suspect. Now
  `GFP_DMA32`.

Worth noting the correlation that exposed the second one: the single run whose
failure looked *different* — a hard reset in ~300 ms instead of the usual slow
wedge — was the one run whose park page happened to land below 4 GB.

**The coherent picture.** Every MTCMOS bit reaches the value a running cluster
has except `SRAM_SLEEP_B_ACK`; neither SRAM ack ever responds in any state or
at any clock source; and the cores do not execute. That is one fault, not
three: **the MP2 SRAM has no supply**, so its control logic cannot acknowledge
and its cores cannot run.

The supply is set by `BigiDVFSSRAMLDOSet()` → `0x102222b0` in MCUCFG →
unreachable from the non-secure world → SMC `0xC20003BF`, which **returns 0 and
has no observable effect**.

**So the whole A72 blocker now reduces to one sentence:** we cannot power the
big cluster's SRAM, because the only interface to it is a secure call that
silently does nothing on this firmware.

Everything else on the path is solved and reproducible: the buck, the
isolation, the MTCMOS sequence, and the SMCs that do work.

---

### NARROWED TO ONE BIT, same evening: MP2 reaches 0x0001004d and only SRAM_SLEEP_B_ACK is missing

Driving the whole cluster MTCMOS sequence by hand — with the buck
prerequisites in place this time, which is what B-40's earlier hand-drive
lacked — gets MP2 one bit away from a running cluster:

```
MP2 after the hand sequence   0x0001004d
MP0, a running cluster        0x0009004d
                              ^^^^ the only difference is bit 19
```

Every step completes. PWR_ON, PWR_ON_2ND, PWR_CLK_DIS cleared, PWR_ISO
cleared, SRAM_PDN cleared with **SRAM_PDN_ACK clearing in 0 us**, SRAM_ISOINT_B
set, SRAM_CKISO cleared, SRAM_SLEEP_B set, PWR_RST_B released. This is much
further than B-40 ever got, and it retires that entry's "the MP2 power switch
does not acknowledge for anyone" — with the isolation cleared, it does.

**What is left is `SRAM_SLEEP_B_ACK` (bit 19), and it never asserts.**

**And the tell is that NEITHER SRAM ack ever moves, in any state.** At rest MP2
sits with `SRAM_PDN = 1` and `SRAM_PDN_ACK = 0` — a genuinely powered-down SRAM
should be acknowledging that. Both acks read 0 always. That is the signature of
SRAM control logic with no supply, not of logic that disagrees with the request.

**Not the clock.** Cluster B's source is `ARMPLLDIV_MUXSEL[1:0]` in MCUMIXED,
which we *can* write. Walked through all four (CLKSQ / ARMPLL / MAINPLL /
UNIVPLL): both acks stayed 0 at every one.

**So the missing thing is the big cluster's SRAM rail**, which is exactly the
one step of `cpu_power_on_buck()` we cannot perform: `BigiDVFSSRAMLDOSet(110000)`
writes `0x102222b0`, in MCUCFG, which is unreachable from the non-secure world.
The SMC that would do it, `0xC20003BF`, **returns 0 but has no observable
effect** — the acks do not move whether it has been called or not.

Two things that are NOT the problem, both measured:

* The eFuse calibration is present and readable. `0x1020666C` reads
  `0x0000998D` from Linux, **exactly** the `Big Vsram LDO_Cal/eFuse = 0x998d`
  Android reports. Only MCUCFG (0x10200000 / 0x10222000) is blocked; the eFuse
  block at 0x10206000 is fine.
* The ack register hunt is settled. `PWR_STATUS`/`CPU_PWR_STATUS` are SPM+0x180
  and +0x188, and the mt6797 CPU bit map is `MP0_CPU0..3` = bits 15..12,
  `MP1_CPU0..3` = 11..8, **`MP2_CPU0..3` = 7..4**. MP2's four bits are zero in
  every sample, which is consistent — we never got a core started, only the
  cluster domain.

**The next hypothesis, and it is concrete.** `BigiDVFSEnable_hp()` refuses to
run unless `infoIdvfs == 0xff`, which the vendor comments as "true eFuse enable
ptp" — i.e. **EEM/PTP must be initialised first**, and `mt_eem.c` is unported.
If ATF's `BIGIDVFSSRAMLDOSET` depends on state that EEM sets up, that would
explain a call that returns success and does nothing. That is the thread to
pull next.

**The cheaper alternative, if hands are available:** boot the Gemian Reference
slot (boot2, needs the SILVER chord) and read `/sys/devices/system/cpu/online`.
Gemian's `arch/arm64/kernel/psci.c` carries `cpu_power_on_buck()`
byte-identical with `CONFIG_CL2_BUCK_CTRL` on and `CONFIG_NR_CPUS=10`. If
Gemian shows 0-9, a stock Linux does this with our exact ATF and the diff is
purely in kernel state; if it shows 0-7, then no Linux has ever done it here
and Android's extra ingredient is the whole question.

---

### CORRECTION, later the same evening: the iDVFS SMC service IS present. The SETTERS work; only the getters are missing.

The section below concludes "there is no SMC shortcut" from four read-only
getters all returning SMC_UNK against a bogus-id control. The control was
sound and the getters really are absent — but the conclusion drawn from them
was wrong, because **the calls the vendor actually uses on the A72 path are
setters, and this ATF implements them**:

```
BIGIDVFSSRAMLDOSET(110000)   fn=0xC20003BF  ->  0    ANSWERED
BIGIDVFSENABLE(0x0010a203, 100000, 110000)
                             fn=0xC20003B0  ->  0    ANSWERED
```

against `BIGIDVFSPLLGETFREQ/GETPOSDIV/GETPCW/SRAMLDOGET` (0xC20003BA/BC/BE/C0)
which all return SMC_UNK, exactly like the bogus id. So the family is
**partially implemented: writes yes, reads no.**

**The methodological error is the useful part.** I chose the getters
deliberately, because they are read-only and therefore safe to probe cold.
That is good instinct for safety and bad sampling for the question asked:
"is this service present?" was answered with the subset of the service least
likely to be implemented, and the answer generalised to the whole family. A
safe probe is not automatically a representative one.

It also explains a puzzle recorded above. Android's `/proc/idvfs/dvt_test`
reports `Big Vsram = 1100mv` and a 16-bit `LDO_Cal/eFuse = 0x998d` — and the
16-bit width is the tell: `BigiDVFSSRAMLDOEFUSE()` only returns 16 bits on the
path taken when both A72s are offline, i.e. the one that does NOT do a secure
read. Android was never exercising the getters either.

**What this changes.** The last unreproduced step of `cpu_power_on_buck()` —
`BigiDVFSSRAMLDOSet(110000)`, the big cluster's SRAM rail — is available after
all, and so is `BigiDVFSEnable`. MCUCFG being unreadable from the non-secure
world (verified: reads 0, writes dropped, and enabling the Device APC clock
changes nothing) stops mattering, because ATF will do those accesses for us.

**And it is STILL not enough.** With the full sequence run — MP2 bit 0 set,
VPROC2 enabled, CPU_EXT_BUCK_ISO cleared 0x2 -> 0x0, SRAMLDOSET and
BIGIDVFSENABLE both returning 0 — ATF's CPU_ON still reaches
`MP2_CPUSYS_PWR_CON = 0x00010137` and stops. Decoded against the vendor's own
bit names:

| bit | name | MP2 stalled | MP0 running |
|---|---|---|---|
| 0 | PWR_RST_B | 1 (we set it) | 1 |
| 1 | PWR_ISO | **1** | 0 |
| 2 | PWR_ON | 1 (ATF) | 1 |
| 3 | **PWR_ON_2ND** | **0** | **1** |
| 4 | PWR_CLK_DIS | 1 | 0 |
| 5 | SRAM_CKISO | 1 | 0 |
| 8 | SRAM_PDN | 1 | 0 |

ATF sets PWR_ON and waits for the power-good before setting PWR_ON_2ND, and
the good never comes.

**The next measurement, and it is read-only.** The ack ATF polls is not in
PWR_CON at all — it is `PWR_STATUS` at SPM+0x180 and `CPU_PWR_STATUS` at
SPM+0x188 (this blocker previously looked at 0x60c, which is another SoC's
header and reads 0 here even for the running MP0/MP1). Reading those four
registers with MP2's PWR_ON asserted says directly whether the power switch is
acknowledging at all — which separates "the supply is not reaching the domain"
from "the sequence is wrong".

Also note B-40's earlier "SRAM_PDN_ACK stays set" reading used a bit position
that does not match this header: SRAM_PDN_ACK is bit **12**, and it is 0 in
both the quoted 0x0001004C and the resting 0x00010132.

---

### 2026-08-22 (evening): there is NO SMC shortcut to the A72s. Measured, and it cost 15 minutes.

Before porting 2500 lines of CSPM, a cheaper hypothesis was worth eliminating,
and reading `mt_idvfs.c` had made it look very promising:

```c
int BigiDVFSEnable_hp(void)   /* for cpu hot plug call */   <- the vendor's own comment
        ...
        SEC_BIGIDVFSENABLE(idvfs_init_opt.idvfs_ctrl_reg, cur_vproc, cur_vsram);
        /* = smc(MTK_SIP_KERNEL_IDVFS_BIGIDVFSENABLE = 0xC20003B0, ...) */
```

The Big cluster's enable is **a secure monitor call to ATF**, not a register
sequence, and there is a whole SIP family 0xC20003B0..0xC20003C1 plus a
register read/write pair. There is also a second hardware block for it —
`dvfs_proc2@11016000`, distinct from CSPM at 0x11015000. If our ATF carried
that service, the A72 route would have been a few SMCs.

**It does not.** Extended `tools/psci-probe` with the four read-only getters
and, crucially, a deliberately-bogus SIP id as the control:

| call | returns |
|---|---|
| `BIGIDVFSPLLGETFREQ` 0xC20003BA | `0xFFFFFFFF` SMC_UNK |
| `BIGIDVFSPLLGETPOSDIV` 0xC20003BC | `0xFFFFFFFF` SMC_UNK |
| `BIGIDVFSPLLGETPCW` 0xC20003BE | `0xFFFFFFFF` SMC_UNK |
| `BIGIDVFSSRAMLDOGET` 0xC20003C0 | `0xFFFFFFFF` SMC_UNK |
| **bogus id 0xC20003FE (control)** | **`0xFFFFFFFF` SMC_UNK** |

The control is the whole point: this firmware *does* reject ids it does not
know, so SMC_UNK from the getters means absent rather than meaningless.

`IDVFS_READ` (0xC200035F) is the one id that does **not** return SMC_UNK — but
it returns 0 for every address tried, including `0x10006218` where devmem reads
`0x00010132` and `0x1001a204` where devmem reads `0xC00BC000`. Whatever answers
at that id is not reading registers. It is not the iDVFS service.

**So the route is the CSPM port after all**, which is what Android's own log
said all along (`[CPUHVFS] cluster2 on, swctrl = 0x25f0`), and `cspm_probe()`
confirms the mechanism: `swctrl_reg[CPU_CLUSTER_B] = CSPM_SW_RSV2`, a software
reserved register the PCM firmware polls. iDVFS is about the big cluster's
*frequency* once it is already powered, and this ATF cannot do that either.

**Two corrections to the port plan while here.**

1. **The firmware to load is the 2025-word one, not the 1969-word one.**
   `CPUHVFS_HW_GOVERNOR` is commented out in the vendor's own header
   (`base/power/include/mt_cpufreq_hybrid.h:30`), so the shipped build takes the
   `#else` branch: `pcm_dvfs_v0.1_160131_02`, size **2025**. The 1969-word
   `v0.2` quoted in the handoff and in this blocker's earlier correction is the
   hardware-governor variant that Android does not run.

2. **CSPM needs the i2c6 clock, and the DT node says so.** The vendor node is

   ```
   compatible = "mediatek,mt6797-dvfsp";
   reg = <0x11015000 0x1000>, <0x0012a000 0x3000>;   /* CSPM regs, CSRAM 12K */
   interrupts = <GIC_SPI 161 IRQ_TYPE_LEVEL_LOW>;
   clocks = <&infrasys INFRA_I2C_APPM>;  clock-names = "i2c";
   ```

   `INFRA_I2C_APPM` is `CLK_INFRA_I2C_APPM` (54) in mainline's binding — the
   *same* controller cpufreq's regulator now sits behind. B-45's warning is no
   longer about observation: i2c6 is in the path of **every DVFS transition**
   since #51. Plan the semaphore or the pause before starting CSPM, not after.

   Also note `__cspm_kick_im_to_fetch()` hands CSPM a **physical address**
   (`base_va_to_pa`) and the IM fetches the PCM image from DRAM over EMI — so
   the firmware array's placement and any EMI MPU permissions are a real part
   of the port, not a detail.


### RESOLVED 2026-08-22, the cpufreq half: DVFS is live and worth the measured 1.72x / 1.57x

`#51`. Two policies, `policy0` = cpu0-3 on the little cluster's table and
`policy4` = cpu4-7 on the big-little cluster's, driver `mtk-cpufreq`,
governor schedutil. The A72 half of this blocker is untouched and stays open.

Measured with a fixed integer loop, the same instrument that established the
897/1274 MHz baseline:

| cluster | from | to | Miter/s | measured | clock ratio |
|---|---|---|---|---|---|
| LL (cpu0) | 897 MHz | 1547 MHz | 286.1 -> 502.2 | **1.755x** | 1.725x |
| L (cpu4) | 1209 MHz | 2002 MHz | 402.1 -> 666.2 | **1.657x** | 1.656x |
| L vs the 1274 MHz LK left | | | | **1.572x** | 1.571x |

**The PCW the hardware holds matches what the OPP arithmetic predicts at every
point tested** — 1547 MHz -> `0x400EE000`, 2002 MHz -> `0x40134000`,
1209 MHz -> `0xC00BA000`, 897 MHz -> `0xC1114000` — which is the same standard
of proof the GPU clock fix (#56) was held to.

What it took, in five patches:

* `clk/0001` — the MCUMIXED provider. The block at `0x1001a000` was never
  modelled, which is the whole reason there were no CPU clocks. Note it
  **corrects this blocker's own table**: ARMCAXPLL2 is the CCI, not the A72
  cluster. The vendor's per-cluster mapping is explicit — LL/L/CCI get
  ARMCAXPLL0/1/2 and `MT_CPU_DVFS_B` gets `NULL`, because that cluster's
  frequency belongs to the iDVFS co-processor.
* `regulator/0003` — VSRAM, an array of eight on-die LDOs in INFRACFG_AO.
  Measured at 1100 mV, not the 1200 mV the handoff recorded; pinned to 1200 mV
  so the whole 1000-1200 mV Vproc range satisfies the vendor's Vsram >= Vproc.
* `opp/0001` — a CPU with no OPP table shares nothing. Without this the two
  bare A72 nodes made `dev_pm_opp_of_get_sharing_cpus()` return -ENOENT for
  cpu0 and cost all eight A53s their cpufreq.
* `cpufreq/0001` — mt6797 platform data, skip CPUs with no OPP table, and
  blocklist mt6797 in `cpufreq-dt-platdev` (without which cpufreq-dt claimed
  all ten CPUs under one policy carrying the wrong table).
* `dts/0046` — clocks, OPP tables, VSRAM pin, and BUCKA widened to
  [1000000, 1200000] uV.

**Two deliberate narrowings of the OPP tables, both costing only idle power.**
The bottom of each is cut where the vendor starts using the `ARMPLLDIV_CKDIV`
fractional divider (LL below 624 MHz, L below 650 MHz), which nothing here
models. And voltages are floored at 1000 mV, because **BUCKA is not the little
cluster's rail alone** — the vendor puts LL, L *and CCI* on VPROC1, this port
scales only the A53s, and the CCI sits at ~630 MHz where 1000 mV is a
measured-safe floor. Lowering it is what a CCI policy would earn.

**Read B-47 before enabling this for daily use.** Eight cores at the top of
these tables took the machine down after ~15-30 s, with the battery sagging
620 mV first.


### CORRECTION 2026-08-22 (same evening): the firmware is NOT missing

The update below ends with "the A72s need firmware we do not have". **That is
wrong.** The CPUHVFS PCM image is a GPL-licensed C array in the vendor tree we
already have locally:

```
07-kernel/ubports-3.18/drivers/misc/mediatek/base/power/mt6797/
    mt_cpufreq_hybrid_fw.h   static const u32 dvfs_binary[]
                             struct pcm_desc dvfs_pcm = {
                                 .version = "pcm_dvfs_v0.2_160125_02",
                                 .size    = 1969,   /* words */
                             };   /* two variants, #ifdef CPUHVFS_HW_GOVERNOR */
    mt_cpufreq_hybrid.c      2512 lines — the CSPM driver that loads it,
                             CSPM base 0x11015000
```

So this is **an unported driver, not absent firmware** — a materially easier
problem, and the same shape as the connectivity and panel ports this project
has already done. `cspm_module_init()` (line 1993),
`__cspm_kick_im_to_fetch()` (951) and `cpuhvfs_kick_dvfsp_to_run()` (2456) are
the entry points.

The failure mechanism recorded below stands unchanged; only the conclusion
about reachability was wrong. I recorded an absence of evidence as a
conclusion, in a blocker, an issue and a state document, before looking in the
tree for the thing I said we did not have.

**One interaction to plan for before starting CSPM:** i2c6 works today
*because* the DVFS processor is not running. The vendor's i2c driver takes
`cpuhvfs_get_dvfsp_semaphore(SEMA_I2C_DRV)` before every transfer on the
`mediatek,appm_used` bus because the co-processor masters it too, and mainline's
`i2c-mt65xx` knows nothing about that. Starting CSPM is the most likely way to
break the bus that the CPU regulator sits on (B-45).

### UPDATE 2026-08-22 (later): DIAGNOSED. ATF asserts PWR_ON for MP2 and the domain never acknowledges

A formal diagnosis pass. The headline is that **the hang is not in Linux and not
in the supply — it is inside ATF, in the MTCMOS power-up for cluster 2, and it
is a spin with no timeout.**

#### The instrument, which is most of the result

Writing `cpu8/online` hard-locks the writing CPU and costs ~4 minutes of
watchdog reset per attempt, with "the machine went away" as its entire output.
Two replacements, both in `tools/psci-probe/`:

* **`gemini-psci-probe.ko`** — PSCI calls that change nothing, each announced
  to `/dev/kmsg` **before** it is issued. A call that never returns still names
  itself, because netconsole emits synchronously.
* **`cpuhp/fail`** — write a state number to
  `/sys/devices/system/cpu/cpu8/hotplug/fail` and everything below it still
  runs. Arbitrary `target` needs `CONFIG_CPU_HOTPLUG_STATE_CONTROL`, which we
  do not have; `fail` bisects the hotplug path with no rebuild at all.

#### What they establish

| probe | result |
|---|---|
| `PSCI_VERSION` | **0.2** — so `PSCI_FEATURES` returning NOT_SUPPORTED is correct, not a fault |
| `CPU_ON(cpu1, bogus entry)` | `-4 ALREADY_ON`, survives — the control |
| `CPU_ON(cpu8, **bogus** entry)` | `-2 INVALID_PARAMS`, **survives** |
| `CPU_ON(cpu8, **valid** entry)` | **HANGS** |
| `cpu_up(8)` with `fail=93` | states 1-92 all pass, fails cleanly at `cpu:bringup`, **machine alive** |
| `AFFINITY_INFO(0x000, 0)` | **hangs**, on a core that is demonstrably ON |

Two things follow immediately. **The entire cpuhp prepare phase is innocent** —
the hang is inside state 93, `cpu:bringup`. And the difference between a
surviving CPU_ON and a fatal one is *only the entry point*, which means this
firmware validates the address **before** the MPIDR: the `INVALID_PARAMS` was
the address check, and the earlier reading of it as "firmware does not know
cluster 2" was wrong.

#### Where it hangs, watched live

Polling the SPM from the other CPUs while one is stuck inside the SMC:

```
before        MP2_CPUSYS_PWR_CON = 0x00010132     (reset value)
during, x50   MP2_CPUSYS_PWR_CON = 0x00010136     and never changes again
```

`0x132 -> 0x136` is bit 2, and `mt_spm_reg_mt6797.h` names it
`MP2_CPUTOP_PWR_ON_LSB`. **ATF sets PWR_ON and then spins.** It never reaches
`PWR_ON_2ND` (bit 3). It is waiting for a power-good that never asserts, in a
loop with no timeout, at EL3 with interrupts masked — which is exactly a hard
lockup on the calling CPU and every other CPU piling up behind
`cpus_read_lock` afterwards.

#### VPROC2 is definitively not the cause

Repeated with BUCKB enabled at 1.000 V, and the trace is identical: `0x132 ->
0x136`, stalled, SMC never returns. Two independent runs, now with a precise
readout rather than "the machine died". Also eliminated: cluster-2 CCI
coherency (`ACINACTM` clear on all three clusters, both CCI-400 ACE ports at
`0xC0000003`) and the A72 PLL (`ARMCAXPLL2_CON0 = 0xF0000101`, enabled,
programmed for ~630 MHz).

#### Why the domain does not acknowledge

The vendor's own Linux tree has `spm_mtcmos_ctrl_cpu0..cpu7`, `cpusys0` and
`cpusys1` — and **no `cpusys2`**, confirmed again in
`mt6797/mt_spm_mtcmos.c`. So the vendor kernel does not power MP2 either.
Android does it through **CPUHVFS**, the DVFS co-processor running `dvfsp_fw`,
which the vendor kernel loads and ours never does. Its log line
`[CPUHVFS] cluster2 on, swctrl = 0x25f0` is that processor doing the work.

The coherent reading is that MP2's power switch is owned by CPUHVFS on this
SoC, and ATF's PWR_ON is a request that only completes when that processor is
running. **That is a firmware dependency we do not have**, and it is the answer
the handoff called "the honest risk".

#### What would still be worth trying, in order

1. **Drive the MTCMOS sequence by hand** (`tools/psci-probe/` has the script)
   and watch `SRAM_PDN_ACK` (bit 16) after clearing `SRAM_PDN` (bit 8). If it
   clears, the domain *can* be powered from the AP and ATF's spin is merely a
   sequencing bug we could pre-empt by powering before CPU_ON. If it never
   clears, the domain is not ours to power. **This was staged and not run — the
   device became unavailable first.**
2. If (1) works, pre-power MP2 and then call CPU_ON.
3. If (1) fails, the A72s need `dvfsp_fw` and a CPUHVFS driver, and that should
   be stated as the answer rather than ground against.

**Gap 2 of this issue — cpufreq for the A53s, a measured 1.72x and 1.57x — does
not depend on any of the above** and is now unblocked by the i2c6 fix.



### UPDATE 2026-08-22: VPROC2 is measured at last, and it is NOT the blocker

The i2c6 fix (B-45) made the DA9214 readable, so the rail question is finally a
measurement instead of an inference:

```
BUCKA_CONT 0x5D = 0x01   ENABLED,  VBUCKA_A 0xD7 = 0x46 -> 1000 mV
BUCKB_CONT 0x5E = 0x00   DISABLED, VBUCKB_A 0xD9 = 0x46 -> 1000 mV
```

**BUCKA is the live CPU rail**, at exactly the 1.000 V our device tree asserts
as a `regulator-fixed` fiction — so the A53 clusters run off BUCKA, and the
RT5735 on i2c7 is not their supply. **BUCKB is VPROC2 and has never been
switched on**, which matches the A72s being dark. Both identifications now rest
on chip reads rather than on `DA9214_VPROC2 = 0xD9`.

**But enabling it does not bring the A72 up, and I nearly reported that it
did.** Writing `BUCKB_CONT = 0x01` brings VPROC2 up cleanly — BUCKA untouched,
system healthy — and then `echo 1 > cpu8/online` hard-locks the writing CPU
within a second, exactly as before.

*The control that matters.* An RCU stall report during the attempt says
`ncpus=9`, and I read that as the A72 having joined. **It is not evidence of
anything.** Repeating the attempt with BUCKB deliberately left OFF produces
`ncpus=9` too: the count is incremented while the *control* CPU prepares the
hotplug, before the new core would ever run. With and without VPROC2 the
failure is identical.

### What is now eliminated

| candidate | verdict | evidence |
|---|---|---|
| **VPROC2 missing** | **dead** | the rail comes up on command; cpu8 fails identically with it on and off |
| **CCI snoop / ACINACTM for cluster 2** | **dead** | `MP0/MP1/MP2_AXI_CONFIG` (`0x1020002C/22C/42C`) all read `0x00000000`, so ACINACTM is clear for all three clusters; both CCI-400 ACE ports (`0x10394000`, `0x10395000`) read `0xC0000003` — SNOOP_REQ and DVM_MSG_REQ set; `CCI400_STATUS` = 0, nothing pending |
| **the A72 PLL being off** | **dead** | `ARMCAXPLL2_CON0` (`0x1001a220`) = `0xF0000101`, enabled; `CON1` = `0xC10C1D89` → posdiv 1, pcw 0x0C1D89 → **~630 MHz**, already programmed |

### What the failure actually looks like

`MP2_CPUSYS_PWR_CON` (`0x10006218`) stays at its reset value `0x00010132`
throughout — unpowered, isolated, in reset. **ATF never powers the cluster.**
The calling CPU hard-locks immediately (no interrupts at all), and other CPUs
pile up in `smp_call_function_many_cond`, waiting on a CPU that is in the
online mask and will never answer.

So the remaining candidate is the one B-40 named originally and is now the
*only* one left: **the MP2 MTCMOS sequence is never executed**, by the kernel or
by ATF. Under Android that work is done via CPUHVFS, the DVFS co-processor
running `dvfsp_fw`, which our kernel never starts.

### The next experiment, and it needs no kernel build

Write the MTCMOS power-up sequence to `MP2_CPUSYS_PWR_CON` and
`MP2_CPU0_PWR_CON` by hand with devmem and watch the register move from
`0x00010132` towards MP0's `0x0009004D`. If the cluster can be powered from the
AP side at all, that shows it without touching the kernel. Implement against
`mt_spm_reg_mt6797.h` addresses only — `mt_spm_cpu.h` is stale (above).

If it cannot, the A72s need firmware we do not have, and that is the answer.
Note the fallback still stands and is now *unblocked*: the A53 cpufreq port
needs BUCKA, which we can finally read and drive.



### UPDATE 2026-08-21 (evening): the boot1 trip happened, and it answers most of this

Stock Android on p22, booted with `gemini-bootsel.py normal --reboot` and left
with `gemini-bootsel.py recovery --via adb` + `adb reboot`. Read-only
throughout. Full captures in `04-docs/captures/android-boot1-2026-08-21/`.

**1. The A72s work, and so does everything else about this SoC.** All ten CPUs
online; cpu8/9 report `CPU part 0xd08` (Cortex-A72). Live DVFS tables:

| cluster | cores | range | at rest | Vproc | Vsram |
|---|---|---|---|---|---|
| LL (0-3) | A53 | 221 – **1547** MHz | 1118-1222 | 1160 mV | 1200 mV |
| L (4-7) | A53 | 325 – **2002** MHz | 325-2002 | 1130 mV | 1200 mV |
| B (8-9) | **A72** | 338 – **2522** MHz (`cpufreq_oppidx` lists 2587) | 845 | 890 mV | 1100 mV |
| CCI | — | 169 – 988 MHz | 845 | 1010 mV | 1125 mV |

Note there is a **VSRAM rail as well as VPROC**, at a different voltage. Any
OPP port needs both (`proc-supply` *and* `sram-supply`).

**2. What our own A53s are actually running at — measured, not estimated.**
`ARMCAXPLL0/1_CON1` at `0x1001a204` / `0x1001a214` (MCUMIXED 0x1001a000,
offsets and the PCW/posdiv/ckdiv arithmetic from the vendor `mt_cpufreq.c`
`_cpu_freq_calc()`), read on kernel `#41`:

```
0x1001a204 = 0xC1114000   posdiv=1 pcw=0x114000  ->  897 MHz   cpu0-3 (LL)
0x1001a214 = 0x400C4000   posdiv=0 pcw=0x0c4000  -> 1274 MHz   cpu4-7 (L)
0x1001a274 (CKDIV1) = 0                             no further division
```

897000 is exactly an entry in Android's LL OPP table, which is the cross-check.
Confirmed independently with a fixed integer loop: **173-179 Miter/s on cpu0-1,
254 Miter/s on cpu4-7** — a ratio of 1.47 against the 1.42 the registers
predict.

**So the machine runs its A53s at 58% and 64% of their rated maximum**, and
`173 Miter/s` — the number B-40 originally recorded and read as "roughly
1.2-1.4 GHz" — is cpu0 at **897 MHz**. That estimate was too high and is
corrected here.

**DVFS is therefore worth more than the A72s, as suspected.** 1.72x on the
little cluster and 1.57x on the big-little cluster, from a port with no new
silicon to power up, against 2 extra cores that need a firmware path we do not
have.

**3. Cluster 2's power sequence is CPUHVFS, and that is why it was never
found.** B-40 asked where MP2's MTCMOS sequence lives, given that
`spm_mtcmos_ctrl_cpu1..cpu7` covers only the A53 clusters. Android's log
answers it:

```
[Power/dcm] dcm_mcusys_mp2_sync_dcm(1)
[Power/cpufreq] MT_CPU_DVFS_B freq = 845000
[CPUHVFS] (0) [0018f295] cluster2 on,  pause = 0x0, swctrl = 0x25f0 (0x7b922)
[CPUHVFS] (0) [00192a5a] cluster2 off, pause = 0x0, swctrl = 0x55f0 (0x7b955)
```

driven by `hps_main`, with `/proc/cpufreq/enable_cpuhvfs = 1`,
`idvfs_mode = 1`, and a firmware blob at `/sys/kernel/debug/cpuhvfs/dvfsp_fw`.
**The A72 cluster is powered by a DVFS co-processor running MediaTek firmware,
not by a register sequence the kernel writes.** That is a materially harder
dependency than "transcribe the MTCMOS steps", and it should be established
before any more effort goes into an MTCMOS port. cluster2 is also cycled on and
off several times a second at idle, so it is a hotplug target, not a
"bring it up once" target.

**4. THE DA9214 EXISTS. B-43's retraction is itself retracted.** Android:

```
/sys/bus/i2c/devices/6-0068/name    = vproc_buck
/sys/bus/i2c/devices/6-0068/driver -> ../../bus/i2c/drivers/da9214
/proc/device-tree/soc/i2c@1100e000/vproc_buck@68/compatible = mediatek,vproc_buck
```

A DA9214 is on i2c6 at 0x68 on this board and the vendor driver is bound to it
right now. So the original claim was right, B-43's "measured absent" was wrong,
and the `Unsupported device id = 0x0` that produced it is a fact about how
mainline's `da9211` reaches the part — it uses a paged regmap — not about
whether the part is there. Note also that i2c6 had only just been enabled in
`#34`, so that probe was the first traffic ever put on that bus by our kernel.
**Do not re-retract either way without reading the chip through the vendor's
own access sequence.**

**5. The GPU rail is not settled either.** i2c7 carries **two** regulators, not
one: `rt5735@1c` (`rt,rt5735-regulator`, with an `mtk_gpuregulator_intf`
attribute) and `vgpu_buck@60` (`mediatek,vgpu_buck`). B-43 concluded from
i2c7:0x1c alone that it "is VGPU". There is a separate node actually named
vgpu_buck at 0x60. Which supplies what is open.

**6. The vendor DTB's CPU nodes carry no `clock-frequency` at all** — the
1391/1950/2288 MHz figures in the table below came from `mt6797.dts` in the
ubports tree, which is not this board's shipped DTB. Treat them as
reference-platform numbers.

---

**Opened 2026-08-21.** MT6797 is a **tri-cluster** Helio X20 and we are using
two thirds of it, at an unknown fixed clock.

**Measured on the device:**

```
possible: 0-9   present: 0-9   offline: 8-9
all 8 online cores report CPU part 0xd03  (Cortex-A53)
/sys/devices/system/cpu/cpu0/cpufreq       does not exist
/sys/devices/system/cpu/cpufreq/           does not exist
dmesg: "smp: Brought up 1 node, 8 CPUs"    -- no failure message for cpu8/9
```

`echo 1 > /sys/devices/system/cpu/cpu8/online` **hangs the SoC** -- the write
never returns and the watchdog reclaims the machine. That is the signature of
PSCI `CPU_ON` blocking on a core that has no power, not of a rejected call.

**What the hardware actually is, from the vendor 3.18 DT
(`arch/arm64/boot/dts/mt6797.dts`) and `mt_cpufreq.c`:**

| cluster | cores | vendor `clock-frequency` | vendor OPP range |
|---|---|---|---|
| 0 (LL) | 4 x Cortex-A53 | 1391 MHz | 1391 down to 221 MHz, 16 steps |
| 1 (L) | 4 x Cortex-A53 | 1950 MHz | 1846 MHz top (FY bin) |
| 2 (B) | **2 x Cortex-A72** | 2288 MHz | **2145 MHz** top (FY1221 bin) |

Our DT declares all ten with `enable-method = "psci"`, exactly as the vendor
does, so the missing cores are not a DT-description problem.

**Two separate gaps, and they should not be conflated:**

1. **The A72 cluster (MP2) is not powered.** The vendor's own
   `mt-smp.c`/`hotplug.c` drive per-core MTCMOS through
   `spm_mtcmos_ctrl_cpu1..cpu7` -- i.e. Linux powers the two A53 clusters'
   cores itself -- and there is no `spm_mtcmos_ctrl_cpusys2` anywhere in
   `base/power/mt6797/`. So cluster 2's power sequence lives somewhere we have
   not found yet (firmware/ATF, or `mt-plat`). Until it is found, plain PSCI
   `CPU_ON` has nothing to talk to and hangs.
2. **There is no cpufreq, for a mundane and fixable reason.**
   `CONFIG_ARM_MEDIATEK_CPUFREQ=y` is already set and the driver is present; it
   has nothing to bind to because our CPU nodes carry no
   `operating-points-v2`, no `proc-supply`, no `sram-supply` and no `clocks`.
   The full OPP and voltage tables exist in the vendor `mt_cpufreq.c` and are
   straightforwardly transcribable. This is a port, not a mystery.

**What is NOT known and should be measured before either is attempted:** the
frequency the A53s are actually running at right now. There is no cpufreq to
ask, and the mt6797 clk driver does not model ARMPLL, so `clk_summary` has no
CPU entry. Empirically one core does 173 Miter/s on a fixed integer loop
against 209 on a Raspberry Pi 4 (Cortex-A72 @1.8 GHz), consistent with roughly
1.2-1.4 GHz but not pinning it. **If LK has left the clusters at a low OPP,
DVFS is worth more than the A72s.**

**UPDATE 2026-08-21 (later): measured, and there is now a concrete path. Every
piece is mainline-supported; nothing here needs firmware.**

*What the hardware says.* Read-only, on the running machine:

```
MP0_CPUSYS_PWR_CON (0x10006210)  0x0009004D   A53 cluster, running
MP1_CPUSYS_PWR_CON (0x10006214)  0x0009004D   A53 cluster, running
MP2_CPUSYS_PWR_CON (0x10006218)  0x00010132   A72 cluster
MP2_CPU0_PWR_CON   (0x10006240)  0x00010332
MP2_CPU1_PWR_CON   (0x10006244)  0x00010332
```

MP2 decodes as `PWR_ON=0`, `PWR_ISO=1`, `PWR_RST_B=0`, `PWR_CLK_DIS=1`,
`SRAM_PDN=1` — its **reset value**. The A72 cluster is unpowered, isolated,
held in reset with its SRAM down, and nothing (kernel or firmware) has ever
touched it. **So this is not a hardware limit. We simply never power it.**

*The three things that are missing, in dependency order.*

1. **VPROC2, the cluster's core supply, does not exist in our tree.** The board
   has exactly one modelled `vproc` (1.000 V). The vendor `mt_cpufreq.c` shows
   the big cluster runs off a separate **VPROC2** rail with its own 0.80-1.18 V
   table, driven by a **Dialog DA9214** dual-buck: `DA9214_SLAVE_ADDR_WRITE
   0xD0` (7-bit **0x68**) on `da9214_BUSNUM` **6**
   (`drivers/misc/mediatek/power/mt6797/da9214.c`).
   **Mainline already supports this part** — `CONFIG_REGULATOR_DA9211` names
   DA9214 explicitly — and it is simply `not set` in our config.
2. **The I2C bus it lives on is switched off.** `i2c6: i2c@1100e000` is present
   in `mt6797.dtsi` with `status = "disabled"`, and the board DTS never enables
   it. We currently expose i2c-0..i2c-5 only. This is a one-line change plus a
   node.
3. **The MP2 MTCMOS power sequence has no implementation.** The shape is the
   standard MediaTek one (from `spm_mtcmos_ctrl_cpusys1`): `PWR_ON` ->
   `PWR_ON_2ND` -> poll status -> clear `PWR_ISO` -> release L2 -> settle ->
   `SRAM_ISOINT_B` -> clear `SRAM_CKISO` -> clear `PWR_CLK_DIS` -> set
   `PWR_RST_B`. It then needs invoking before PSCI `CPU_ON`, which is what
   currently hangs.

*Determined device-free 2026-08-21: **VPROC2 is BUCKB**, and BUCKA is the
other CPU rail.* Two independent sources agree. The vendor defines
`#define DA9214_VPROC2 0xD9` (`mt_cpufreq.c`), and mainline's register map has
`DA9211_REG_VBUCKB_A = 0xD9` (`da9211-regulator.h`). So the A72 supply is the
DA9214's **BUCKB**; **BUCKA is a separate CPU core rail and may well be the one
the running A53 clusters depend on.** Do not declare BUCKA without knowing.

*Which makes the ordering a safety property, not tidiness.* The regulator core
disables regulators nothing has claimed, in a sweep at late_initcall. A
built-in da9211 with a DA9214 node would make the first boot that has the node
also the first boot that could switch off the CPUs' own supply — taking the
panel with it, on a machine whose only display that is. Hence
`CONFIG_REGULATOR_DA9211=m` (`configs/gemini-cpu-regulator.config`): as a
module nothing binds until `modprobe da9211`, after the sweep has already run,
so identifying the two bucks is a deliberate observed step rather than
something that happens during boot while nobody is watching.

*Unresolved, and the reason the probe still comes first:* the board has **two**
candidate VPROC providers. `mt6797-gemini-pda.dts` carries an RT5735 at
i2c7:0x1c also named `vproc` (currently `status = "disabled"`), while the
live `vproc` consumers see a `regulator-fixed` called `vproc_fixed` asserting a
flat 1.000 V — a device-tree fiction with no control behind it, which is part of
why there is no DVFS. Whether the A53s actually run off the RT5735 or off the
DA9214's BUCKA is not established, and reading BUCKA's enable bit and voltage
settles it in one step.

*A trap that would have cost a day.* **`drivers/misc/mediatek/base/power/mt6797/mt_spm_cpu.h`
is stale — it was copied from an earlier SoC and never updated**, and
`spm_mtcmos_ctrl_cpusys1()` operates on its addresses. Proven by reading the
hardware: that header puts `PWR_STATUS` at SPM+0x60c and `CA15_CPUTOP_PWR_CON`
at SPM+0x2b0, and on this machine SPM+0x60c reads **0x00000000** (a status
register that reads zero is not a status register) while SPM+0x2b8 and
SPM+0x208 are zero too. The live registers are the ones in
`mt_spm_reg_mt6797.h`. **Implement against those addresses only, and do not
trust the vendor cpusys1 code's register names.**

**The read-only measurement to take FIRST, before booting anything.** The SPM
exposes CPU power state directly, and the vendor register map for it is in
`drivers/misc/mediatek/base/power/include/spm_v2/mt_spm_reg_mt6797.h`
(SPM_BASE = `0x10006000`):

| register | address | what it tells us |
|---|---|---|
| `CPU_PWR_STATUS` | `0x10006188` | which CPU cores/clusters are powered |
| `CPU_PWR_STATUS_2ND` | `0x1000618C` | the second-source copy |
| `MP0_CPUSYS_PWR_CON` | `0x10006210` | cluster 0 — known good, the reference |
| `MP2_CPUSYS_PWR_CON` | `0x10006218` | **cluster 2 (A72)** |
| `MP2_CPU0_PWR_CON` | `0x10006240` | A72 core 0 |
| `MP2_CPU1_PWR_CON` | `0x10006244` | A72 core 1 |

Reading those against the MP0 equivalents says whether cluster 2 is unpowered
or powered-but-not-released, with no reboot and no risk. Do it before the
Android trip, not after.

**Where the power sequence is NOT.** `MP2_CPUTOP_PWR_CON` and friends are
defined in the vendor SPM header, but **no `.c` file in the vendor Linux tree
writes them** — `spm_mtcmos_ctrl_cpu1..cpu7` covers only the two A53 clusters'
cores. So cluster 2's bring-up is very likely in ATF/firmware behind PSCI,
which would mean the A72s are not reachable by a kernel-side change alone. That
is a hypothesis from absence of evidence, and the register reads above are what
would turn it into a fact.

**The instrument nobody has used: boot1.** The device carries stock rooted
Android on p22, which is software-selectable, and it drives all three clusters
with the vendor stack every day. Reading `/sys/devices/system/cpu/{present,online}`
and the `cpufreq` tree there answers "is this a hardware limit or are we simply
not driving the hardware" in about a minute, for both gaps at once. That is the
next step, and it is the owner's own suggestion.

## ⚫ B-46 — RETRACTED THE SAME DAY: the camera was lying, not the display

**Opened and withdrawn 2026-08-22.** I filed this as "sway renders a perfect
desktop and none of it reaches the panel", with a black-versus-white photograph
pair as proof. **The display was fine the whole time. The instrument was not.**

### What was actually wrong

`gemini-eyes.py` left the C920e in Aperture Priority. Pointed at a lit phone
panel in a dark room the camera exposes for the *room*, runs
`exposure_time_absolute` up to ~412, and blows the panel to a featureless pale
cyan rectangle. A black screen and a white screen photograph **identically**
that way — an LCD's black leakage is itself blue-cyan, and white saturates to
the same wash. So the "test that can fail" could not, in fact, fail.

What survives auto-exposure is saturated *hue*. Re-run with a red background
and the panel photographs red; with green, green. Pin the exposure to 320 and
the whole desktop appears — swaybar, its clock, a `foot` window and its shell
prompt, over the background colour actually set.

Corroborating hardware evidence, taken before the photograph was re-shot:

```
OVL0_EN        0 (X11)  ->  1 (sway)
OVL0_SRC_CON   0        ->  1
RDMA0_GLOBAL   0x100    ->  0x101
OVL0 frame-done IRQs    ->  161 in 3 s  (~54 Hz)
OVL0_L0_ADDR   alternates 0xFC000000 / 0xFD000000 — sway's two buffers
```

Page flips were landing at frame rate the entire time this entry claimed they
were not.

### What is true, and worth keeping

- **sway on the internal panel works**, both launched by hand and as the
  sddm session. `rootfs-files/desktop/` is installed, not staged.
- **X11 really does keep the DDP pipeline disabled**: `OVL0_EN = 0`,
  `SRC_CON = 0`, `RDMA_ENGINE_EN` clear, zero frame-done interrupts, and yet a
  visible desktop. That is `modesetting` with `AccelMethod "none"` writing into
  an inherited scanout and never flipping, and it is worth remembering the next
  time those registers are read as a health check — under X they are not one.
- **B-38's sway results were still never verified against the glass.** That part
  of this entry stands; the retracted "solid orange rectangle" was almost
  certainly the same exposure fault, one session earlier.

### The lesson, which is the previous session's lesson pointed at myself

The last two entries here were about evidence that cannot carry the claim made
of it. This one is the same failure committed by the person writing that
sentence: I designed a discriminating test, got a null result, and filed a
blocker and a tracker issue without ever asking whether the *instrument* could
distinguish the two cases. It could not. A camera on auto-exposure answers "is
the panel showing a saturated colour" while appearing to answer "is the panel
showing the desktop".

`gemini-eyes.py` now pins exposure by default and says why in the source.
**Before believing a measurement, check that the instrument can tell the two
answers apart** — not just that the experiment can.

## 🟢 B-39 — trigger 1 RESOLVED 2026-08-21: a GPU rate change was reprogramming the SoC's main PLL

**Trigger 1 is closed on `#43`. Trigger 2 (the USB display) is untouched and
stays with #27.** Every earlier attribution in this entry — including the
night update's "the trigger is a new client surface arriving while the GPU is
already rendering" — is **superseded**. That description was accurate as an
observation and wrong as a cause.

### What it actually was

`CLK_TOP_MUX_MFG` is declared with `MUX_GATE()`, which sets
`CLK_SET_RATE_PARENT` and leaves reparenting enabled. Two of its four parents,
`syspll_d3` and `univpll_d3`, are `FACTOR()` children of **mainpll** and
**univpll** — the PLLs behind the AXI bus, MSDC, I2C and the UARTs — and
`FACTOR()` also carries `CLK_SET_RATE_PARENT`. So `clk_mux_determine_rate_flags()`
is entitled to satisfy a GPU operating point by **switching the GPU onto a
system PLL and reprogramming that PLL for the whole die**.

Read from the hardware, not from the framework's bookkeeping — requesting the
610 MHz GPU OPP on `#42`:

```
CLK_CFG_5 (0x10000050)  0x01818100 -> 0x02818100   mfg mux 1 (mfgpll_ck) -> 2
MFGPLL_CON0 (0x1000c240) 0x00000101 -> 0x00000000  the GPU PLL switched OFF
MFGPLL_CON1 (0x1000c244) unchanged at the 520 MHz value
```

while `devfreq/cur_freq` and `clk_summary` both reported 610 MHz. The next rate
change from that state hard-locked CPUs and timed out every unrelated bus
master in turn. That is the whole of trigger 1.

### The experiments, in the order that mattered

The reproducer is `tools/gpuwedge.c` — plain EGL/GBM clients on
`/dev/dri/renderD128`, no compositor, no KMS, no `card0`. `scripts/gemini-gpuwedge.sh`
runs one experiment from a fixed state and decides from netconsole.

| # | what | result |
|---|---|---|
| A | steady hog + 208 new contexts, 90 s | **survived** |
| A1 | 32 simultaneous contexts, round-robin, 25 s | **survived** |
| A2 | bursty hog alone (domain cycling), 120 s | **survived** |
| A3 | **bursty** hog + new contexts | **wedged at poke 5, and again at poke 18** |
| C1 | A3 with the MFG domain pinned on (`power/control=on`) | **wedged** — `_set_opp_voltage … -110` |
| D1 | A3 with devfreq pinned (no OPP transitions) | **survived 120 s, 394 pokes** |
| F1 | OPP table swept from a shell, **GPU completely idle, no GL client** | **wedged** |
| I1 | the whole OPP **voltage** range on the vgpu rail, clock pinned | **survived** — the rail is innocent |
| K1 | 60 transitions between 238 and 365 MHz | **survived** — not the count |
| L1 | climb reading the PLL **hardware** at each step | **named it** (table above) |

F1 is the one that reframed everything: **the GPU need not be running at all.**
Sweeping the OPP table by hand kills the machine, so trigger 1 was never a
panfrost job-fault bug.

### Three earlier readings, retracted

1. **"a new client surface arriving while the GPU is already rendering".** Real,
   reproducible, and not the cause. A new client raised GPU utilisation past
   `simple_ondemand`'s 45% upthreshold, which produced an OPP change. `glxgears`
   alone is vsync-limited and never crosses that threshold, which is why it
   "ran 90 s at full load" harmlessly — it was not at full load.
2. **`HW_ISSUE_9435` and the soft-stop path.** A good lead from the vendor's
   workaround list, and unrelated. panfrost's reset path was never reached.
3. **The `-110` regulator timeout, the touchscreen's I2C timeout, the msdc
   timeouts, `DATA_INVALID_FAULT`.** All victims of the interconnect already
   being wrecked. `DATA_INVALID_FAULT` is **not** independently explained and
   has not been seen since the fix; it should not be treated as closed by this
   entry.

### The fix

`patches/v6.6/gpu/0005-clk-mediatek-mt6797-mfg-mux-no-reparent.patch` —
`CLK_SET_RATE_NO_REPARENT` on `mfg_sel`, so a rate request stays on the current
parent and reaches MFGPLL, the only PLL a GPU OPP has any business moving.
Upstreamable as-is; it is an SoC bug, not a board bug (relevant to #40).

On `#43` the whole 238–780 MHz table now lands on MFGPLL, and the PCW the
hardware ends up holding matches the value the OPP arithmetic predicts at every
step (780 MHz → `CON1 = 0x810F0000` → exactly 780000000):

- 2800 consecutive OPP transitions with the GPU idle: **clean**.
- 180 s of the workload that used to kill the machine in 2–5 s: **clean**.
- 20 qterminal open/close cycles under GPU-composited sway with glxgears
  redrawing through Xwayland, GPU active 20/20 samples, devfreq reaching
  **780 MHz** — **twice, each from a fresh boot**.

### What this cost, and the lesson

Every measurement in the night update was honest and none of it could reach the
cause, because all of it was taken through a compositor. The first experiment
that removed the compositor also removed the GPU, and that is what found it.
**When a symptom has a long causal tail, instrument the earliest thing you can
still make fail — not the thing that fails loudest.**

A second lesson, cheaper: `clk_summary` reads every clock's hardware enable
bit, including `mfg_bg3d`, which lives in the MFG power domain. Reading it with
that domain asleep is the B-36 trap by another door, and it wedged the machine
once during this session.

## 🟡 B-39 (historical) — TWO independent ways to hard-wedge the SoC

**Opened 2026-08-21 and rewritten three times the same day. Read the update
first; the earlier attributions are retracted at the bottom.**

### UPDATE 2026-08-21 (night): the trigger is precise, and three hypotheses are dead

**The trigger is a new client surface arriving while the GPU is already
rendering** — not GPU load, not Qt, not Xwayland, not "a real window".

| what | GPU | result |
|---|---|---|
| `glxgears`, 90 s continuous | busy | **survives** |
| foot / Qt-on-Wayland / xclock / qterminal, opened with the GPU idle | idle | **all survive** |
| **any** new window opened while `glxgears` renders | busy | **wedges, twice, in seconds** |

That retires the "qterminal is fatal, foot is safe" framing: the difference was
never the client, it was whether anything else was already on the GPU.

**Refuted, in order:**

1. **The display's IOMMU.** B-44 is fixed; this still wedges.
2. **The MFG async-bridge timing register.** The vendor ORs
   `max_freq >= 780000 ? 0xa : 0x5` into `MFG + 0x1c` at every GPU power-on
   (`mtk_config_platform.c`); ours reads `0x00000000`. Note our GPU clock is
   500.5 MHz, so the vendor value here is **0x5** — the `0xa` recorded in
   `STATE-2026-08-21-evening.md` is the >=780 MHz branch and is wrong for this
   machine. Written live with devmem while the domain was up, verified as
   `0x00000005`, wedged on the next window anyway.
3. **`panfrost_device_reset()` cycling the MFG power domain.** It does not:
   soft-reset, power on cores, MMU reset, unmask interrupts. The domain is only
   cycled by runtime suspend/resume.

**The strongest remaining lead: panfrost applies 3 of the 15 workarounds the
vendor applies for this exact GPU revision.** t880 r1p0 maps in the vendor's
kbase to `base_hw_issues_t86x_r1p0`, 15 entries; panfrost's `hw_issues_t880`
has three, and only one of those (`T76X_3953`) is referenced anywhere in
panfrost's C code. The interesting absentee is **`HW_ISSUE_9435`** — *"Compute
endpoint has a 4-deep queue of tasks, meaning a soft stop won't complete until
all 4 tasks have completed"* — against `panfrost_reset()`, which soft-stops
every slot, waits **10 ms**, prints `Soft-stop failed`, and issues
`GPU_CMD_SOFT_RESET` regardless. Resetting a Mali with tasks still running and
AXI transactions outstanding is a plausible way to leave the interconnect
holding one, which is exactly the aftermath.

Stated against my own lead: in the one complete capture, `gpu sched timeout` was
followed by `js fault DATA_INVALID_FAULT` with **no** `Soft-stop failed` between
them, so that soft-stop did complete inside 10 ms. 9435 is a lead, not a cause.

`HW_ISSUE_8408` is worth reading too — *"Repeatedly Soft-stopping a job chain
(Vertex Shader, Cache Flush, Tiler) causes DATA_INVALID_FAULT on tiler job"* —
which says the `DATA_INVALID_FAULT` is a **consequence of the timeout handling**
rather than the original problem. The first event is a js=0 job that missed its
500 ms deadline.

**What this bug actually needs is a serial console.** netconsole dies with the
machine and its last line is always about a second stale — the touchscreen's
I2C timeout, which is the first victim to notice, not the cause. B-1's FTDI
adapter is not in the rig.

### UPDATE 2026-08-21 (after B-44): "both silent" is no longer true, and the
### IOMMU was not the cause

Retested on `#40`/`#41` — the kernels in which the display finally runs behind
the M4U — because B-44 raised the possibility that trigger 1 was the missing
IOMMU. **It was not.** Trigger 1 still wedges. But it is no longer silent, and
what it says changes the diagnosis completely.

*The isolating experiment, run twice in each direction.* Identical workload —
sway on card0, one terminal in a `while :; do ls -la /usr; done` loop, then
repeated qterminal open/close cycles — differing only in the renderer:

| renderer | GPU state | result |
|---|---|---|
| `WLR_RENDERER=gles2` + `WLR_RENDER_DRM_DEVICE=renderD128` | `active` | **2 wedges in 6 runs** |
| `WLR_RENDERER=pixman` | never leaves `suspended` | **0 wedges**, 12+1 cycles, twice |

So the compositor, Wayland, the modeset, the panel and the eMMC-heavy workload
are all cleared. **Panfrost is necessary.**

*What the wedge actually looks like, captured for the first time.* Three
separate signatures, all on netconsole, all new:

```
panfrost 13040000.gpu: gpu sched timeout, js=0, config=0x3b01, status=0x8,
                       head=0xa141640, tail=0xa141640
panfrost 13040000.gpu: Panfrost Dump: BO has no sgt, cannot dump
panfrost 13040000.gpu: js fault, js=1, status=DATA_INVALID_FAULT, head=0xa147600
        ...15 s later...
mtk-msdc 11240000.mmc: msdc_request_timeout: aborting cmd=52
```

and, in another run:

```
gemini-nt36xxx 4-0062: page select failed: -110      (I2C, touchscreen)
mtk-msdc 11240000.mmc: msdc_request_timeout ... cmd=52   (SDIO)
mtk-msdc 11230000.mmc: msdc_request_timeout ... cmd=25   (eMMC write)
```

and, in a third:

```
rcu: INFO: rcu_preempt detected stalls on CPUs/tasks:
rcu:  1-...!  2-...!  7-...!   (1 GPs behind)
rcu:  (detected by 4, t=5256 jiffies, g=26217, q=324 ncpus=8)
Task dump for CPU 1: task:ls  state:R running task
```

**Read those together and the shape is clear: a panfrost job faults, and then
every unrelated bus master on the SoC times out in turn** — I2C, SDIO, eMMC —
while three of eight CPUs stop making progress. That is not a display bug and
not a memory-bandwidth problem. It is the GPU's fault-and-reset path taking
something the whole system needs. `rmmod panfrost` oopsing during a fault loop,
recorded earlier, is very likely the same thing from the other side.

**The next step is therefore panfrost's reset path, not the display.** Two
concrete questions, in order: (1) what is `DATA_INVALID_FAULT` on js=1 — a Mesa
bug on Midgard, or an address panfrost handed the GPU that the M4U-free Mali MMU
cannot reach; (2) does `panfrost_device_reset()` power-cycle the MFG domain with
transactions outstanding, given this SoC's bespoke VGPU_SRAM LDO handling
(`pmdomain/0007`).

**Two process notes.** The reason all of this appeared at once is that
`configs/gemini-lockup-detect.config` was added the same day: this kernel had
**no** softlockup, hardlockup, hung-task or workqueue watchdog compiled in, so
"netconsole captures nothing" was partly a statement about the kernel's
configuration rather than about the failure. And the hardware watchdog's 30 s
is shorter than the default 20 s softlockup threshold plus its reporting
latency, so `kernel.watchdog_thresh=5` after each boot is what gets a report out
before the machine is reclaimed.

**`mtk_gem_prime_import_sg_table: sg_table is not contiguous` no longer appears
at all** — the IOMMU removed it, as designed. It was always described here as a
lead rather than a proof, and it is now closed as a non-cause.

---

**The original entry follows.**

**The signature, common to every occurrence (~9 times in one session):** the
machine goes away instantly, SSH resets mid-command, `/sys/fs/pstore` is empty
afterwards, and netconsole — armed and listening every time — captures
**nothing at the moment of death**. The hardware watchdog reclaimed it
unattended on every single occasion, which is the one genuinely good news here.

### Trigger 1 — a real window under sway, with no USB display involved

sway started with `WLR_DRM_DEVICES=/dev/dri/card0`, so it never opens the udl
card at all; the adapter stays bound and idle. Backgrounds alone run happily for
minutes and the cursor moves at 3 CPU ticks per 400 warps. **Open one
`qterminal` and the machine dies.** Also reproduced with `wl-mirror`, and with
`qterminal` on either output when both were active. Four-plus occurrences.

In the runs where netconsole caught anything at all, the last line was:

```
[drm:mtk_gem_prime_import_sg_table] *ERROR* sg_table is not contiguous
```

which is what a direct-scanout attempt on a panfrost-rendered client buffer
would produce — see the IOMMU section below. It also appears in runs that
survive, so it is a lead, not a proof.

### Trigger 2 — sustained full-screen updates on the USB display

`Xorg` running on the **udl device alone** (`Option "kmsdev" "/dev/dri/card2"`)
— no compositor, no window manager, no panfrost, no cross-device buffer
sharing whatsoever:

| on the udl X screen | result |
|---|---|
| `xclock` | fine |
| `qterminal`, a real window (screenshotted) | fine |
| a full LXQt session | **wedges**, twice |
| **30 x `xsetroot -solid`** — nothing but full-screen 1920x1080 fills | **wedges** |

`xsetroot` is a two-line X client. Nothing is in that test but the X server
repainting 1920x1080 and `udl` pushing it over USB. Small, incremental updates
are fine; large or sustained ones are not.

**These are not the same bug and must not be merged.** Trigger 1 needs
panfrost and mediatek-drm and no USB display; Trigger 2 needs the USB display
and neither panfrost nor a compositor.

### What does NOT wedge

The ordinary LXQt/X11 desktop on the internal panel — the machine's daily
configuration — runs for hours. That is because modesetting with
`AccelMethod "none"` memcpys damaged regions into the scanout buffer and
essentially never page-flips: measured **zero** display interrupts across 20
full-screen `xsetroot` repaints on the panel. It is slow, and it is stable, and
those two facts have the same cause.

### Where Trigger 2 most likely belongs: ADR-0004's own gate

The ADR made "sustained, robust USB 2.0 **high-speed bulk**" the precondition
for any USB display and named both gaps: the right-port MUSB host is
**PIO-only with single-buffered 512 B FIFOs**, ~43-64 Mbit/s measured, and **a
misbehaving device wedges the host until reboot** (issue #27). A 1920x1080x32
frame is ~66 Mbit, so a full-screen repaint is about a second of saturated
bulk-OUT on a host with no DMA — the exact workload the ADR said had to be
proven, and the exact failure it predicted. **#27 has been reopened**: it was
closed against a much lighter workload than a display imposes.

### The IOMMU finding, which stands on its own

Relevant to Trigger 1 and to Track 2 generally; not claimed as proven cause of
either wedge.

```c
/* mtk_drm_gem.c, mtk_gem_prime_import_sg_table() */
if (drm_prime_get_contiguous_size(sg) < attach->dmabuf->size) {
        DRM_ERROR("sg_table is not contiguous");
        return ERR_PTR(-EINVAL);
}
```

`mt6797.dtsi` has **no `m4u` node and zero `iommus` properties**, and
`CONFIG_MTK_IOMMU=y` does nothing because **mainline `mtk_iommu.c` has no
mt6797 support at all** — no compatible entry, no
`dt-bindings/memory/mt6797-*.h`. The vendor 3.18 tree does have the node. So
mediatek-drm can only scan out physically contiguous memory, panfrost never
allocates any, and every pixel the Mali renders reaches the panel through a CPU
copy. That is a real ceiling on Track 2 whatever the wedges turn out to be.

### Retractions, in order

1. **First version:** "a real client surface under sway", with the missing
   IOMMU named as the structural cause. Drawn from the sway cases alone before
   any isolating experiment. Overstated: it did not know about Trigger 2.
2. **Second version:** claimed the `xsetroot` result "removes sway, Wayland,
   LXQt, panfrost and the IOMMU from the list of required conditions". True for
   Trigger 2 **only**. It was written before sway was tested with
   `WLR_DRM_DEVICES=/dev/dri/card0`, which then wedged with no USB display in
   play at all and showed there are two separate problems.

Both were cases of generalising from whichever half had been measured most
recently. The lesson is the ordinary one: an isolating experiment tells you
about the path it isolated, and says nothing about the path it removed.

## 🟡 B-38 — X11 structurally cannot use this GPU; Wayland can, and does

**Opened 2026-08-21, after B-34.** The Mali renders now, and the desktop still
does not use it. This is the second half of #36 and it is not a tuning problem.

**The structural fact.** The two DRM devices have disjoint capabilities:

```
card0 -> mediatek-drm   CRTC + DSI connector, and NO render node
card1 -> panfrost       renderD128, and NO CRTC and NO connector
```

Xorg's only bridge between "the device that scans out" and "the device that
renders" is glamor, and glamor binds EGL to the *display* device's fd. That fd
has no render capability, so glamor cannot initialise and `AccelMethod "none"`
is not a choice we made, it is the only thing that works. Confirmed live:
`xrandr --listproviders` reports **one** provider with `cap: 0x2` (Sink Output
only — no Source Offload), and `DRI_PRIME=1 glxinfo -B` reports `llvmpipe`
exactly like the unprefixed one. There is no X configuration that fixes this.

**Wayland does it correctly, measured on hardware the same day.** wlroots takes
the two devices separately — but it has to be told, because its autodetection
looks for a render node on the KMS device and finds none:

```
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 sway
  [wlr] Opening DRM render node '/dev/dri/renderD128' from WLR_RENDER_DRM_DEVICE
  [wlr] GL renderer: Mali-T880 (Panfrost)
  [wlr] Using DRM node /dev/dri/card0            (GBM/scanout)
```

Without that variable sway comes up on the software renderer and the GPU stays
`runtime_status: suspended` — which looks like success if you only photograph
the screen.

**What it is worth, same workload as B-37's measurement:**

| 400 pointer moves | wall time | compositor CPU ticks |
|---|---|---|
| Xorg, `SWcursor`, `Rotate left` | 5 s | **1021** (~two cores) |
| sway, GPU compositing | 44 ms | **3** |

**CORRECTION 2026-08-21: "correctly oriented" was never evidence and is
retracted — `transform 90` is 180 degrees wrong.** The photograph offered for
it, `04-docs/captures/sway-orange.jpg`, is a **solid orange rectangle**. A
uniform colour cannot show an orientation, so the claim was not supported by
the thing cited for it, and it stood for a day until the owner looked at the
panel and said the desktop was upside down.

Settled properly with two tiled terminals printing `LEFT` and `RIGHT`,
photographed both ways on the glass: at `transform 90` the text is upside down
and `LEFT` is on the right-hand side of the panel; at `transform 270` the text
is upright and `LEFT` is on the left. sway's transform turns the output the
opposite way from Xorg's `Rotate left`, so the two differ by 180 degrees.
`scripts/gemini-desktop-setup.sh` now writes **270**.

Worth recording how it was nearly not found: the webcam framed only the
top-left corner of the panel, so several photographs of sway showed nothing but
background and settled nothing. It took the owner re-aiming the camera to frame
the whole screen before the instrument could answer at all. **The photograph is
the honest instrument only if what is photographed can carry the answer** — a
solid colour cannot, and neither can a crop that misses the content.

Photographed on the glass: full-screen, `output DSI-1
transform 90` (`scratchpad/sway-orange.jpg`). Keyboard and the Novatek
touchscreen both enumerate under sway with no configuration.

Note sway does **not** use the hardware cursor plane either — plane-3 stays at
`crtc-pos=0x0+0+0` and `DISP_REG_OVL_OFFSET(3)` stays `0`. It composites the
cursor and it costs 3 ticks, because the compositing is on the GPU. So B-37's
CPU cost is really a symptom of software compositing, not of the cursor plane.

**Not switching the desktop.** The owner has a working LXQt/X11 session and
that is their call, not this session's. What a switch would still need:
`Xwayland` is **not installed** (sway logs `Cannot find Xwayland binary`), so
every existing X application — including all of LXQt — has nowhere to run yet;
screen blanking, backlight, the Bluetooth mouse and session management would all
need re-proving under the new stack.

**Opened 2026-08-21.**

## 🟡 B-37 — the hardware cursor plane is drawn once and never moves

**Update 2026-08-21 (later): `Atomic "on"` does NOT fix it. The hypothesis
below is refuted, and the experiment could not have worked.** With
`Option "Atomic" "on"` and `SWcursor` removed, the pointer was warped to two
corners and the panel photographed each time: the arrow is in the *identical*
place in both photographs. `scratchpad/b37-atomic-A.jpg`, `-B.jpg`.

The reason the experiment was not a real A/B: with `Atomic "on"`, Xorg still
issues **`DRM_IOCTL_MODE_CURSOR`**. Confirmed by turning on `drm.debug` and
capturing one warp — the ioctl is the legacy cursor path either way, so both
DDX settings land in the same kernel code. The DDX option was never the
variable it was assumed to be.

**What is actually broken — two separate defects in mediatek-drm, measured:**

1. **The latch is gated on a vblank interrupt that is off whenever nothing is
   page-flipping.** mt6797 has `shadow_register = false` and no CMDQ channel,
   so `mtk_crtc_ddp_config()` — the only thing that writes plane registers —
   runs *only* from `mtk_crtc_ddp_irq()`. Measured: `1400b000.disp-ovl0` in
   `/proc/interrupts` does not advance by a single count in 5 s of idle
   desktop, and does not advance across 20 `xsetroot` repaints either
   (modesetting with `AccelMethod "none"` memcpys into the scanout buffer, so
   the panel updates with no DRM traffic at all). Start a client that keeps
   vblank alive — `glxgears` — and the IRQ runs at ~9/s.
2. **Even with vblanks flowing, the position written to hardware stops
   changing.** With `glxgears` running the cursor moved exactly **once**, to
   the position that was pending, and then froze there. Read directly from the
   OVL: `DISP_REG_OVL_OFFSET(3)` at `0x1400b09c` holds `0x0707031F`
   (y 1799, x 799) across every subsequent warp, while
   `/sys/kernel/debug/dri/0/state` tracks the pointer perfectly
   (`crtc-pos=64x64+899+1899`, `+199+99`, …). So the commit reaches DRM
   software state and never reaches the register.

   The panel-coordinate mapping, confirmed against several warps under
   `Rotate left`: `panel_x = y - 1`, `panel_y = 2160 - x - 61`.

   Suspected but **not measured**: `mtk_plane_atomic_async_check()` calls
   `drm_atomic_helper_check_plane_state(plane->state, …)` — the *current*
   state, not the new one — so `new_state->dst` is never recomputed, and
   `mtk_plane_update_new_state()` reads `pending.x/y` straight out of
   `new_state->dst`. That would write a stale position forever. Saying this is
   the cause would be a guess; the evidence above is not.

**So `SWcursor` stays**, and the CPU cost stays with it, until one of those two
is fixed in the kernel. This is #20's, not a DDX tuning problem, and the fix
cannot live in userspace.

**Opened 2026-08-21.** The pointer on the glass had not moved since login, and
every layer underneath was working: the mouse delivers `REL_X`/`REL_Y`,
libinput passes them, X's pointer position tracks exactly, clicks land where
the pointer really is. Only the drawn arrow is stale. It presents to a user as
"the mouse does nothing" and is not an input problem at all.

**Measurement.** Warp the X pointer with `xdotool mousemove`, photograph the
panel, repeat at a second position:

| X reports | drawn cursor |
|---|---|
| `x:200 y:150` | centre of screen |
| `x:1900 y:950` | centre of screen, unmoved |

Both on a **freshly started X server** with nothing reading the input devices.
The primary plane is fine throughout — the panel clock ticks between
photographs. Xorg logs `Silken mouse enabled`, i.e. it believes it has a
working hardware cursor: `drmModeSetCursor` draws the pointer once at the
initial centre position and `drmModeMoveCursor` is then ignored.

**Worked around, not fixed:** `Option "SWcursor" "true"` in the OutputClass
(`scripts/gemini-desktop-setup.sh`). Verified by the same warp-and-photograph
method.

**The workaround is expensive.** Measured Xorg CPU: ~0% idle, and **1021 ticks
over 5 s for 400 pointer moves — about two cores' worth, ~25 ms of CPU per
move.** (Upper bound: `xdotool` warps jump randomly across the whole screen and
maximise the damaged area.) It is costly *because of rotation* — with
`Rotate left` every cursor move damages a region that must be rotated in
software and flushed. This is a large part of the desktop's perceived lag.

**Untested hypothesis, and the reason this is really #20's.** `Option "Atomic"
"off"` is set, so modesetting uses the legacy `drmModeMoveCursor` path rather
than an atomic commit. If mediatek-drm only latches cursor-plane position as
part of a commit something else triggers, the cursor would track while the
session is actively painting and freeze once the desktop goes idle. **That fits
the owner's observation that it tracked when the Bluetooth mouse was first
connected and stopped later**, which nothing else here explains — a fresh X
server does not restore it.

**The one-build experiment:** set `Atomic "on"`, keep the hardware cursor,
repeat warp-and-photograph. Tracks under atomic and not legacy → the legacy
cursor path on this driver is broken and SWcursor is the right permanent answer
for the internal panel. Fails under both → the plane never latches at all.

**Method note.** X screenshots cannot see this bug: the hardware cursor is not
in the framebuffer, so `import -window root` shows the cursor at the correct
position or not at all. The webcam is the only honest instrument — the same
lesson as "screenshots cannot judge rotation", for the same reason.

## 🔴 B-32 — the build ignored its own config fragments for three days

**Opened and fixed 2026-08-21.** `07-kernel/build-local/src/.config` was seeded
once from CI artifact `235e1e8` (2026-08-17) and never re-merged. The Bluetooth
fragment landed at `4c22b9d` and the GPU MFGCFG fragment at `aaa17c6`, both
after it. So from 2026-08-18 onward, **adding a symbol to `configs/*.config`
changed nothing about anything flashed to the device**, silently:

- `CONFIG_COMMON_CLK_MT6797_MFGCFG` was off, so the GPU node's
  `clocks = <&mfgcfg CLK_MFGCFG_BG3D>` had no provider and **panfrost could not
  bind at all** — while the tracker recorded the GPU as "binds, shader cores
  never power up". The issue was describing a build nobody had flashed for days.
- `CONFIG_UHID` was off. BlueZ needs uhid to expose an LE HID peripheral as an
  input device, so a Bluetooth mouse would have paired and produced no events.
- `CONFIG_BT_LE` and `CONFIG_BT_HCIVHCI` were off too.

**Fix:** `scripts/gemini-kbuild.sh` re-merges every fragment on every build and
then asserts that each symbol a fragment asked for survived `merge_config` and
`olddefconfig`. A dropped symbol is a hard error. It found one on its first run:
`CONFIG_PSTORE_FTRACE`, which depends on `FUNCTION_TRACER` and had therefore
been discarded on every build since the fragment was written.

**The general shape, worth carrying:** a build that ignores its configuration is
worse than a build that fails, because the tracker keeps describing a machine
that does not exist.

## 🔴 B-33 — the patch series could not rebuild the kernel we flash

**Opened and fixed 2026-08-21.** Applying every patch in `patches/v6.6/` to a
pristine v6.6 and diffing against `build-local/src` — apparently for the first
time — showed they were not the same tree:

- `input/0003` created the touchscreen driver's **first** version and never
  added its Kconfig or Makefile entries, so a kernel built from the series
  compiled the file it had just created into nothing. Every change after that
  (ready-line gating, the paged-read fix that resolved B-31, the panel-follower
  work) lived in a loose copy at `patches/v6.6/input/gemini-nt36xxx.c` that
  nothing applies.
- The touchscreen's **device-tree node was in no patch at all**. It existed only
  as a hand-edit to the build tree.

Recreating the build tree from the patch series would have silently deleted the
touchscreen — the subsystem that took the longest to get working.

**Fix:** `input/0003` regenerated as the source of truth and wiring the driver
into the build; the loose copy deleted; `dts/0038` adds the touch node.
`scripts/gemini-patch-check.sh` asks the question on demand and reports
IDENTICAL.

**Trap it encodes:** the baseline must come from `git archive`, not `cp -a`.
`07-kernel/linux-6.6` has its own uncommitted state, so copying the working tree
gives a baseline that is neither pristine nor patched; half the series then
applies with fuzz and the comparison means nothing.

## 🟢 B-34 — RESOLVED 2026-08-21: the GPU's SRAM LDO was never enabled

**Read this before the history below. The GPU renders.** `/root/gputest`
returns `RESULT: pixels CORRECT, renderer Panfrost (hardware)`, exit 0, with
**zero** GPU faults, repeatedly, on kernel `#31`.

**The cause is a power supply mainline does not know exists.** Beyond the
mtcmos domains and the external VGPU buck, mt6797 has an on-die LDO in
infracfg_ao that feeds the Mali's *internal SRAM*, and its reset value is off.
The vendor calls it **VGPU_SRAM** and enables it from its Mali platform
callback — `infracfg_ao + 0xFBC = 0x1FF` as the very first action of
`mtk_pm_callback_power_on()`, `0` as the very last of
`mtk_pm_callback_power_off()`
(ubports 3.18.60, `.../mali-r20p0/drivers/platform/mt6797/mtk_config_platform.c`;
`mt_gpufreq.c` names it in a comment, `/* enable: // VGPU_SRAM */`).

That explains every symptom precisely, including the ones that made this look
unexplainable. The register file is powered by something else, so the GPU looks
completely healthy — every control register reads back, `GPU_ID` and the
feature registers are right, the mtcmos ACKs land, `SHADER_READY` is `0xF`, and
Mesa brings up a GLES 3.1 context. The memories the MMU and the bus interface
keep their state in are *not* powered, so the first memory transaction comes
back as noise — which is exactly what "fault addresses full of `F`s and `D`s,
different every time" was telling us, and why chasing coherency, PTE
shareability, >4 GB page tables and MFG_ASYNC clocking found nothing. Those
four eliminations below are still correct; none of them was ever the cause.

**Measured, A/B/A/B, on one running kernel with `devmem`, before any rebuild:**

| `INFRACFG_AO + 0xFBC` | `gputest` | GPU faults |
|---|---|---|
| `0x1FF` | pixels CORRECT, Panfrost | 0 |
| `0x000` | nothing rendered | **197** |
| `0x1FF` | pixels CORRECT, Panfrost | 0 |
| `0xFF` | pixels CORRECT, Panfrost | 0 |
| `0x1` | pixels CORRECT, Panfrost | 0 |

**Bit 0 alone is sufficient.** The neighbouring trim registers the vendor also
writes (`0xFC0`/`0xFC4` = `0x0F0F0F0F`) are **not** required — restoring them to
their reset `0x09090909` changed nothing. We write the vendor's full `0x1FF`
anyway, because that is what ships.

**Landed as `pmdomain/0007`**, on the **MFG** domain rather than MFG_ASYNC: MFG
is `KEEP_DEFAULT_OFF` and so follows panfrost's runtime PM, while MFG_ASYNC is
powered at probe and would leave the LDO on forever. Verified on `#31` that the
LDO tracks the domain — `0x1FF` when `runtime_status` is `active`, `0x0` when
`suspended` — and that rendering survives suspend/resume. Panel gate PASS 13/13
on the same kernel.

**How it was found, because the method is the transferable part:** not by more
measurement on the device, but by reading the vendor's own GPU power-on
sequence in `07-kernel/ubports-3.18`, which has been sitting in this tree the
whole time, and asking what it does that we do not. It does four things:
this LDO; `MFG_write32(0x1c, ... | 0xa)` marked `/* timing */`; a TOPRGU MFG
software reset at init; and an MFG PMU enable. The first one was the answer.
"A register-level comparison against the vendor stack" was recorded as the next
step and assumed to need the Reference slot and a running GPU job. It did not.
The source was enough.

**Left over, deliberately unlanded:** `MFGCFG + 0x1c` reads `0x00000000` on our
kernel and the vendor ORs in `0xa` (or `0x5` below 780 MHz) on every power-on.
Rendering is correct without it and adding an unproven write to a working
machine is not a trade worth making — but it is frequency-dependent, so if GPU
DVFS is ever raised above the current OPP, look here first. (`MFGCFG + 0x20`
already reads `0x0000000A`, which may mean the field moved between the 3.18
register map and what LK programs; unresolved, and it does not matter today.)

## (history) B-34 — panfrost: the GPU's first read of DRAM comes back as noise

**Opened 2026-08-21.** With the MFG shader-core power domains fixed
(`pmdomain/0006`, `dts/0037`, `gpu/0004`) the GPU powers up completely —
`PWR_STATUS 0x2B003F5E`, `SHADER_READY 0xF`, `TILER_READY 0x1`,
`SHADER_PWRTRANS 0x0` — reports its full feature set, and Mesa brings up
`Mali-T880 (Panfrost)` OpenGL ES 3.1 on the GBM platform.

Submitting a job then fails:

```
panfrost 13040000.gpu: AS_ACTIVE bit stuck
panfrost 13040000.gpu: GPU Fault 0x00ff0388 (GPU_SHAREABILITY_FAULT) at 0x000000fff7fddb00
```

**Established:**

- The **first** fault arrives before `panfrost_mmu_enable()` finishes its opening
  `AS_COMMAND_FLUSH_MEM`, so it is not a page-table-content problem — it is the
  GPU's first memory transaction.
- Fault addresses vary per fault and sit near the top of the 40-bit PA space
  with long runs of `F`s and `D`s (`0xd7bfbddb00`, `0xffffffda40`,
  `0xfff7bdd300`). That is the signature of reads returning noise, not of one
  bad pointer.
- `INFRA_TOPAXI_PROTECTEN = 0x000104B8` — MFG bus-protection bit 21 is clear, so
  `pmdomain/0003`'s decision to leave `bus_prot_mask = 0` is not the cause.

**Eliminated by measurement, one build each:**

1. **`GPU_COHERENCY_ENABLE` left at its ACE-Lite reset default.**
   `COHERENCY_FEATURES` reads `0x0` and kbase writes this register on every init
   while panfrost never does, so writing `COHERENCY_NONE` looked right. The
   register is **RAZ/WI on this part**: it reads back `0x0` after the write and
   nothing changed. Reverted.
2. **Inner- vs outer-shareable PTEs.** io-pgtable's Mali path always sets
   `SH_OS`; kbase uses `SH_IS` on non-coherent systems. Switching it changed
   nothing, and could not have, since the first fault precedes any PTE use.
   Reverted rather than left in the tree unproven.
3. **Page tables above 4 GB.** DRAM is `0x40000000`-`0x13FFFFFFF`, so pgd
   allocations land above 4 GB routinely, and `AS_TRANSTAB_LO` alone reads
   `0x0930F007` — an address that is not RAM. Instrumented: `transtab=0x109564007`,
   `readback lo=09564007 hi=00000001`. **The hardware keeps the high half.** A
   `mem=3G` kernel did not help either.

4. **MFG_ASYNC starved of its main clock.** `pmdomain/0005` *replaced*
   `CLK_MFG` with `CLK_MFG52M` on MFG_ASYNC and MFG, on the reasoning that the
   vendor's PGATE pre_clk is `mfg_52m_sel` — true, but a pre_clk is what the
   vendor turns on *around* an mtcmos transition, not instead of the domain's
   ordinary feed, and `MAX_CLKS` is 3 so there was never a reason to choose.
   Gave both domains `{CLK_MFG52M, CLK_MFG}` (kernel #29): no change, 598 fault
   lines in 40 s. Reverted.

**Next places to look:** the EMI MPU's master-domain permissions for the GPU's
AXI ID — MediaTek's preloader programs DRAM region permissions per master, and
getting one master wrong denies exactly that master, which is the shape of this
failure. Testing it needs vendor EMI register knowledge and guessing at DRAM
protection registers on a working machine is a bad trade. The other route is a
register-level comparison against the vendor stack actually running a GPU job,
which is what the Reference slot exists for.

**Side finding:** `rmmod panfrost` while a reset is queued oopses in
`drm_sched_start` -> `kthread_unpark(NULL)` from `panfrost_reset_work`. Upstream
teardown race, only reachable once the GPU is in a permanent fault loop, but it
means the module is not hot-swappable while faulting. Use a reboot.

## 🟡 B-35 — ramoops does not survive a hard watchdog reset; netconsole does

**Opened 2026-08-21.** A hang left **no evidence at all**. There is no serial
console in the rig, SSH needs userspace, and `/sys/fs/pstore` was completely
empty after a real hang and watchdog reset — despite `CONFIG_PSTORE_RAM=y` and a
256 KB console zone in the `ramoops@44410000` node. The RAM contents do not
survive the reset here.

**Worked around** with `CONFIG_NETCONSOLE=y` + `scripts/gemini-netconsole.sh`,
which streams the kernel log to the host over the USB gadget as it is printed,
so the last line before a wedge is already on the host. It caught the
`mtk_drm_crtc_atomic_disable` vblank timeout (issue #20) on its first use.

Not marked resolved: pstore would still be the better instrument for a failure
that takes the network with it, and why the region does not survive is unknown.

## 🟡 B-36 — reading a DSI register block with the display asleep can hang the SoC

**Opened 2026-08-21.** The documented trap was that such a read returns
`0x00000000`. It is worse than that: a measurement script that read
`0x1401b000` after a DPMS blank **wedged the machine** hard enough that the
watchdog needed about five minutes to reclaim it, and the failure looked exactly
like "DPMS off hangs the kernel" — which it does not; a clean run returns
normally and the system stays up.

Two rules follow. Do not read display registers while the display is asleep,
and note that the panel gate's DSI base is `0x1401c000`; `0x1401b000` is a
different block and reading it is what did the damage.

## 🟡 B-17 — DRM atomic commit never completes (`flip_done`/vblank timeout loop), panel stays dark

**Opened 2026-07-08**, split out of B-13 once B-13's original scope (cpu0
hard-lock + `-517` DSI-attach probe-defer) was confirmed fully resolved
(see B-13's "Update 2026-07-08" above and boot.md "BUILD #161 recheck / new
blocker found: mtk_mipi_tx D-PHY probe EBUSY").

**Correction 2026-07-08 (same day, live SSH investigation on build #159):**
the D-PHY `-EBUSY` this blocker was originally opened around is a **red
herring**, not the cause. Live check on the running device:
```
# /proc/iomem
10215000-1021508f : 10215000.mipi-dphy mipi-dphy@10215000
# /sys/kernel/debug/clk/clk_summary
mipi_tx0_pll   1  1  1  927504000  0  0  50000  ?
```
The D-PHY *is* bound (built in via `CONFIG_PHY_MTK_MIPI_DSI=y`) and its PLL
is running live at 927.5 MHz — proof the real, built-in copy of
`mediatek-mipi-tx` probed successfully. The `-EBUSY`/"already registered"
message logged later at boot is a **second, stale registration attempt**:
`/lib/modules/6.6.0-dirty/kernel/drivers/phy/mediatek/phy-mtk-mipi-dsi-drv.ko`
is a leftover `.ko` from an earlier build configuration (when this driver
was `=m`) still present on the rootfs; something (module autoload/coldplug
replay) tries to insert it after the built-in driver already owns the
`"mediatek-mipi-tx"` driver name, and the second registration is correctly
rejected. Harmless duplicate-load noise — not a probe failure of the real
D-PHY, and not the reason the panel stays dark. (Cleanup: the stale `.ko`
should be removed from the rootfs module tree in the next `mkrootfs.sh`
run so this noise stops appearing in logs.)

**Actual symptom (root cause still open):** on build #159 (banner #48),
with live SSH access confirmed and the full DSI/panel/D-PHY/DRM bind chain
completing successfully — DSI host attaches on deferred retry after ~62s,
`panel-solomon-ssd2092 1401c000.dsi.0: Solomon SSD2092 FHD DSI panel
registered`, `probe of 1401c000.dsi.0 returned 0`, `fb0: mediatekdrmfb`
created, `GEMINI-DEBUG bind: complete` — every DRM atomic commit (driven by
the fbdev helper's hotplug retry) times out waiting for vblank/flip
completion, in an infinite ~10-second-period loop:
```
mediatek-drm mediatek-drm.1.auto: [drm] *ERROR* flip_done timed out
mediatek-drm mediatek-drm.1.auto: [drm] *ERROR* [CRTC:51:crtc-0] commit wait timed out
mediatek-drm mediatek-drm.1.auto: [drm] *ERROR* [PLANE:33:plane-0] commit wait timed out
mediatek-drm mediatek-drm.1.auto: [drm] *ERROR* [CONNECTOR:32:DSI-1] commit wait timed out
[drm:mtk_drm_crtc_atomic_begin] *ERROR* new event while there is still a pending event
WARNING: ... drm_atomic_helper_wait_for_vblanks.part.0+0x23c/0x260
```
System otherwise remains stable and fully reachable over SSH throughout —
this is not a hang, just a permanently-dark, permanently-retrying display
pipeline. No panel-driver `prepare`/`enable` activity (regulator/reset/init
command sequence) is visible in the default-level kernel log — this is
expected at default log level (no dynamic-debug enabled for
`panel-solomon-ssd2092.c` or `mtk_dsi.c`), not evidence they were skipped.

**Not yet investigated:** why the CRTC never produces a real vblank/frame.
Leading candidates, in the order they should be checked next:
1. Whether `drm_panel_prepare()`/`drm_panel_enable()` are actually being
   invoked by the DSI bridge's atomic enable path, and whether the SSD2092
   init command sequence (`panel/0005-drm-panel-add-solomon-ssd2092-fhd-panel.patch`)
   completes or errors silently — enable `dyndbg` for both `mtk_dsi.c` and
   `panel-solomon-ssd2092.c` on the next boot to see this.
2. Whether the DSI host's own vblank/TE (tearing-effect) IRQ path is
   correctly configured post-B-13 fix (patch
   `0008-…-dsi-keep-irq-disabled-b13-test.patch` intentionally holds the IRQ
   masked until DSI power-on — confirm it is actually unmasked again once
   the pipeline reaches enable, otherwise no vblank can ever fire by
   construction).
3. Whether `disp_ovl0`/`mutex`/`disp_rdma0` etc. actually reach a running
   state (their `status = "okay"` in `dts/0001` doesn't guarantee correct
   runtime configuration) — cross-check the vendor 3.18 `dispsys`/DDP
   config sequence for post-power-on register pokes not yet ported.
The vendor 3.18 source (`/Volumes/extdata/github/gemini-android-kernel-3.18`,
per CLAUDE.md) remains the reference for whichever of the above needs a
concrete register/sequence answer.

**Recovery:** no special recovery needed — the system remains stable and
SSH-reachable with this failure present; it is a rendering-pipeline stall,
not a hang or crash. Current known-good baseline is build #159 (`boot`/
`boot2` identical, flashed 2026-07-08).

**Update 2026-07-08 (evening) — debug build #164 (candidate #1 trace) itself
appears to hang/stall before reaching USB gadget config:** built and flashed
`patches/v6.6/zz-debug/0001-GEMINI-DEBUG-dsi-panel-enable-trace.patch`
(candidate #1 from the list above — `pr_info()` inside `mtk_dsi_irq()` on
every fire, plus `dev_info()`/`pr_info()` traces in `mtk_dsi_poweron`,
`mtk_output_dsi_enable`, the bridge `atomic_enable`/`atomic_pre_enable`
callbacks, and `ssd2092_prepare`/`enable`/`get_modes`). Packed as build #164
(banner #53, `ALLOW_DEBUG=1` — the pack script's debug-instrumentation gate
correctly caught this and required the override).

Across multiple flash/power-cycle attempts, build #164 **never presented a
USB gadget device at all** (`ioreg` showed nothing but the always-attached
FTDI serial adapter — no RNDIS/Ethernet Gadget, ever, at any point). Serial
capture couldn't help distinguish "still booting" from "genuinely stuck",
because the UART/USB mux cutoff (`mtu3` driver flipping the mux register
during its own probe, ~0.44-3.0s kernel time) happens in software
regardless of which cable is physically connected — confirmed this session
by capturing with the FTDI left connected throughout (no swap), which still
cut off at the identical point as every prior capture.

**Control test:** reflashed known-good build #159 (banner #48, no debug
instrumentation) on both `boot`/`boot2` with no other change. RNDIS gadget
enumerated normally, link came up, `ping` and `ssh root@10.15.19.82`
succeeded immediately (`uname -a` confirmed banner #48). This isolates the
stall to something specific about build #164, not a Mac-side/cable/RNDIS-
service problem (a stale macOS network service was also removed and USB
services restarted along the way, with no effect on #164 — ruling out
Mac-side state as the cause).

**Working hypothesis:** the `pr_info()` inside `mtk_dsi_irq()` is the prime
suspect. If DSI's IRQ (SPI 229) is firing at high frequency — which is
exactly candidate #2's concern, IRQ storming — a synchronous console print
on every fire from interrupt context could starve the CPU badly enough that
boot never reaches the point of configuring the USB composite gadget
(which happens from userspace, well after kernel init). This would
actually be a positive result for root-causing B-17 (confirms the IRQ is
storming) if true, but the trace as written can't get the data out to
confirm it, since it may itself be the reason nothing further happens.

**Next step:** rate-limit the IRQ trace (`pr_info_ratelimited()` instead of
unconditional `pr_info()`, and/or a bounded per-boot counter) so it can
report whether the IRQ is storming without adding enough overhead to stall
boot outright. See `patches/v6.6/zz-debug/` for the revised patch.

**Update 2026-07-08 (later same evening) — IRQ-storm hypothesis inconclusive
after three isolation builds; reframed by a known-good build ALSO failing:**
followed the isolation path above through three further debug builds, each
flashed and captured on hardware:
- **Build #168** (`patches/v6.6/zz-debug/0001-...patch` v2, banner #54): same
  full trace set as #164 but with the `mtk_dsi_irq` print rate-limited via an
  `atomic_long_t` fire counter (log only the 1st and every 4096th fire). Did
  not reach USB gadget networking.
- **Build #170** (v3, banner #55): stripped to *only* the rate-limited IRQ
  counter — every other trace point removed, to isolate whether the IRQ
  print alone (regardless of frequency) was the problem. Still did not reach
  USB gadget networking.
- **Build #172 / #174** (v4, banners #56/#58): added an IRQ-storm circuit
  breaker (`disable_irq_nosync()` once the fire count crosses 200000,
  guarded by `atomic_xchg` so it only fires once) so that even a genuine
  storm would be forcibly broken and boot could proceed regardless of root
  cause. #174 additionally carried the pstore config fix (see below). Neither
  build reached USB gadget networking.

**Critical pivot:** after build #174 also failed, known-good build **#159**
(previously reliable, zero debug instrumentation, the same image that
isolated #164 in the first control test above) was reflashed as a sanity
check — and **it also failed to bring up the USB gadget, and the device
self-reset unprompted**. This directly contradicts the working hypothesis
that the debug patches (IRQ storm / print-in-ISR starvation) were
responsible: #159 has none of that code and failed anyway. A follow-up
serial-only sanity capture (build #177, no USB involved) confirmed the board
itself boots completely normally on this same #159 image — clean banner,
reaches the expected `mtu3`/mux cutoff at ~2.99s, no panics or anomalies —
ruling out a boot-level kernel hang. So the board boots fine; specifically
USB-C data enumeration/link-up was failing intermittently across every build
tried late in this session, including known-good ones.

**Working hypothesis revised: marginal battery / power delivery, not
software.** The user observed the device's battery had not been charging
throughout the session's many flash/power-cycle operations and was very low.
A low/marginal battery would explain the whole pattern independent of which
kernel was flashed: brownout-induced resets under a current spike (USB
gadget enumeration, display init, CPU ramp), USB data-line negotiation being
the first thing to suffer when the rail is marginal, and a previously
reliable build suddenly failing with no code change. This is considered the
leading explanation for the late-session #164/#168/#170/#172/#174/#159
failures as of 2026-07-08, superseding the IRQ-storm hypothesis as the
primary suspect (though the IRQ-storm circuit breaker and rate-limited trace
remain harmless/available in `patches/v6.6/zz-debug/` if needed again).
**Not yet confirmed** — next step is to retest once the device has had a
proper charge: reflash the known-good baseline (build #178/banner #63, see
below) and confirm consistent gadget enumeration and SSH reachability across
multiple power cycles.

**Unrelated fix folded in this session — stable gadget MAC address:**
separately, `CONFIG_USB_ETH` is built in (`=y`), so `g_ether` had no
persistent MAC and randomized one on every boot; macOS keys its Ethernet
"service" identity off the MAC, so every boot produced a brand-new `enNN`
interface and network service, making "is the gadget really not coming up"
indistinguishable from "it came up as a different interface than expected."
Fixed via `g_ether.dev_addr=42:00:15:19:82:01 g_ether.host_addr=42:00:15:19:82:00`
added to `CONFIG_CMDLINE` in `configs/gemini-cmdline.config` (built-in
drivers still honor `<modname>.<param>=` on the kernel cmdline). Also folded
in: `configs/gemini-pstore.config` (new file) enabling `CONFIG_PSTORE_RAM`/
`CONFIG_PSTORE_CONSOLE`/`CONFIG_PSTORE_PMSG` — the `ramoops@44410000`
reserved-memory node already existed in `dts/0001` (matches the vendor's own
pstore region for dual-boot safety) but the kernel config to actually back
it with pstore had never been enabled. Both changes built cleanly with debug
instrumentation removed, packed as **build #178 (banner #63,
`logs/2026-07-08-178-stable-boot-fixed-mac/`, sha256
`eeca62d1ef9cddbbdc825c63b708568870f2b669e407eb43f55448c00c2e1b7c`)** —
this is the current baseline pending the battery-recharge retest above.

**Also investigated, dead end:** while troubleshooting the late-session USB
failures, checked whether stock Android's `/proc/last_kmsg` (via `adb shell`
in a production, non-rooted build) held anything from the #159 self-reset —
it only contained an empty ram-console header (`hw_status: 0`, no crash
recorded), and `/sys/fs/pstore/` was inaccessible without root. Android's
ram-console format is unrelated to the Linux pstore/ramoops format just
enabled above (different formats, even sharing the same physical region), so
this path can't retroactively explain the #159 reset — only a future boot
with build #178's pstore config, followed by a crash, followed by *our own*
kernel's `/sys/fs/pstore/` would be informative.

**Update 2026-07-09 — retested post full scatter-file reflash; gadget/SSH
failure reproduces even on a known-clean baseline, battery hypothesis now in
doubt:** the Gemini was fully restored to stock (SP Flash Tool + scatter
file, all partitions incl. `boot`/GPT back to factory Android/Kali) and
confirmed RNDIS worked fine on the Linux workstation with the *stock* image.
Build #178 (banner #63, same sha256 as above) was then reflashed to `boot2`
fresh against this known-clean baseline (`logs/2026-07-09-181-post-scatter-
reflash-boot2.log`). Kernel boot itself is clean — same banner, same benign
`-517` DSI defer, reaches the same `mtu3` mux-cutoff point with no
anomalies — but the `g_ether` gadget again enumerates on the Mac with the
correct fixed MAC (`en12`, `42:00:15:19:82:00`) and then sits at `status:
inactive` indefinitely; no ping/SSH to `10.15.19.82`. This is the **same
symptom** as the pre-reflash failures this blocker attributed to a low
battery — but now reproduced immediately after a full factory restore and a
presumably-charged battery (device was freshly reflashed, not sitting idle
draining). This weakens the battery-only explanation: either the battery
issue is still present independently of state, or there is a genuine
software-side regression in how `g_ether`/CDC networking comes up under
Linux 6.6 on this device that coincidentally overlapped with the low-battery
period. **Not yet root-caused** — next step is to get a shell some other way
(serial console login, if enabled, or checking `dmesg`/journal for the
`g_ether`/`dwc3`/`mtu3` gadget-side attach sequence) rather than relying on
inferring gadget health purely from the Mac-side link state.

**Aside — LK splash successfully reaches the panel (2026-07-09):** during
this same boot, the user observed the screen flash briefly with color (no
text) a few seconds after power-on. Log analysis shows this came from LK's
own bootloader-side display driver (`DDP/mgr`/`videolfb` lines, ms-based
timestamps, *not* the Linux kernel's `[   0.xxx]` log) actively binding
`ovl0`→`dsi0` and loading `lcmname = aeon_ssd2092_fhd_dsi_solomon` around the
4.1s mark — well before `Linux version 6.6...` even prints. The Linux
kernel's own DRM/DSI probe in the same log still fails with the same benign
`-517` EPROBE_DEFER it always has. This is useful confirming evidence for
B-17: the DSI physical link, panel, and LK's own driver can successfully
push real pixels to this exact panel, so "colorful, no text" is most likely
LK loading a corrupted/uninitialized logo buffer, not an electrical/panel
fault — the still-open problem is purely getting the *Linux* DSI driver past
its own probe-defer, not proving the hardware path works (it already does).

**Update 2026-07-09 — build #159 (previously verified live-SSH) now
reproduces the identical gadget failure, ruling out #178's changes as the
cause:** reflashed `boot2` with build #159 (banner `#48`,
`logs/2026-07-07-159-ovl-larb-devicelink/new_kali_boot.img`, sha256
`09b2ea91a80e850c099fcf5dfe958a89033129e18983871f3b96f384ffe06b98`) — the
same image that was live-SSH validated in an earlier session (this blocker's
"Critical pivot" note above) — as a control against build #178. Two boot
attempts landed on the stock **Android** `boot` partition instead of `boot2`
(confirmed by the dumped LK cmdline: `androidboot.hardware=mt6797
buildvariant=user printk.disable_uart=1`, not our Linux cmdline) before a
third attempt correctly reached `boot2` (`logs/2026-07-09-182-build159-post-
scatter-recheck.log`, confirmed by `Linux version 6.6.0-dirty ... #48 SMP
PREEMPT Tue Jul  7 10:13:15 UTC 2026` at kernel time 0). Boot itself is
clean, same `mtu3` mux-cutoff point as always.

Result: **identical failure to #178.** `ioreg` confirms the USB gadget
device itself enumerates on the Mac (`RNDIS/Ethernet Gadget`, `active`,
matched), `en12` is created, but `unified log` shows the actual link-layer
state: `configd: (IPConfiguration) MANUAL en12: status = 'media inactive'`
— i.e. this is not an IP/DHCP/routing problem, it is the USB
CDC/RNDIS **carrier/link** signal itself never asserting from the Gemini
side, even though USB enumeration (descriptors, driver match) succeeds.
Same result on both #159 and #178 rules out every software delta introduced
between them (fixed-MAC cmdline, pstore config, all the DSI-IRQ debug
patches) as the cause. Remaining candidate explanations, in order of
likelihood:
1. **Battery/power** (original hypothesis) — still not conclusively ruled
   in or out; the device read ~42% at the time of this test, healthier than
   the earlier near-dead state, but marginal current-delivery under USB
   enumeration load can't be excluded without a controlled bench-supply test.
2. **Physical layer** (cable, port, or connector wear) — untested this
   session; worth swapping cable/port as a cheap isolation step.
3. A genuine kernel-side regression in `mtu3`/`g_ether` link-state signaling
   that predates #159 (i.e., was already broken when #159 was thought
   "known-good" via SSH, and something about the *test conditions* that day,
   not the kernel, made it work) — cannot be ruled out without a shell on
   the device via a path that doesn't depend on this USB link (e.g. serial
   getty login, if enabled on `ttyS0`).

**Next step:** try a different USB cable/port for the next attempt (rules
out #2 cheaply); if it still fails, get a shell over serial instead of USB
to inspect `dmesg`/`journalctl` for the gadget-side attach sequence directly,
rather than continuing to infer gadget health only from the Mac-side state.

**Update 2026-07-09 (continued) — exhaustive isolation across cable, Mac
state, and kernel age; USB link itself proven healthy; root cause still
unresolved.** A full round of isolation steps, each ruling out one
candidate:

- **Different USB cable** (user-tested): no change, same symptom. Rules out
  a worn/marginal cable.
- **Full Mac power-off + restart**, then removing and letting macOS
  recreate the "RNDIS/Ethernet Gadget" network service from scratch: no
  change. Rules out any stale `configd`/`IOUSBHostFamily`/cached-service
  state on the Mac as the cause.
- **Static IP re-verified intact** on the freshly recreated service
  (`10.15.19.1/24`, matches the device's `10.15.19.82` config) — not an IP/
  DHCP misconfiguration.
- **`ioreg` USB link diagnostics, captured live while build #159 was
  running:** `UsbLinkSpeed = 480000000` (full USB2 High-Speed — the
  electrically demanding chirp handshake succeeded), `bNumConfigurations = 2`
  (both descriptor configs advertised, as expected for `g_ether`'s
  RNDIS+ECM composite), `kUSBCurrentConfiguration = 1` (macOS selected and
  fully enumerated a configuration). This is strong proof the physical
  link, connector, and low-level enumeration are entirely healthy — the
  failure is narrowly scoped to *after* configuration selection, at the
  interface-level data alt-setting/carrier handshake (where Linux's
  `u_ether`/`usb_f_ecm` is supposed to call `netif_carrier_on()`).
- **Manually cycling the interface** (`ifconfig en12 down` / `up`) while the
  device stayed connected: no change — rules out the well-known "macOS ECM
  driver needs a nudge" community workaround.
- **Control test with build #71** (`logs/2026-07-06-71-usb-gadget-plus-uart-
  clk-fix/new_kali_boot.img`, banner `#5`, Mon Jul 6 06:22:43 UTC 2026) — the
  very first build CLAUDE.md documents as fully validated end-to-end over
  SSH-over-USB, predating every later display/DSI/SMI/IRQ change. Reflashed
  fresh to `boot2`; boots clean to the same `mtu3` cutoff point
  (`logs/2026-07-09-184-build71-earliest-good-recheck.log`); **identical
  failure** — `en12` exists, `UsbLinkSpeed`/config selection identical to
  #159, `status: inactive`, no ping/SSH.

**This is the most significant result of the session: the literal kernel
bytes that gave working SSH on 2026-07-06 now fail identically, on a
Mac that has been fully power-cycled, with a swapped cable, and a freshly
recreated network service.** This conclusively rules out every software
delta accumulated across builds #71→#178 (DSI-IRQ debug patches, fixed-MAC
cmdline, pstore config, SMI-larb work) as the cause — the kernel image
itself is not the variable. It also weakens the pure-Mac-state theory,
since the Mac is now about as clean as it can be without an OS reinstall.

**What's left, in order of likelihood given the evidence above:**
1. **Something changed on the Gemini's own analog/hardware side that
   persists across kernel reflashes but isn't kernel-controlled** — most
   plausibly PMIC/charging-IC state or NVRAM-backed calibration data reset
   by the full SP Flash Tool scatter-file restore (which, unlike a
   `boot2`-only reflash, rewrites `nvram`/`nvcfg`/`proinfo`). A full HS link
   negotiation succeeding doesn't rule out a marginal analog condition that
   only manifests at the specific point Linux's gadget stack tries to
   signal carrier.
2. **Battery/charging state** — still not conclusively excluded; a
   controlled bench power supply (bypassing the battery/PMIC entirely)
   would be the definitive test, not yet performed.
3. A genuine bug in Linux's `mtu3`/`u_ether`/`usb_f_ecm` interaction that
   was *never* actually reliable on this hardware, and appeared to work in
   earlier sessions only due to a — currently unidentified — favorable
   condition that no longer holds. Cannot be fully ruled out without a
   kernel-side shell (blocked by the UART/USB mux sharing the same port,
   see B-15) or persistent logging (blocked by `journald`'s default
   volatile storage on this rootfs, see the mkrootfs.sh discussion this
   session).

**Not yet tried:** connecting the Gemini to a genuinely different host
machine (e.g. the Linux workstation, which already saw RNDIS work
successfully once this session on the stock Android image) to determine
whether the failure is Mac-specific or universal — this is the next
highest-value isolation step, since it's the one major variable (the host)
not yet swapped.

### PENDING TEST (as of 2026-07-09) — cross-host isolation, run this next

**Read this whole block before doing anything — it's written so a fresh
Claude Code session with no memory of prior conversation can pick this up
cold on the Linux workstation.**

**Background:** `g_ether`'s USB gadget networking (the fast-track SSH-over-
USB path from Phase 8) has stopped working partway through this session,
identically across three kernel builds spanning the whole project history
(#71 — the very first ever validated build, #159, #178) and after
exhaustive Mac-side isolation (different cable, full Mac power-cycle, fresh
network service, static IP re-verified, `ioreg` confirms a fully healthy
480 Mbps USB link with configuration selection succeeding). See the "Update
2026-07-09" entries above this block for the full trail. The remaining
open question: **is this failure specific to the Mac, or does it reproduce
on any host** (pointing instead at the Gemini's own hardware/firmware
state)?

**What to actually do:**

1. This machine (Linux workstation) has **no build VM, no FTDI rig, no
   mtkclient, and no direct hardware access** — see the Machine Profiles
   section in `claude.md`. The Gemini itself, already flashed with a known
   kernel build on `boot2`, needs to be physically brought to this machine
   and plugged in via USB-C by the user — you cannot flash or drive the
   device yourself from here.
2. Once the user has the Gemini connected via USB-C to this Linux machine
   (booted into whichever kernel is currently on `boot2` — ask the user
   which build, or check the last blockers.md/boot.md entries for the most
   recently flashed one), check for the gadget interface the same way the
   Mac side was checked, translated to Linux equivalents:
   - `ip link show` / `ip addr show` — look for a `usb0` or similar
     interface (Linux typically auto-names CDC-ECM/RNDIS gadgets `usb0`,
     `enxAAAAAA...`, or similar, unlike macOS's `enNN`).
   - `dmesg | grep -i "cdc_ether\|rndis\|usb0"` — Linux hosts have a
     built-in `cdc_ether`/`cdc_ncm` driver; check whether it binds and
     reports a link-up/link-down (carrier) message, which is the direct
     equivalent of macOS's `ifconfig ... status: inactive`.
   - `ethtool usb0` (or whatever the interface is named) — reports `Link
     detected: yes/no`, the Linux equivalent of the carrier check.
3. If a static IP is needed on the Linux side to talk to the Gemini's
   `10.15.19.82` (see `configs/gemini-cmdline.config`'s `usb0.network`
   config, which sets that address on the device side), configure e.g.
   `sudo ip addr add 10.15.19.1/24 dev usb0` (adjust interface name) and
   then `ping 10.15.19.82` / `ssh root@10.15.19.82`.
4. **Interpreting the result:**
   - If the Linux workstation *also* sees no carrier / no link, that's
     strong evidence the problem is on the Gemini's side (hardware,
     firmware, or NVRAM state from the SP Flash Tool restore), not
     something Mac-specific — refocus investigation there (see the
     numbered candidate list above this block).
   - If the Linux workstation *does* get a working link/SSH, that's
     surprising given the Mac-side isolation done so far, and would point
     back at something specific to the Mac (perhaps a lower-level
     Thunderbolt/USB-C controller or driver state that a full OS restart
     didn't clear) — in that case, re-open the Mac-side investigation with
     that new fact in hand, and note it doesn't fully square with the
     `ioreg` evidence already gathered (healthy 480 Mbps link, successful
     config selection) which argued against a Mac hardware/driver problem.
5. **Document the result** in this same B-17 section of `blockers.md`
   (append, don't overwrite) and in `boot.md`, then commit and push so the
   Mac-side session (or the next session on either machine) has it. Follow
   the existing per-attempt provenance convention (see CLAUDE.md's Logging
   Requirements) even though no build/flash happened on this machine —
   note which build was on `boot2` at the time, and the exact Linux-side
   commands/output.

**Update 2026-07-08 — cross-host isolation complete; failure reproduces on Linux workstation; Mac-specific cause ruled out.**

Run on the Linux workstation (machine with no build VM/FTDI, per CLAUDE.md Machine Profiles). Gemini plugged in via USB-C while already running whatever build was on `boot2` at the time.

Build identity from this machine: the host-side MAC presented was `3a:d7:49:25:ce:01` (a random locally-administered MAC), which does **not** match build #178/#179's configured fixed host MAC (`42:00:15:19:82:00`). This indicates the build on `boot2` was older than #178 — most likely build #159 (banner #48), based on the last confirmed reflash in the prior session's "Update 2026-07-09" entries above. (User should confirm if unsure.)

Commands and output:

```
$ lsusb | grep -i "rndis\|ethernet gadget\|gadget"
Bus 001 Device 072: ID 0525:a4a2 Netchip Technology, Inc. Linux-USB Ethernet/RNDIS Gadget

$ ip link show enx3ad74925ce01
61: enx3ad74925ce01: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc fq_codel state DOWN ...
    link/ether 3a:d7:49:25:ce:01 brd ff:ff:ff:ff:ff:ff

$ ethtool enx3ad74925ce01
...Speed: 425Mb/s...Auto-negotiation: off...Link detected: no

$ sudo ip addr add 10.15.19.1/24 dev enx3ad74925ce01 && sudo ip link set enx3ad74925ce01 up
$ ip link show enx3ad74925ce01
61: enx3ad74925ce01: <NO-CARRIER,BROADCAST,MULTICAST,UP> ...

$ ping -c 3 10.15.19.82
PING 10.15.19.82 ... Destination Host Unreachable (3 packets, 0 received, 100% loss)
```

`cdc_ether` bound correctly (USB enumeration and descriptor exchange fully succeeded; `DRIVER=cdc_ether`, `PRODUCT=525/a4a2/606` confirmed via sysfs). The failure is the same as on the Mac: carrier never asserts from the Gemini side after the host driver binds — `NO-CARRIER`/`Link detected: no` throughout, identical to macOS's `status: inactive`.

**Interpretation (per the guidance block above):** the failure reproducing on a fully independent host, after a complete Mac power-cycle and scatter-file restore, **conclusively rules out Mac-specific hardware/software state as the cause.** The problem is on the Gemini's own side.

**Root cause identified 2026-07-08:** all of the "identical failure across builds #71/#159/#178" and both Mac + Linux host failures trace to a single cause: **the SP Flash Tool scatter-file restore wiped p29 (`linux` partition) and replaced the Debian 13 rootfs with the factory Kali image.** Every working SSH session in this project ran against the Debian 13 rootfs built by `scripts/mkrootfs.sh`, which installs `usb0.network` (static `10.15.19.82/24`) and enables `systemd-networkd`. The Kali image has none of that — so `usb0` on the device side is never configured, `10.15.19.82` is never assigned, and SSH is impossible regardless of kernel build or host machine. The USB enumeration and gadget bring-up themselves are fine (built-in `CONFIG_USB_ETH=y`, no userspace involvement needed for those). The kernel was never the problem. Every diagnostic trail from this blocker that blamed the kernel, NVRAM, battery, or PMIC can now be set aside pending a rootfs reflash.

**Fix — reflash Debian 13 rootfs (run on Mac, build VM must be running):**

```bash
# 1. SSH into build VM (adjust IP/port as needed, see claude.md Machine Profiles)
ssh -p 10022 root@localhost

# 2. In the VM: rebuild the rootfs (takes ~5 min)
cd ~/linux-6.6          # or wherever the repo is cloned in the VM
bash scripts/mkrootfs.sh
# Output: /mnt/host/OUTPUT/debian13-rootfs.img

# 3. Exit VM; on Mac: put Gemini in preloader mode and flash linux partition
# (hold Vol-Up while powering on with USB-C connected, or use mtkclient's
# auto-detect — NEVER use `mtk wl`, only `mtk w linux ...`)
/tmp/mtk-venv/bin/python3 ~/mtkclient/mtk.py w linux ~/gemini-build/OUTPUT/debian13-rootfs.img

# 4. Power-cycle the Gemini (boot2 should still have the last-flashed Linux kernel)
# 5. Confirm SSH:  ping 10.15.19.82  →  ssh root@10.15.19.82  (password: toor)
# 6. On device after first boot, grow the partition to fill p29's full 25.8 GiB:
#    resize2fs /dev/mmcblk0p29
```

After SSH is restored, resume B-17's original display investigation (the `flip_done` timeout / vblank loop) from the "Not yet investigated" candidates at the top of this section.

**Update 2026-07-08 — fix verified end-to-end; B-17's gadget/SSH sub-issue CLOSED.**

Ran the reflash procedure above (build VM on the Mac, `scripts/mkrootfs.sh` rebuilt a fresh `debian13-rootfs.img`, sha256
`a87d4780e7ccbbdba0a281b7e174c60f0eff181c1e470c5bdc8c5b3e8cd8c79e`, flashed to `linux` (p29) with
`mtk w linux ~/gemini-build/OUTPUT/debian13-rootfs.img`, boot2 left untouched at build #71). Serial capture
(`logs/2026-07-09-185-freshrootfs-boot-check.log`) confirms a clean boot through `mtu3 11271000.usb` init (the
expected UART/USB mux handoff point per B-15) with no new kernel-side errors. Post-boot, macOS's `en12`
(RNDIS/Ethernet Gadget) came up `status: active` at 100baseTX — the carrier/link problem is gone, confirming the
diagnosis. Mac side needed a static IP added manually (`sudo ifconfig en12 alias 10.15.19.1 netmask
255.255.255.0`) since the interface only self-assigned an APIPA address; once set, `ping 10.15.19.82` and
`ssh root@10.15.19.82` (password `toor`, fresh host key — expect and clear the one-time `ssh-keygen -R
10.15.19.82` host-key-changed warning) both succeeded:

```
$ ssh root@10.15.19.82 uname -a
Linux gemini 6.6.0-dirty #5 SMP PREEMPT Mon Jul  6 06:22:43 UTC 2026 aarch64 GNU/Linux
Debian GNU/Linux 13 (trixie)
```

This closes the gadget-networking sub-thread of B-17 (root cause: SP Flash Tool scatter restore had wiped the
rootfs, not a kernel/driver defect — no code change was needed, only a rootfs reflash). The **display** sub-issue
(DRM atomic commit / `flip_done` timeout, panel dark) that gives this section its title remains open — see the
"Not yet investigated" candidates above for where to resume that separately.

| Date | Was | Resolution |
|------|-----|-----------|
| 2026-06-10 | Console identity contradiction (ttyMT0 vs ttyMT3 vs ttyS0) — risk of silent dead boot | **ttyMT0 = UART0 @ 0x11002000 @ 921600**, triple-sourced (vendor DTB bootargs + spec Table 2-7 pinmux + mainline dtsi). ttyMT3 was a never-used `CONFIG_CMDLINE` fallback. See kernel.md. |
| 2026-06-10 | Reserved-memory carve-outs unknown — risk of stomping ATF/TEE | Full map recovered from vendor DTB; carve-outs + ramoops added to `dts/0001`. See kernel.md / boot.md. |
| 2026-06-10 | Vendor decompiled DTS lived in volatile `/tmp` | Re-extracted and committed: `docs/vendor-dtb/` (DTB + DTS + known-good kernel config). |
| 2026-06-08 | WiFi/BT port feasibility unknown | Researched: ~75–103 KLOC vendor stack, broken upstream since 5.7/6.0. Deferred to Phase 9; USB-Ethernet is the Phase 8 plan. See research.md. |
| 2026-06-07 | GCC ≤4.9 believed required | Empirically debunked; GCC 15.2.0 works for both 3.18 and 6.6. See CLAUDE.md. |
| 2026-06-07 | `mtk wl` GPT corruption | Banned; targeted `mtk w` writes only. See CLAUDE.md Flashing. |

---

## 📌 Baseline snapshot — 2026-07-08 (known-good, post B-17 rootfs fix)

Recorded for future reference after B-17's gadget/SSH sub-issue was closed (root
cause: SP Flash Tool scatter restore had wiped the rootfs, not a kernel bug —
see B-17 above). This is the full state of the device at the point SSH-over-USB
was reconfirmed working end-to-end.

**Flashed images and hashes:**

| Partition | Image | sha256 |
|-----------|-------|--------|
| `boot2` | `logs/2026-07-06-71-usb-gadget-plus-uart-clk-fix/new_kali_boot.img` (build #71, banner #5) | `c38e176bf18870a17636d66d22081c2e463384f9587c322bd4de2d8fe484d98e` |
| `linux` (p29) | `debian13-rootfs.img` (fresh, via `scripts/mkrootfs.sh`, 2026-07-08) | `a87d4780e7ccbbdba0a281b7e174c60f0eff181c1e470c5bdc8c5b3e8cd8c79e` |

**GPT partition table** (read via `mtk.py printgpt`, read-only — matches the
documented layout exactly, no corruption from any prior `mtk wl` incident):

```
recovery:    Offset 0x0000000000008000, Length 0x0000000001000000
para:        Offset 0x0000000001008000, Length 0x0000000000080000
expdb:       Offset 0x0000000001088000, Length 0x0000000000a00000
frp:         Offset 0x0000000001a88000, Length 0x0000000000100000
nvcfg:       Offset 0x0000000001b88000, Length 0x0000000000800000
nvdata:      Offset 0x0000000002388000, Length 0x0000000002000000
metadata:    Offset 0x0000000004388000, Length 0x0000000002000000
protect1:    Offset 0x0000000006388000, Length 0x0000000000800000
protect2:    Offset 0x0000000006b88000, Length 0x0000000000c78000
seccfg:      Offset 0x0000000007800000, Length 0x0000000000800000
oemkeystore: Offset 0x0000000008000000, Length 0x0000000000200000
proinfo:     Offset 0x0000000008200000, Length 0x0000000000300000
md1img:      Offset 0x0000000008500000, Length 0x0000000001800000
md1dsp:      Offset 0x0000000009d00000, Length 0x0000000000400000
md1arm7:     Offset 0x000000000a100000, Length 0x0000000000300000
md3img:      Offset 0x000000000a400000, Length 0x0000000000500000
scp1:        Offset 0x000000000a900000, Length 0x0000000000100000
scp2:        Offset 0x000000000aa00000, Length 0x0000000000100000
nvram:       Offset 0x000000000ab00000, Length 0x0000000000500000
lk:          Offset 0x000000000b000000, Length 0x0000000000080000
lk2:         Offset 0x000000000b080000, Length 0x0000000000080000
boot:        Offset 0x000000000b100000, Length 0x0000000001000000
logo:        Offset 0x000000000c100000, Length 0x0000000000800000
tee1:        Offset 0x000000000c900000, Length 0x0000000000500000
tee2:        Offset 0x000000000ce00000, Length 0x0000000000500000
keystore:    Offset 0x000000000d300000, Length 0x0000000000d00000
system:      Offset 0x000000000e000000, Length 0x00000000a0000000
cache:       Offset 0x00000000ae000000, Length 0x000000001b000000
linux:       Offset 0x00000000c9000000, Length 0x0000000671700000
boot2:       Offset 0x000000073a700000, Length 0x0000000001000000
boot3:       Offset 0x000000073b700000, Length 0x0000000001000000
userdata:    Offset 0x000000073c700000, Length 0x00000007520fbe00
flashinfo:   Offset 0x0000000e8e7fbe00, Length 0x0000000001000000

Total disk size: 0x0000000e8f800000, sectors: 0x000000000747c000
```

**Verified working at this snapshot:** `ssh root@10.15.19.82` (password
`toor`) over the `g_ether` USB gadget; kernel banner
`Linux gemini 6.6.0-dirty #5 SMP PREEMPT Mon Jul  6 06:22:43 UTC 2026 aarch64`;
`Debian GNU/Linux 13 (trixie)` userspace.

**To restore this exact baseline later:** flash `boot2` and `linux` with the
two images above (`mtk w boot2 ...` / `mtk w linux ...`), leave all other
partitions untouched.

## Update 2026-07-09 — B-17 DSI IRQ bounded-timeout fix attempted, regresses intermittently, still open

Attempted a fix for the recurring cpu0 hard-lock (the same class of failure
B-13 already root-caused): `patches/v6.6/drm/0012-drm-mediatek-dsi-bound-irq-busy-wait-timeout.patch`
bounds the unbounded `while (tmp & DSI_BUSY)` spin in `mtk_dsi_irq()` with
`readl_poll_timeout_atomic()` (1us poll / 20ms timeout).

**First version regressed hard**: builds #195/#197 (boot.md) both hit a
full watchdog reset (`wdt_by_pass_pwk`) — root cause was the timeout branch
returning `IRQ_HANDLED` without clearing `DSI_INTSTA` or waking
`irq_wait_queue`, turning a level-triggered IRQ's bounded poll into an
unbounded hardirq-storm with the status bit never cleared. Fixed by always
clearing/waking on both paths.

**Corrected version (build #200) is still not reliable.** Same flashed
image, three power-on attempts, three different outcomes: a full ATF
`aee_wdt_dump` hang (cpu0 stuck, no console output at all), then two
clean-looking serial boots that never brought up USB gadget networking
(`en12` stayed `inactive`, no device visible to `system_profiler` at all).
See boot.md "BUILD #200" for full detail. `/sys/fs/pstore/` was checked
after each recovery and found empty — no crash record survives, consistent
with the failure being ATF's own pre-Linux hang detector (which never
reaches Linux's panic/oops path) combined with the physical power-cycles
likely dropping DRAM self-refresh.

**Working hypothesis:** this is a timing-dependent race in how/when LK's
leftover DSI engine state (from framebuffer/logo display during boot) gets
touched by Linux's `mtk_dsi` driver — not a deterministic bug reachable by
re-reading patch 0012's C code, since the same image produces different
outcomes run to run. Patch 0012 is held out of the default patch stack
until this is understood; `boot2` is back on the known-good build #71
baseline.

**Update 2026-07-09 — stress test result: 0008-alone (no 0012) also hangs, same signature. Race is upstream of both DSI patches.**

Built #203 (`logs/2026-07-09-203-b17-0008-only-plus-pstore-trace`, banner #73,
sha256 `3eda93ee165eb4cb6a37fa2d6eab5647483df618d61e655af7ba5d46f0f87344`):
patch 0012 held out entirely (only 0008 in the drm stack), plus a new debug
patch (`patches/v6.6/zz-debug/0002-GEMINI-DEBUG-dsi-irq-poweron-poweroff-trace.patch`)
adding unconditional `pr_info` trace points around `mtk_dsi_irq()` entry/exit
and `mtk_dsi_poweron`/`poweroff` entry/exit, relying on
`CONFIG_PSTORE_CONSOLE=y` to mirror them into `/sys/fs/pstore/console-ramoops-0`
even across a warm (watchdog) reset.

Capture `logs/2026-07-09-204-b17-0008-only-plus-pstore-trace-boot.log`
spans two power-on sessions:

- **Session 1** (first ~1880 lines): booted clean on serial, reached the
  normal `mtu3` mux-switch cutoff, `RGU STA: 0` / "WDT does not trigger
  reboot" confirms this was a genuine cold start, not a post-hang reset. But
  `en12` never enumerated (`status: inactive`, no USB device visible to
  `system_profiler` at all) even after ~45s of polling — same silent
  gadget-networking failure seen twice on build #200.
- **Session 2** (device power-cycled again): the *new* capture opens with
  `RGU STA: A0000000` / `"SW reset with bypass power key flag"` /
  `"[PLFM] WDT reboot bypass power key!"` — meaning **session 1's kernel
  silently hung and self-reset via hardware watchdog sometime after we
  stopped polling it**, even though its serial log looked clean up to the
  `mtu3` cutoff. Session 2 itself then hangs identically: `el3_exit` at
  4.324s, **no kernel console output at all** (not even the `Linux version`
  banner), then `aee_wdt_dump: on cpu0` at 14.327s with
  `pc == lr == 0xffffffc000087fa8` — byte-for-byte the same hang address
  seen on build #200's first attempt. No `GEMINI-DEBUG` trace lines appear
  anywhere in either session, meaning the kernel never got near
  `mtk_dsi_irq()`/`mtk_dsi_poweron()` before hanging.

**Conclusion: this is not a patch 0012 regression.** Patch 0008 alone,
predating 0012 entirely, reproduces the identical early cpu0 hard-lock
(same hang PC, same `aee_wdt_dump` signature) and the identical silent
USB-gadget failure. The race is upstream of both DSI patches — most likely
in how early cpu0 boot collides with LK's leftover DSI/display engine
state, independent of what either patch does once Linux's own `mtk_dsi`
driver code starts running (trace evidence shows the hang precedes that
code executing at all in the fatal case). This reframes the investigation:
re-reading/adjusting `mtk_dsi.c` further is unlikely to fix it, since the
hang is already over by the time that code would run.

**Next steps (agreed 2026-07-09, not yet executed):**
1. Stress-test patch 0008 *alone* (no 0012) across several consecutive
   power cycles — 0008 predates this regression and has been assumed
   stable, but was never specifically stress-tested back-to-back the way
   0012 just was. If 0008-alone is also flaky, the race is upstream of
   0012 entirely and this investigation has been chasing the wrong patch.
2. Add unconditional (non-ratelimited) debug `printk`/`pr_info` trace
   points around `mtk_dsi_irq()` entry, the RACK write, the poll result,
   and `mtk_dsi_poweron`/`poweroff` entry/exit — tagged so they're
   greppable, and using plain printk (not `dev_err_ratelimited`) so a
   storm isn't suppressed. Since `CONFIG_PSTORE_CONSOLE=y` is already
   enabled, these should land in `/sys/fs/pstore/console-ramoops-0` even
   across a *warm* reset (watchdog-triggered), letting us revert to the
   baseline image afterward and read the trace without needing to catch
   it live over UART. Caveat: this only works if DRAM self-refresh
   survives whatever reset path fires — a full physical power-cycle by the
   user appears to wipe it (observed empty pstore after build #200's
   attempts), so where possible prefer waiting for/triggering the
   automatic watchdog reset rather than manually power-cycling, to
   maximize the chance the trace survives to be read.
3. Once forward progress is possible, consider the GIC pending/active-IRQ
   dump technique from the parked bare-metal plan (`SPI 229`
   pending/active state at hang time) to identify definitively whether
   the same DSI IRQ is implicated across all three failure modes.

## Update 2026-07-09 (later) — hang re-attributed: the "early cpu0 hard-lock" was almost certainly the ANDROID kernel, not ours

Resolving the recorded hang address against build #203's `System.map`
(`logs/2026-07-09-203-b17-0008-only-plus-pstore-trace/System.map`) overturns
the conclusion above:

- Our Linux 6.6 kernel's text starts at `0xffff800080000000` (`_text`,
  confirmed from System.map; all 171k symbols live in `0xffff8000...`).
  The hang PC `0xffffffc000087fa8` is **not in our kernel's address space at
  all.**
- `0xffffffc000080000` is exactly the text base of the **pre-4.20 arm64 VA
  layout used by 3.18-era kernels** — i.e. the vendor Android kernel
  (offset `0x7fa8` from text start, inside early head.S-era setup code).
- The same hang session contained `[LK]jump to K64 0x40080000` — the
  **Android `boot` partition's kernel load address**, not boot2's
  `0x40200000`. Every clean session in these captures shows
  `jump to K64 0x40200000` followed by our correct banner.
- Android boots are consoleless by design (LK hardcodes
  `printk.disable_uart=1`), which fully explains "el3_exit then total
  silence then aee_wdt" — no need to hypothesize our kernel dying before
  console init.

**Revised hypothesis:** on the "hang" cycles, LK selected/fell back to the
Android `boot` partition (boot-partition selection on the Gemini is
power-on-button-combo dependent, and LK may also fall back after a WDT
flag), and the *Android 3.18 kernel* hung and watchdog-looped. Patch 0008
(and 0012) may never have hung at all — every capture where LK jumped to
`0x40200000` booted our kernel cleanly on serial. The "byte-identical hang
PC across builds #200 and #203" is explained trivially: both were the same
Android kernel image.

**What survives of B-17 regardless of the above:** the silent
USB-gadget-enumeration failure. Now observed on three genuinely-clean boots
(2x build #200, 1x build #203 session with `RGU STA: 0`, correct #73
banner, clean serial log to the `mtu3` cutoff): `en12` stays
`status: inactive`, `system_profiler SPUSBDataType` shows **zero** matching
USB devices (so the device never appears on the Mac's USB bus at all — not
an IP-config issue), SSH times out. This is independent of any hang and is
now the primary open B-17 question. Also unresolved: blockers.md's earlier
claim that build #203 session 1 "silently hung after polling stopped"
(post-hoc `RGU STA: A0000000`) — that WDT flag could equally have been set
by a subsequent Android-fallback cycle; treat as unconfirmed.

**Evidence-handling lesson:** the raw hang captures were lost —
`ftdi-monitor.py` was relaunched to the *same* log path each power cycle,
overwriting prior sessions (only the final clean session survives in
`logs/2026-07-09-204-...`). Per the existing logging rules, every capture
attempt must get a fresh `NN`-numbered filename.

**Revised next steps (2026-07-09):**
1. **Button-controlled power-on test** (zero-build; build #203 still on
   boot2): several consecutive power-ons deliberately selecting the Linux
   boot entry, fresh log file per attempt, checking the `[LK]jump to K64`
   address in each. If `0x40200000` boots are always clean and only
   `0x40080000` cycles hang, the hang half of B-17 closes as a
   boot-selection artifact.
2. **Read the AEE crash record from the `expdb` partition** via mtkclient
   (`mtk r expdb expdb.bin` — targeted read, safe): MTK's AEE writes
   watchdog dumps there, so the hang cycles' register/stack dumps are
   likely recoverable and symbolicatable against a vendor 3.18 System.map
   (buildable from `gemini-android-kernel-3.18` in the VM).
3. Investigate the USB-gadget enumeration failure as its own thread
   (likely `mtu3`/UART-USB mux timing) — it will not be fixed as a side
   effect of display work.

## Update 2026-07-10 — B-17 crash ROOT-CAUSED and FIXED (OVL leftover IRQ → NULL-deref panic); flip_done timeout is the remaining display blocker

Full narrative and evidence in boot.md ("B-17 ROOT-CAUSED AND FIXED",
2026-07-10). Summary:

- **#203 never reached userspace** (journal on shared rootfs: all 10 boots
  banner #5). Its crash hid in the 0.5–6s window behind the mtu3 console-mux
  cutoff. Disabling USB entirely (build #218, banner #76) kept serial alive
  and captured the death: **NULL deref in interrupt context** —
  `mtk_disp_ovl_irq_handler → mtk_crtc_ddp_irq → mtk_crtc_ddp_config`
  dereferencing `crtc->state` (NULL until the first atomic commit). LK
  leaves the OVL scanning out the splash with `OVL_INTEN` armed; probe
  requests the IRQ and the leftover interrupt fires mid-bind. Panic → WDT →
  Android fallback (this also finally explains the fallback boots).
- **Fixed by `patches/v6.6/drm/0012`** (quiesce OVL_INTEN/INTSTA at probe +
  NULL-state guard in the IRQ path). Build #221 (banner #77) validated:
  no oops, kernel alive 93+s, DRM bound, DSI IRQ clean.
- **pstore/ramoops is a dead end on this device**: the preloader
  re-initializes DRAM on every boot path (proven by pmsg-marker and
  sysrq-panic tests with readout build #212/banner #75, even at the
  vendor's own `mediatek,pstore` address 0x44410000). Crash capture
  strategy = serial visibility via no-USB debug builds, not RAM.
- **Remaining B-17/display blocker:** `flip_done timed out` / vblank wait
  timeouts — OVL frame-done never fires after enable; no frames flow, panel
  dark, fbcon commit-retry loop appears to stall boot before systemd.
  Lead: verify DSI video-vs-command mode, TE/trigger and mutex SOF against
  vendor 3.18 source (LK log says `vido_mode`).
- The B-17 *gadget* half (silent USB failure on clean boots) is unchanged;
  the FTDI-first-then-swap cable protocol remains the reliable workaround.

**Cleanup state:** boot = readout #212 (#75), boot2 = fix-validation #221
(#77, no USB). Before resuming normal work: rebuild with
`configs/gemini-usb.config` restored (currently parked as
`.disabled`; delete `configs/gemini-nousb-debug.config` and also remove it
from the VM — build-pack rsync doesn't `--delete`), and reflash baseline.

## Update 2026-07-10 (later) — flip_done: three stacked defects fixed, pipeline now error-free but frameless; register-dump diagnostic built

Working the remaining flip_done/vblank timeout, three real, cumulative
defects were found and fixed (full evidence in boot.md builds #223–#229):

1. **Mutex EOF bits** — `mt6797_mutex_driver_data` borrowed
   `mt2712_mutex_sof`, which sets only the SOF field; vendor
   `ddp_get_mutex_src()` sets SOF *and* EOF to DSI0 for a video-mode path,
   and without EOF the OVL never receives frame-done. Fixed in
   `patches/v6.6/soc/0001` (new `mt6797_mutex_sof[]`, `SOF | SOF<<6`,
   mirroring mainline mt8183). Build #223 — necessary but not sufficient.
2. **LK leftover DSI video mode** — `DSI_MODE_CTRL` left in video mode by
   the splash survives `mtk_dsi_reset_engine()`, making every panel init
   command time out (-62) in `mtk_dsi_host_transfer()`. Fixed in
   `patches/v6.6/drm/0013` (force cmd mode in `mtk_dsi_poweron`). Build
   #225 — **validated**: init sequence now completes (187 CMD_DONE IRQs).
3. **MIPI-TX PLL sleeping in atomic context** — our mt6797 mipitx PHY had
   `usleep_range()` in clk `.enable` (runs under the CCF enable spinlock);
   `BUG: scheduling while atomic` in every display boot. Fixed in
   `patches/v6.6/phy/0004` (ops moved to `.prepare/.unprepare`, as mt8173
   mipi_tx). Build #227 — **validated**: BUG gone (capture 228).

Despite all three, flip_done ×9 / vblank ×4 persist: the pipeline
configures with zero software errors but no frame-done interrupt ever
arrives. Next step is visibility, not another guess:
`patches/v6.6/zz-debug/0003-GEMINI-DEBUG-ddp-register-dump.patch` (build
#229, banner #81) dumps raw mmsys/mutex/OVL0/RDMA0/DSI0 registers at t≈8s
and t≈8.5s while the stuck commit holds the pipeline powered — the
interpretation matrix is in boot.md's BUILD #229 entry. Awaiting flash and
capture 230.

## Update 2026-07-10 (evening) — flip_done/vblank timeout ROOT-CAUSED: OD0 misconfiguration (mainline mtk_od_config clobbered by mtk_dither_set)

The register-dump campaign (builds #229–#241, boot.md captures 230→242)
walked the failure to a single engine:

1. Eliminated in hardware, in order: mmsys routing table (soc/0002,
   verified mux-by-mux), OVL cascade wiring, layerless-frame limitation
   (layer poke, capture 234), MM CG clock gating, engine clock rate
   (mm_sel = 325 MHz from imgpll, capture 236), wedged-OVL0-FSM from LK
   handoff (OVL_RST poke, capture 238).
2. Capture 238's OVL FLOW_CTRL_DBG decode (vendor ovl_printf_status):
   both OVLs stuck in eng_act with out_valid=1/out_ready=0 → downstream
   backpressure, not head-engine failure.
3. Capture 240's mid-chain pixel counters: OD0 IN_CNT frozen while its
   OUT_CNT free-runs — OD0 blocks its input, self-generates output
   (which is why DSI streamed and RDMA0 underflowed).
4. **Root cause:** OD_CFG[1:0] is the mode field (vendor
   common/od10/ddp_od.c: 0x1 relay/bypass, 0x2 core-en; vendor default
   0x1). Mainline `mtk_od_config()` writes OD_RELAYMODE, then
   `mtk_dither_set()` overwrites OD_CFG with DISP_DITHERING only →
   hardware ends at 0x4 = dither on, no mode. MT8173 tolerates this;
   MT6797's OD (needs table/DRAM init mainline never does) does not.
5. **Confirmed live (capture 242, build #241):** poking OD0_CFG=0x5
   (relay+dither) at 8.7 s instantly completed the stuck atomic commit —
   fbcon bound (`fb0: mediatekdrmfb`), panel registered, plane enabled
   (OVL0 SRC_CON=0x1), old FME_UND/ABNORMAL_SOF signature gone.

**Fix to write:** `patches/v6.6/drm/0014` — preserve the relay bit in
`mtk_od_config` (OD_CFG = OD_RELAYMODE | dithering). Upstream candidate.

**New follow-on blocker (next up):** ~0.4 s into first real scanout,
right after `clk: Disabling unused clocks`, the system bus-hung and
WDT-reset to the Android slot (capture 242). Suspects: (a) SMI larb0
IOMMU-bypass gap (the earlier B-13 vendor-source finding) biting on
first real layer DMA — new latched layer-0 FIFO-underflow bit fits;
(b) late clk cleanup gating a now-needed clock (imgpll refcount). Plan:
build with drm/0014 + `clk_ignore_unused` temporarily restored to
separate (a) from (b) in one flash.

**Update 2026-07-10 (night) — flip_done FIXED and validated (capture 244, build #243):**
`patches/v6.6/drm/0014` (re-assert OD_RELAYMODE after mtk_dither_set)
confirmed: fbcon binds at 1.0 s, zero flip_done/vblank timeouts, full
boot to graphical.target in 10 s with the display stack enabled. The
capture-242 scanout WDT-reset did not reproduce with `clk_ignore_unused`
(TEMPORARY, in configs/gemini-cmdline.config) → the crash suspect is
late clk cleanup, not the SMI larb0 gap. Remaining before this blocker
closes: (1) find which clock clk_disable_unused kills during scanout and
hold a proper reference (then remove the temporary flag); (2) confirm
pixels on the physical panel; (3) strip zz-debug/0003 and restore the
production USB config for a clean production build.

**Update 2026-07-10 (late night) — panel-dark follow-up:** on the
milestone build #243 boot, the physical panel shows **backlight lit,
image black** (PWM confirmed enabled at 200/255 via serial login;
systemd-backlight not the culprit). Pixels aren't reaching the glass
despite a fully clean pipeline → active suspect is DSI video
timing/format mismatch vs. the panel's requirements. LK's own DSI
register dump in every capture is the golden reference; /dev/mem is
blocked, so build #245 (banner #89) extends the zz-debug dump to the
full DSI block 0x000–0x1AC for a kernel-vs-LK register diff. See
boot.md "BUILD #245".

**Update 2026-07-10 — DSI diff complete (capture 246), fix candidate
built (build #247):** kernel-vs-LK register diff shows format/lanes/
resolution all match; mismatches are (1) TXRX_CTRL — kernel disables EOT
packets and runs continuous HS clock where LK enables EOT + non-continuous
clock (mtk_dsi's DIS_EOT logic is inverted vs the flag name), and (2)
vertical/horizontal porches (panel patch used vendor-3.18 LCM values;
LK programs VSA=3/VBP=15/VFP=10 and wider horizontal blanking). Build
#247 (banner #90) updates panel/0005 to match LK on both. PHY_TIMCON
diffs deliberately deferred. See boot.md "CAPTURE 246 result".

**Update 2026-07-10 — controller theory exhausted; panel side
implicated (capture 248):** build #247's registers landed LK-identical
(TXRX_CTRL bit-for-bit, LK porches), link streams frames error-free,
still black. User confirms **LK's Planet logo displays, then goes dark
at kernel takeover** — panel HW and LK init are good; our panel
driver's reset/regulator/init takeover is the killer, or the panel
rejects our config internally. Build #249 (banner #91, zz-debug/0004)
adds differential DCS read-back (0x0a–0x0e) before our reset pulse (LK
state) and after our init, to name the failing step. See boot.md
"CAPTURE 248 result".

**Update 2026-07-10 — panel-dark ROOT-CAUSED (capture 250): TPS65132
bias never programmed.** DCS read-back: panel initialized, display ON,
booster OFF (0x0a=0x1c) — no analog rails. AVDD/AVEE come from a
TPS65132 on I2C1 @0x3e whose volatile VPOS/VNEG registers LK reprograms
every boot; our DTS had GPIO-only fixed regulators, so after the panel
driver's power-cycle the chip runs unprogrammed. Fix in build #251
(banner #92): mainline `ti,tps65132` regulator node (outp/outn 5.4 V,
enable-gpios 60/251) + CONFIG_REGULATOR_TPS65132=y. See boot.md
"CAPTURE 250 result".

**Update 2026-07-10 — second-layer bug: I2C1 combined transfers broken
(captures 252/253).** tps65132 probe timed out; interactive i2cdetect
proved the bus and chip fine but write-then-read (WRRD) transfers dead:
mt6797.dtsi i2c nodes fall back to the mt6577 compat (no auto-restart,
no aux-len). Fix `i2c/0001` (build #254, banner #93): match
"mediatek,mt6797-i2c" to mt8173 driver data (same IP generation,
confirmed against vendor mt6797 mt_i2c). Upstream candidate. See
boot.md "CAPTURE 252/253 result".

**Update 2026-07-11 — thick horizontal-band artifact appears on BOTH the
vendor-live-TIMCON build AND the plain mainline-formula build; not caused
by the TIMCON patch, and NOT a confirmed regression (no prior baseline
was actually documented).** Harvested live, steady-state `PHY_TIMCON0-3`
register values off the stock vendor 3.18 kernel while it was actively
driving the panel correctly (see boot.md "Full scatter-file recovery
reflash..." entry) and hardcoded them in debug patch `zz-debug/0008` as a
controlled experiment (build #105, `logs/2026-07-11-279-...`). Result:
kernel boot fully clean (no flip_done/vblank timeouts, DSI bound, panel
registered, DCS reads all normal); physical panel showed thick,
regularly-spaced light-blue/black horizontal bands — real pixel content.
Photo: `logs/2026-07-11-279-dsi-timcon-vendor-live-values/panel-thick-bars-result.jpg`.

Initially treated as a regression and reverted (patch 0008 disabled,
rebuilt as `logs/2026-07-11-281-revert-timcon-back-to-baseline/`, banner
`#106`) — reflashed #106 (**pure mainline-formula TIMCON, confirmed by
reading the built tree directly, no override present**) and the user
confirmed on physical hardware: **"same horizontal lines as the previous
build."** D-PHY timing (`PHY_TIMCON0-3`) is therefore **ruled out** as the
cause — visually identical result with the vendor-harvested override and
without it. Further, boot.md was checked and contains **no prior entry
describing a "thin top-of-frame corruption" baseline** before this
session's own (now-corrected) write-up — the last actually-documented
visual state (#243/#245) was backlight-lit but fully **black**, not
corrupted. So there is no confirmed evidence this banding is a
regression at all; it may be the first real pixel content this panel has
shown under our own kernel.

Clock-domain cross-check remains valid context: `mm_dsi0_mm`/`mm_sel` =
325 MHz and PLL CON0 = `0xf0002001` are confirmed identical between our
build and the vendor's (build #235/#237) — consistent with the banding
being unrelated to clock/PLL/D-PHY-timing setup generally, not just the
TIMCON registers specifically.

**Next step:** redirect investigation away from D-PHY bit-timing,
toward: (1) OVL/DDP layer config — the coarse, regular band pattern is
consistent with a scanline-count/line-stride mismatch rather than a
signal-integrity issue; (2) panel init command sequence — check for a
missing/incorrect column/page-address-set or memory-write command
producing a short repeating pattern instead of a full frame; (3)
framebuffer/scanout stride — cross-check the documented 1088-px
GPU-aligned stride vs. the panel's native 1080px on this code path. A
deliberate reference photo of build #106 is still worth capturing, but
TIMCON tuning is no longer the active lead. See boot.md "BUILD #105 ...
vendor-live TIMCON experiment" and "BUILD #106 recheck" for the full
account and correction.

**Update 2026-07-11 (later) — lead #2 (DSI HSA/HBP/HFP word counts) also
closed off; DSI-level config now doubly confirmed correct, root cause is
upstream of DSI.** Same method as the TIMCON test: overrode
`DSI_HSA_WC`/`DSI_HBP_WC`/`DSI_HFP_WC` with LK's exact proven register
values (`0x1c`/`0x94`/`0x74`, capture 244) instead of mainline's
formula-plus-correction derivation, which the panel patch's own comment
admits only lands "within one byte" of LK's values (build #107,
`zz-debug/0009-...`, `logs/2026-07-11-283-.../`). Kernel-side register
readback confirmed the override applied exactly as intended, and also
confirmed `DSI_PSCTRL` word-count = 1080×3 (RGB888, native panel width, not
the 1088-aligned GPU stride) with `PACKED_PS_24BIT_RGB888` selected —
correct. Boot fully clean, no flip_done/vblank timeouts, no DSI/panel
errors. User confirmed on hardware: **"same orange white black horizontal
bars then fade to black"** — identical to builds #105/#106.

Two independent, hardware-verified-correct DSI-level fixes (D-PHY bit
timing, and now line word-counts) have produced **zero visible change**.
This rules out the DSI protocol/timing layer as the cause with fairly high
confidence — the DSI engine is provably transmitting a well-formed,
correctly-timed stream. **The problem is upstream: what the OVL/RDMA layer
is actually reading from memory and handing to DSI** (layer pitch/format/
address vs. what DRM/fbcon actually allocated, or CRTC blend config) is now
the primary suspect, since it has not yet been directly instrumented the
way the DSI registers have. Next action: extend the existing DDP debug
dump (`zz-debug/0003`/`0006`) to also capture the per-layer OVL registers
(`OVL_CON`, `OVL_ADDR`, `OVL_PITCH`/`HDR_PITCH`) so the live pitch/format/
address can be read back and cross-checked against the DRM/fbcon-side
allocation. See boot.md "BUILD #107" for full detail.

**Update 2026-07-12 — PANEL LIT: display path RESOLVED end-to-end (build
#132, banner #119, ⭐).** The banding/dark-panel saga is over; three
stacked defects, each individually necessary:

1. **Init-table packet type** (found 2026-07-11, `zz-debug/0020`): vendor
   `DSI_set_cmdq_V2` sends commands ≥0xB0 as GENERIC packets; we sent DCS.
   The whole manufacturer init table had been corrupt from the start —
   this was the actual cause of the banding artifact (not OVL, not DSI
   timing).
2. **D-PHY LP/turnaround timing** (found 2026-07-12, `zz-debug/0008`
   re-enabled): mainline's TIMCON formula yields LP windows ~40% shorter
   than LK's proven values; the panel answered with ACK+Error and LP reads
   failed. With LK's TIMCON0–3, DCS reads work for the first time.
3. **Link mode** (found 2026-07-12): the vendor *kernel* LCM driver runs
   this panel in SYNC_PULSE **video mode** (`LCM_DSI_CMD_MODE=0`); LK's
   command-mode splash misled the 07-11 command-mode pivot (which also
   reintroduced flip_done timeouts). Video mode restored.

Result on hardware: solid-white test fill visible on the glass, clean boot,
zero flip_done/vblank timeouts, graphical.target in 21 s. See boot.md
"BUILD #132". Remaining follow-ups: fbcon-on-glass check (build #134),
then productization — fold 0020 into `panel/0005`, derive/fold correct
TIMCON values properly (vendor formula, not hardcode), strip zz-debug
patches, re-enable USB gadget config, re-verify `clk_ignore_unused` removal.

**Update 2026-07-12 (evening) — PHASE 5 COMPLETE: readable text console on
the physical panel.** After first light (build #132), the residual banding
on structured content was root-caused with an RGB-thirds test pattern to a
video line-timing mismatch: the mode timings had been reverse-engineered
from LK's register dump, but LK drives the panel in command mode, so its
video porches are meaningless. Switching to the vendor kernel LCM driver's
video timings (HFP26/HSA4/HBP20, VFP76/VSA1/VBP43, 167333 kHz) produced a
perfect stable image (build #138) and then a readable landscape console
(rotate:3 + TER16x32, build #143). All fixes productized in build #145
(banner #126): folded into panel/0005 and drm/0015, zz-debug stripped, USB
restored, clk_ignore_unused dropped. See boot.md builds #136–#145.

---

## 🟢 B-18 — AW9523B keyboard enablement breaks USB gadget (SSH-over-USB) — RESOLVED 2026-07-13

**Symptom evolution (all 2026-07-12, Phase 6):** on every build with the
AW9523B GPIO expander enabled (`GPIO_AW9523B=y` + DTS node okay) AND
`USB_MTU3` enabled:
- #147/#157: with a USB host (Mac) attached at power-on → boot wedges
  before userspace (panel stuck at penguins, no gadget enumeration, no
  console visibility — serial dies at the B-15 mux switch t=0.45s).
- #159/#166: boot completes (login on panel, keyboard works on #166) but
  the gadget never enumerates on the Mac: no interface with the fixed
  host MAC 42:00:15:19:82:00, device unreachable at 10.15.19.82.
- Baseline #145 (no aw9523b) boots + SSH works with the same USB config,
  so the regression tracks the aw9523b bring-up, NOT key scanning (the
  keypad node was still status-disabled in #147–#157; the expander probe
  alone — soft-reset + register init + GPIO58/SHDN driven high — is the
  active ingredient).

**Suspects (untested):** GPIO58 (SHDN) high or the AW9523B INT line
(GPIO87/EINT10, now floating enabled) interacting with the left-port
USB-C mux / charger / CC logic when VBUS is present; check vendor 3.18
sources (aw9523_key.c power-up sequencing, USB-C mux GPIO usage) and the
vendor DTB for GPIO58/87 dual roles before the next hardware experiment.
Device-side dmesg of a failed-gadget boot (#166) was not captured — the
working keyboard cannot type `|`/`-`/`>` (no Fn layer yet) and serial is
dead on mtu3 builds; capture it once the Fn layer or a file-based
diagnostic exists.

**Decision (user, 2026-07-12):** disable USB entirely and operate over
the serial console + on-device keyboard: `configs/gemini-usb.config` →
`.disabled`, new `configs/gemini-serial-console.config` (USB_MTU3 off +
clk_ignore_unused + console=tty0 cmdline). With mtu3 off the B-15 mux
never switches, so ttyS0 (console + getty) works for the whole session —
USB-broken + mtu3-on would have left no remote access at all.
clk_ignore_unused is mandatory in this mode (build #153 wedged in the
unused-clock sweep without mtu3 holding SSUSB clocks).

**Root cause found and fixed 2026-07-13:** desk research (vendor
`aw9523_key.c` in gemini-android-kernel-3.18) showed the chip's INT pin
(GPIO87/EINT10) gets an explicit `bias-pull-up` pinctrl state selected at
probe (`aw9523_key_setup_eint()`). Our DTS already *defined* the matching
state, `aw9523b_pins` (`patches/v6.6/dts/0001-...patch`: SHDN/GPIO58
output-high + GPIO87/INT `bias-pull-up`/`input-enable`) — but it was never
referenced by any `pinctrl-0` property on the `aw9523b` i2c node, so it was
dead DTS: GPIO87/INT was left **floating**, right next to USB/mtu3 IRQ
activity, which is consistent with both failure modes (#147/#157 hard wedge
with a host attached at power-on; #159/#166 silent gadget non-enumeration).

**Fix:** add `pinctrl-names = "default"; pinctrl-0 = <&aw9523b_pins>;` to
the `aw9523b: gpio@5b` node. One-line functional change; regenerated the
three DTS patches touching `mt6797-gemini-pda.dts` (0001/0009/0011) via
apply-edit-rediff so their line-number context stays consistent
(`patches/v6.6/dts/0001-arm64-dts-mediatek-add-gemini-pda-board.patch`,
`.../0009-...ssusb-gadget.patch`, `.../0011-...smi-larb0-common.patch`).

**Verified on hardware, build #175 (banner #140,
`logs/2026-07-13-175-b18-aw9523b-pinctrl-fix/`):** clean boot to prompt,
keyboard works, USB gadget enumerates (`en12`, fixed MAC
`42:00:15:19:82:00`), ping + `ssh root@10.15.19.82` both succeed — all
three B-18 symptoms cleared in one fix, no diagnostic matrix needed.
`configs/gemini-usb.config` restored (from `.disabled`);
`configs/gemini-serial-console.config` retired to `.disabled` (its
USB-off/`clk_ignore_unused` fallback is no longer needed — mtu3 + keyboard
now coexist). **New baseline:** display + keyboard + USB gadget SSH, all
together, for the first time since Phase 6 began.

## 🟢 B-19 — WiFi Stage 1 Gate G1a: USB host mode — RESOLVED 2026-07-15 (build #248: RTL8156 ethernet adapter enumerates + SSH-over-LAN, no-hands cold boot verified)

**RESOLVED 2026-07-15.** Build #248's four baked-in #231 fixes all worked;
the final live root cause was **external charge power suppressing the
BQ2589x OTG boost** (chip enters charge mode, REG0B VBUS_STAT=001, OTG
bit dropped — no VBUS ever sourced, so every prior test with a charger
attached was doomed regardless of kernel state). With the charger
unplugged: RTL8156 USB-C ethernet adapter enumerates from a cold boot
with zero manual pokes, r8152 binds, DHCP, SSH from the Mac over the LAN
(192.168.100.144). Full timeline + rough edges (charger hot-plug kills
boost without self-resume — /root/h.sh recovers; one unexplained panic
on first reboot, pstore empty; rtl8156b-2.fw missing but works) in
boot.md "BUILD #248 flashed". Gate G1b's original purpose (SSH not via
the gadget) is satisfied by this ethernet path; WiFi-dongle work can
reuse it directly.

**History — resumed 2026-07-15 (user decision — CONSYS G2b hunt parked in turn):**
target is a USB ethernet adapter on the left port, giving SSH-over-LAN as
the debug channel for later WiFi work. Build #248 (sha256
`99bf2c1aa53a46348a55e0e43e1f898f594435dd99370dd7c4559450e2b76edb`) bakes
in all four #231 defects: (1) bq25890_vbus_enable re-asserts WD-off each
enable (power/0001); (2) autosuspend root-caused to Debian's
60-autosuspend.rules hwdb writing power/control=auto — countered by rootfs
udev rule 99-gemini-usb-host-pm.rules (both controllers + all USB devices
pinned "on"); (3+4) new DTS-gated `mediatek,force-usb-host` in phy/0001
(host IDDIG + SUSPENDM forced in u2 power_on — mtu3 never calls
phy_set_mode(), so mainline's host-role path is dead code here). dts/0012
re-enabled (dts/0013 EOF-context regenerated to apply after it);
gemini-usb.config back to the host build + usbnet adapter drivers
(CDCETHER/RTL8152/AX88179/AX8817X/RNDIS/SMSC95XX); rootfs also got DHCP
on any en*/eth* (not usb0) and re-staged /root/h.sh + /root/s.sh (s.sh now
reads the queued linestate monitor 0x11290870/74). New pre-build evidence:
**MT6351 VUSB33/VA10 rails proven ON live** via pwrap regmap on #247
(0x0A16=0xda62, 0x0A6E=0xda62, EN bits set) — the #231 "analog rails"
suspect is weakened. If #248 still shows Powered/Not-connected, the
linestate adapter-out-vs-in comparison at the panel console is the
deciding diagnostic. See boot.md "BUILD #248".

**History — parked 2026-07-14 → 2026-07-15:** build #231 exhausted the vendor-sourced GPIO/mux
candidates and four PHY/runtime-PM defects were root-caused without
producing a connect event. WiFi pivoted to the internal CONSYS path
(B-21) until G2b stalled (see B-21). Host-mode overlay was retired to
`patches/v6.6/dts/0012-...patch.disabled`; `configs/gemini-usb.config`
restored to the gadget build (build #233). Both reversals undone in #248.

**Symptom:** with `mtu3` in host/OTG mode (`patches/v6.6/dts/0009-...`),
`xhci-mtk` binds and the controller comes up without crashing, but a USB
stick plugged into the right-hand port never appears beyond the virtual
root hub in `lsusb`, and `/proc/interrupts` never shows a single fire on
the xhci IRQ (SPI 126) — consistent with no electrical connect event ever
being registered, across every build tried.

**Ruled out, in order (see boot.md builds #142–#148):**
- xhci sysfs device-name collision with the parent ssusb node (#142→#143
  fix: separate MAC-only register window for the xhci child).
- Pure `dr_mode="host"` never flipping the port0 U2/U3 mux, because that
  only happens via the OTG role-switch path (#143→#144 fix: `dr_mode="otg"`
  + `usb-role-switch` + `role-switch-default-mode="host"` — this also
  fixed a real "HC died" crash, but did not fix enumeration).
- Missing VBUS gating: GPIO94 (`usb1_drvvbus`) alone is not the real power
  switch — SW7226 (GPIO72) is a separate load-switch IC in series (#145
  fix, vendor-sourced from `usb_typec.c`'s `fusb300_eint_work()`).
- Missing FUSB301A-mux idle state: GPIO70/71 floating instead of the
  vendor's documented safe-idle values (#146 fix, same vendor source).
- FUSB340 USB3 redriver (GPIO251/252, a fourth, separate mux found in the
  vendor DTB's `usb_c_pinctrl@0` node) — **tried in #147, caused a real
  display regression** (panel went blank, no crash visible in the serial
  log up to the expected B-15 mux death) and was reverted in #148 (display
  confirmed restored on hardware). Either the vendor-DTB pin decode for
  GPIO251/252 is wrong, or those pins are genuinely shared with/gate the
  display power path — do not re-attempt without independent GPIO
  debugfs readback *and* a test that isolates display from USB, not both
  changed in the same build.

**Current state (build #148, vendor GPIO fixes 94/72/70/71 all present,
confirmed via `/sys/kernel/debug/gpio` readback on hardware):** every
GPIO-level gate documented in the vendor 3.18 source/DTB for this signal
path is now asserted correctly, and the stick still does not enumerate.
This is a materially different situation from B-18 (which was a genuine
one-line dead-pinctrl-reference bug) — the GPIO layer is very likely
exonerated at this point.

**Not yet investigated:** whether this is a physical issue (bad
cable/adapter/stick — should be tested with a second known-good stick and
cable before further kernel changes), or a deeper mtu3/xhci-mtk driver gap
specific to this SoC that isn't visible in the vendor 3.18 source (which
never ran xhci/host mode at all — Android only ever used gadget mode on
this port). Recommend a physical-layer sanity check (different stick,
different cable, multimeter/scope on VBUS at the connector if available)
before spending further flash cycles on driver-side theories.

**Blocks:** WiFi Stage 1 Gate G1a (plan.md Phase 8), and therefore Stage
1.2 (MT7921U dongle) and Gate G1b.

**Update 2026-07-13 (post build #148):** a second, different USB2.0 device
(SD card reader, VID:PID 349C:0418, confirmed via Mac-side `ioreg`) was
tested in the same port and also shows nothing beyond the root hub — rules
out "bad first stick" as an explanation. Next candidate, not yet tested:
cable/CC orientation. GPIO70/71 (`fusb301a_sw_sel`) hardcode the CC1
orientation default; if the physical connector is inserted CC2, that mux
may not be a simple polarity swap but could route D+/D- to nothing at all
in the "wrong" orientation. Zero-risk test recommended before any further
kernel changes: flip the cable/adapter 180° and recheck `lsusb`.

**Correction 2026-07-13 (confirmed with user): wrong physical port tested
throughout builds #142-148.** The Gemini PDA has two physical USB-C ports.
`mtu3`/xhci-mtk (all the GPIO/mux work in this blocker) is wired to the
**left** port — the same one used for UART/FTDI (B-15's `FORCE_UART_EN`
mux bit lives inside mainline `mtk-tphy`'s PHY init for this exact
connector) and for gadget SSH (build #175). The **right-hand** port —
where every test device in this blocker was plugged in — is driven by the
separate legacy `usb1@11200000` MUSB-style controller
(`mediatek,mt6797-usb11`), which has **no mainline driver** (hardware.md,
driver_ports.md). Nothing was ever going to enumerate there regardless of
GPIO state. `plan.md`'s Gate G1a instructions ("right-hand port") were
written before this was verified against physical hardware and are wrong.
**Next step: retest with the SD card reader in the left port** (single
cable-swap with the FTDI rig, same protocol as gadget-SSH verification).

**Update 2026-07-13 (build #150): likely root cause found -- CC-less
adapter cable.** Enabled the existing (previously unused) FUSB301A driver
(`patches/v6.6/usb/0001-...`) as an I2C-only diagnostic (not wired to
usb-role-switch). With the SD card reader plugged into the confirmed-correct
left port via a **USB-C-to-USB-A adapter**, the chip itself reports
`status=0x00 type=0x00 cc=CC1 role=0` -- i.e. the ATTACH bit has never been
set. A bare/passive C-to-A adapter typically carries no CC pins at all
(just VBUS/GND/D+/D- passthrough), so this is consistent with the cable
itself never presenting a valid Rp/Rd to the FUSB301A, independent of any
kernel/GPIO work. The vendor's own fusb300_eint_work() only asserts the
VBUS/mux switch GPIOs *after* confirming CC attach over I2C -- our static
"skip negotiation, assert post-attach state" approach may not be
sufficient if the physical switch ICs (sw7226/fusb301a-sw) have any
hardware interlock tied to genuine CC attach, separate from the GPIOs we
drive from Linux. **Next step (zero kernel changes): retest with a native
USB-C storage device, or a C-to-A adapter/dongle confirmed to implement
real Type-C CC signaling**, before any further driver/DTS work on this
gate.

**Stopping point 2026-07-13 (Gate G1a paused, not resolved).** Final test:
enabled the FUSB301A driver's DFP-mode write and re-probed with a
**native USB-C dongle** (MediaTek network dongle, proper Type-C plug, no
adapter) on the confirmed-correct left port. Result: identical
`status=0x00 type=0x00 cc=CC1 role=0` to the CC-less USB-A adapter test --
ATTACH never asserts regardless of device or cable. This rules out the
CC-less-adapter theory too.

**Summary of everything ruled out this investigation (builds #142-150):**
- xhci sysfs device-name collision (fixed, #143)
- dr_mode host-only never flipping the port mux (fixed, #144 -- also fixed
  a real "HC died" crash)
- Missing VBUS gating (GPIO94, sw7226/GPIO72) -- confirmed correctly
  asserted (#145)
- Missing FUSB301A-mux idle state (GPIO70/71) -- confirmed correctly
  asserted (#146)
- FUSB340 redriver (GPIO251/252) -- caused a real display regression,
  reverted (#147/#148); dead end, not a safe lead
- Wrong physical port (right-hand, driven by the driverless legacy
  `usb1@11200000` MUSB controller) -- corrected to the left port (mtu3),
  confirmed via B-15/gadget-SSH precedent
- Bad test device -- ruled out with two different devices (SD reader,
  network dongle)
- CC-less adapter cable -- ruled out with a native USB-C dongle

**Remaining unknown:** the FUSB301A's `MODE` register write
(`regMode = 0x04`, meant to force DFP/host mode) is explicitly flagged
`FIXME`/unverified in the driver source -- it was reverse-engineered from
the vendor 3.18 driver's usage, not from a real ON Semiconductor FUSB301
datasheet. If that encoding is wrong, the chip may never actually enable
CC toggling/attach detection, which would explain `ATTACH=0` regardless of
what's plugged in. **Do not attempt further register-level changes to
this driver without the real FUSB301A/FUSB301 datasheet** (or the vendor
Android BSP driver's register `#define`s, which were not consulted for
this field -- only the call-site logic was). Paused here rather than
continue guessing bit patterns blind.

**Reverted to known-good baseline:** flashed back to build #175
(banner #140, `logs/2026-07-13-175-b18-aw9523b-pinctrl-fix/`, sha256
`d34d58474bca24a851eda4c93ac660aada268c8cb3de1f231d44b00d7c7883c8`) --
keyboard + display + USB gadget SSH all working together. All of builds
#142-150's DTS/config changes (dr_mode=otg, xhci child node, VBUS/sw7226/
fusb301a-mux/fusb340 gpio-hogs, FUSB301A diagnostic) remain in
`patches/v6.6/` for whenever this gate is resumed, but are NOT in the
currently flashed image.

**Blocks:** WiFi Stage 1 Gate G1a (plan.md Phase 8), and therefore Stage
1.2 (MT7921U dongle) and Gate G1b. Phase 8 networking is otherwise fully
functional today via gadget SSH (build #175 baseline).

**Update 2026-07-14 — LIKELY EXPLANATION FOUND (vendor-source harvest +
live verification, research.md "USB Left-Port PHY & Type-C Harvest"):**
1. **Wrong chip all along.** The Gemini has TWO FUSB301 chips at 0x25:
   i2c0 (vendor node `fusb301a@25`) serves the RIGHT port and its
   OTG/HDMI muxes; **i2c1 (`fusb301@25`) is the LEFT port's CC
   controller** — proven live on #177 by plugging the Mac into the left
   port: i2c1 Status=0x2b (ATTACH/VBUSOK/CC2), i2c0 Status=0x00. Every
   Stage 1 experiment read/wrote the i2c0 chip, so "ATTACH never asserts"
   was the truthful state of the empty right port.
2. **Real register map recovered** (vendor `fusb301.h`): Mode is reg
   0x02 (SOURCE=0x01, SINK=0x04, DRP=0x10); the old guessed "0x04 MODE"
   write hit the Manual register. Both chips power up in Mode=0x04
   (SINK), so a downstream device presenting Rd is invisible — consistent
   with zero connect events at G1a.
3. **Vendor DID ship host mode on this port** (contrary to earlier
   note): known-good config has `CONFIG_USB_XHCI_MTK=y`, and the DTB's
   `usb3_xhci@11270000` has an `usb_iddig_bi_eint` (EINT 181) — vendor
   host/device switching keyed off IDDIG, not CC.
   **Next steps when resumed:** program i2c1 chip Mode=0x01 (SOURCE),
   retest left-port enumeration; trace how IDDIG is generated on a
   Type-C port before more role-switch DTS work.

**Update 2026-07-14 — Stage C Phase 1 (vendor IDDIG/VBUS harvest) complete
(research.md harvest §7).** Key facts:
- Vendor host-mode trigger = pure ID-pin OTG: EINT 181 level-low →
  debounced `mtk_xhci_mode_switch()` loads xhci and enables VBUS; no CC
  logic anywhere in the host path. Who drives IDDIG low on a Type-C
  connector is still unproven (i2c1 FUSB301's eint handler is a stub;
  FUSB301 has no legacy ID output pin) — to be resolved empirically in
  Phase 0 by watching EINT181 while attaching a sink with the chip in
  SOURCE mode.
- **Left-port host VBUS = RT9466 charger OTG boost** (`set_chr_enable_otg`
  → CHG_CTRL1 reg 0x01 bit0 OPA_MODE, chip at i2c0 0x53), NOT the MT6351
  PMIC (`CONFIG_MTK_OTG_PMIC_BOOST_5V` unset in the known-good config).
  Mainline `rt9467-charger.c` exposes exactly this boost as a
  `usb-otg-vbus-regulator` — clean Phase 2 shape is RT9466 node +
  regulator as `vbus-supply` of ssusb (caveat: driver hard-requires its
  IRQ, the B-11 EINT gap — patch it optional or fix B-11).
- ~~**Phase 0 zero-kernel live test (next, needs hardware):** on #225 over
  FTDI serial — i2c1 0x25 Mode(0x02)=0x01 SOURCE, RT9466 boost on via
  i2c0 0x53 reg 0x01 bit0, plug real-Type-C sink into left port, read
  Status(0x11)/Type(0x12) expecting ATTACH=1, and check EINT181 level.~~
  **DONE 2026-07-14 — see below.**

**Update 2026-07-14 — Stage C Phase 0 COMPLETE: full left-port CC+VBUS
chain proven live, zero kernel changes** (research.md harvest §8, logs
`2026-07-14-227..230-b19-phase0-*.log`). Run over gadget SSH with staged
scripts (serial console is unusable on #225 — see B-20 note below). The
working recipe, all three elements required:
1. **FUSB301 i2c1 0x25 Mode(0x02)=0x01 SOURCE** → ATTACH=1 + Type=SINK for
   real devices, both CC orientations verified.
2. **BQ25896 OTG bit (i2c0 0x6b REG03 bit5) with I2C watchdog disabled**
   (REG07[5:4]=00; the 40s WD silently resets REG03 otherwise).
3. **GPIO107 HIGH** (`GPIO_OTG_DRVVBUS_PIN`, aeon dws) — the BQ25896 boost
   is pin-AND-register gated and fails silently (no fault) when low; LK
   hands it over low. This was the final missing piece.
   Result: VBUS_STAT=111, VBUS ADC 5.0V, device LEDs lit.
- **Charger correction: the device has a TI BQ25896 at i2c0 0x6b
  (REG14=0x06), NOT an RT9466** — nothing at 0x53 on any bus. All RT9466
  references in hardware.md/Phase 7 corrected; mainline driver =
  `bq25890_charger.c` (`ti,bq25896`), which also exposes the boost as a
  `usb-otg-vbus` regulator.
- **Remaining for Stage C:** Phase 2 build (host-mode DTS + FUSB301A
  driver rewrite for i2c1/real regmap + bq25896 node + GPIO107 + gating
  the B-20 force-b-session-valid in host role), then Gate G1a enumeration
  test. IDDIG (EINT 181) still untraced — may be unnecessary with
  role-switch-default host.

**Side-finding 2026-07-14 (belongs to B-20/B-15 ledger):** on build #225
the serial console is dead on EVERY boot (FTDI protocol included) — the
forced session-valid bits hold the PHY pads in USB mode; #226's "FTDI
regression clean" capture actually ends at 0.447s (mtu3 probe). Worse, a
boot with the FTDI rig attached at power-on appeared to hang at
`clk: Disabling unused clocks` (panel confirmed stuck, not just serial
loss); boot with NO cable then hot-plug works. serial-login/serial capture
are unavailable on #225 until the force bits are gated by role or DTS knob.

**Update 2026-07-14 — Stage C Phase 2 patches drafted (not yet built):**
- `patches/v6.6/usb/0001` REWRITTEN for the real chip/regmap: binds the
  **i2c1** left-port FUSB301, Mode reg 0x02 = 0x01 SOURCE, Status
  0x11/Type 0x12 decode per the live Phase 0 verification. Polling (500ms,
  B-11 = no EINT), dev_info on CC change, SINK restored in
  shutdown/remove for vendor-chain handoff.
- NEW `patches/v6.6/power/0001-power-bq25890-allow-probe-without-irq.patch`:
  mainline `bq25890_charger.c` hard-fails probe without an IRQ; patched to
  warn-and-continue (B-11). Driver already disables the 40s watchdog in
  hw_init (F_WD=0) — matches the Phase 0 recipe.
- NEW `patches/v6.6/dts/0012-arm64-dts-mediatek-gemini-left-port-host-mode.patch`
  (applies after dts/0009, keeps 0009 gadget baseline intact for easy
  revert): (a) i2c0 charger node corrected to `ti,bq25896`@0x6b with the
  seven required ti,* props (defaults matching live-observed hardware
  values) and an `otg_vbus: usb-otg-vbus` regulator child; (b) i2c1
  `typec@25` FUSB301 node enabled; (c) GPIO107 gpio-hog output-high
  (`otg-drvvbus`); (d) ssusb → dr_mode="otg" + usb-role-switch +
  role-switch-default-mode="host" + xhci child @0x11270000 SPI 126 (the
  #142 sysfs-collision and #143 pure-host-mux lessons baked in),
  vbus-supply=<&otg_vbus>; (e) **B-20 `mediatek,force-b-session-valid`
  REMOVED from u2port0** in this build — it pins the PHY to device role
  (and killed serial); serial console should return to normal B-15
  behaviour. The old GPIO94/sw7226/fusb301a_sw hogs from #144-146 are NOT
  revived (right-port wiring); FUSB340 GPIO251/252 hogs stay excluded
  (display regression, #147).
- `configs/gemini-usb.config` updated: MTU3_DUAL_ROLE, XHCI_MTK,
  TYPEC_FUSB301A, CHARGER_BQ25890, plus usb-storage/usbnet class drivers
  so G1a devices bind.
- Validated: full patch stack applies clean on pristine v6.6; board DTS
  compiles with dtc. Next: /build-pack, regression-check keyboard+display
  (+serial return), then Gate G1a (SPI 126 count + lsusb beyond root hub).

**Update 2026-07-14 (late) — build #231 flashed and live-debugged: all
Phase 2 software works, Gate G1a still not passed.** Full session detail
in boot.md "BUILD #231 flashed". Regression gate passed (boot, display,
keyboard, FTDI-attached boot no longer hangs); FUSB301 driver, bq25890
regulator, dual-role mtu3 + xhci root hub all live. Device never
enumerates: portsc stuck "Powered Not-connected", SPI 126 never fires.
Four real defects found and worked around by hand (all must be fixed in
build #232):
1. bq25890 boost state lost post-boot (REG03/REG07 reset; harden
   `bq25890_vbus_enable` to rewrite WD-off + OTG each enable);
2. runtime PM autosuspend clears IPPC HOST_SEL and power-cycles the PHY
   — with no USB wakeup wired this permanently kills connect detection
   (disable autosuspend for ssusb/xhci in the build);
3. U2PHYDTM1 needs explicit host forcing (FORCE_IDDIG|RG_IDDIG=0 =
   0x200); LK leftovers differ by boot cable (0x43E2E FTDI / 0x0 clean);
4. U2PHYDTM0 SUSPENDM=0 on clean handover — PHY analog asleep; forced
   FORCE_SUSPENDM|RG_SUSPENDM.
Even with all four fixed live, no linestate: ruled out usb2uart mux,
usb2jtag mux, ACR4 GPIO mode, ACR6 config, GPIO70/71, orientation.
NEXT: PHY linestate monitor (0x11290870/74) adapter-out vs -in to split
MAC-side break vs analog-blind; prime remaining suspect = MT6351 PMIC
PHY rails (VUSB33/VA10, no mainline driver) per vendor
`usb_phy_recover()`, the canonical host U2-PHY bring-up sequence.
Debug tooling: `/root/h.sh` + `/root/s.sh` on the rootfs (survive kernel
reflashes; see boot.md). — USB gadget enumeration intermittently dead: root cause = U2PHYDTM1 session-valid FORCE bits (RESOLVED 2026-07-14, build #225)

**Opened 2026-07-13 (late).** The #175/#177 gadget baseline (verified
working twice on 2026-07-13: morning #175, and once mid-evening #177 with
UDC `configured` + SSH) intermittently boots with the mtu3 UDC stuck at
`not attached` and ZERO enumeration on the Mac — across kernels (#175
content), rootfs (pristine and rebuilt), Android bounces, cold boots and
hot replugs. Android itself always enumerates (`Gemini_4G`), so the
hardware path is intact.

**Ruled out (evidence in boot.md "BUILDS #176/#177 + B-20"):**
- Kernel content (byte-identical to verified-working #175)
- Rootfs (fails on pristine hash-verified image; failure is below the
  rootfs layer — no enumeration at all)
- FUSB301A state: register dump with the REAL vendor map shows a perfect
  attach (Status=0x2b: ATTACH=1 VBUSOK=1 ORIENT=CC2; Manual=0x00 — the
  Stage 1 "0x04 = MODE" write theory is dead, 0x04 is the Manual reg and
  it is clean)
- GPIO70/71 mux pins: vendor source proves they belong to the USB1
  (right-port) OTG/HDMI path; driving all 4 states changed nothing.

**Correlation:** every break followed an mtkclient preloader/DA session;
the one mid-evening recovery followed FTDI-serial boots + live cable
swap. Not yet causally explained.

**Prime suspect:** the B-15 left-port UART/USB console "mux" is the
MT6797 U2 PHY's usb2uart function (FORCE_UART_EN/RG_UART_EN in the PHY
DTM registers). LK leaves the PHY in UART mode for its console; if our
tphy init does not (always) clear it, D+/D- stay routed to UART while CC
attach looks perfect — exactly the observed signature.

**Next actions:** (1) genuine #177 FTDI boot capture — does serial die
at ~0.45s? (the earlier "#177 serial to prompt" report was a misread of
the #176 log); (2) devmem the U2 PHY DTM registers on a broken boot;
(3) if UART bits stuck, clear live to prove, then fix tphy init/probe
ordering permanently.

**Blocks:** SSH-over-USB reliability (Phase 8), and any workflow that
flashes then expects gadget access without a magic boot sequence.

**Update 2026-07-14 (early): WORKING AGAIN + pattern identified as the
documented cable protocol.** SSH restored end-to-end (#177, UDC
`configured`, keymap active, rootfs resized to 26G) after: boot with FTDI
attached (serial dies at 0.454s = PHY switch), then hot-swap the cable to
the Mac → RNDIS enumerates. This matches the 2026-07-10 session note
verbatim: "Cable protocol: FTDI in at boot, swap to USB after — booting
with USB in breaks gadget." So B-20's *operational* face is known,
reliable behavior, reproduced twice tonight; what remains open is the
root cause (what LK/U2-PHY/mtu3 do differently when VBUS+host are present
at power-on) and a software fix so boot-with-host-attached works.
Downgrading from 🔴 to 🟡. Workaround: never boot with the Mac cable in;
boot with FTDI (or nothing) in the left port, plug the Mac in after boot.

**Update 2026-07-14 (Stage A/B of USB plan): registers now source-backed,
good-boot baseline captured, broken-boot diagnostic staged.** Vendor
harvest (research.md "USB Left-Port PHY & Type-C Harvest") pinned the
usb2uart registers: U2PHYDTM0=0x11290868, U2PHYDTM1=0x1129086C, plus an
AP-side mux at 0x10005600 (0x80=UART, 0x00=USB) that mainline never
touches. Live dump on a WORKING #177 gadget boot: DTM0=0x52000008
(RG_UART_MODE[31:30]=01 — nominally "uart mode"!), DTM1=0x00043E2E,
MISC=0x80, UDC=configured — so RG_UART_MODE/MISC alone do NOT block
gadget; the good-vs-broken differential will isolate the bits that do.
Vendor gadget attach is PMIC BC1.2 + CHRDET (`mu3d/drv/mt_usb.c`), not
controller VBUS sensing — supports the mtu3 role/VBUS-sensing suspect.
A run-once diagnostic harness is now installed in the rootfs
(`run-once.service`, see boot.md 2026-07-14) and `/root/run-once.sh` is
staged with the full register/FUSB/dmesg dump.

**ROOT-CAUSED AND PROVEN LIVE 2026-07-14 (same session, boot.md "B-20
ROOT-CAUSED"):** the broken/good differential is NOT uart mode (broken
boots have the *cleaner* PHY state) — it is U2PHYDTM1's session-signal
FORCE bits [13:9]. Good boots inherit LK's software-forced
"device-role/session-valid" state (DTM1=0x43E2E); broken boots
(host attached at power-on → different LK path) get DTM1=0x26 with no
FORCE bits, and mtu3 waits forever on hardware VBUS sensing this
platform doesn't have (vendor mu3d forces these bits from PMIC BC1.2
detection — mt_usb.c). Causal proof on a broken boot via the run-once
harness: `devmem 0x1129086C 32 0x3E2E` flipped UDC `not attached` →
`configured` in <5s, RNDIS enumerated on the Mac, SSH worked — first
ever enumeration on the boot-with-host-attached protocol.

**Fix built, awaiting hardware verification:** build #225
(`logs/2026-07-14-225-b20-force-session-valid/`, sha256 `78b71ad3...`,
banner #225) = `patches/v6.6/phy/0001-phy-mtk-tphy-force-b-session-valid
-for-mt6797.patch` (DTS-gated: new `mediatek,force-b-session-valid`
property on u2port0 in dts/0009; forces the proven 0x3E2E state in
`u2_phy_instance_power_on`, undone in power_off, dev_info logged).
Success criterion: boot with Mac cable attached from power-on → gadget
enumerates unaided, ≥3 consecutive boots; FTDI-protocol boots
unregressed (keyboard+display+gadget). Then B-20 closes 🟢 and the
cable protocol is retired.

**VERIFIED ON HARDWARE AND CLOSED 2026-07-14.** Build #225 flashed to
boot2. Three consecutive cold boots with the Mac cable attached from
power-on — the protocol that previously failed 100% of the time — all
enumerated unaided: banner #225, `u2 phy0: forcing session-valid/device
mode` at 0.449s, DTM1 reads 0x3E2E, UDC `configured`, SSH working.
FTDI-protocol regression boot also clean
(`logs/2026-07-14-226-b20-ftdi-regression-boot.log`: banner #225 on
serial, gadget `configured` + SSH after cable swap; serial still dies at
the ~0.45s PHY switch — that is B-15, unchanged and expected). The
cable protocol ("never boot with the Mac cable in") is retired: boot
with the host attached now just works.


## 🛑 B-21 — Internal WiFi via MT6797 CONSYS (Phase 8 Stage 2, activated 2026-07-14, **NO-GO 2026-07-21**)

**NO-GO decision (user, 2026-07-21):** internal CONSYS WiFi/Bluetooth is
parked permanently, not just paused. Rationale: three independent AP-side
transport attempts (PIO firmware-push #262, DMA transport #274, DMA with
the apdma clock made optional #275) all failed to get the CONSYS MCU past
the same `-110` BTIF timeout, and the DMA attempts additionally introduced
an unexplained, unresolved eMMC-mount regression (boot.md 2026-07-21) —
diminishing returns with no new register-level hypothesis left to try.
Even a fixed handshake would still leave the ~75-103 KLOC vendor
`wlan_drv_gen2` port ahead of it (driver_ports.md), which already broke
upstream at frank-w's kernel 6.0 attempt and was never fixed. Per this
section's own risk note below, the gate exists precisely because the
gen2 port might prove uneconomical — it has.

**Path forward for wireless connectivity:** a USB WiFi dongle over the
right port's already-working host mode (same MUSB host path proven with
USB Ethernet, B-22) — zero CONSYS risk, mainline driver support
(`mt7601u`/`ath9k_htc`/`rtl8xxxu` etc. depending on the dongle used).
Internet connectivity is otherwise already solved via right-port USB
Ethernet (B-22, complete). Bluetooth remains blocked on the same CONSYS
gate and is NO-GO alongside WiFi for the same reason.

**If revisiting this decision in the future:** the codebase's
`patches/v6.6/soc/0003`/`patches/v6.6/dts/0013` are reverted to the
pre-Stage-W4 (#269-matching, PIO) state as of 2026-07-21 — the DMA/apdma
work is not preserved on disk, only in this file's history and boot.md.
The two open register-level leads noted in the Stage W4 entry below
(`0x10001f00` bit 11, an SPM/EMI precondition vendor LK sets) were never
conclusively ruled in or out and would be the starting point.

**RE-PARKED 2026-07-20 after #262 tested — G2b FAIL, firmware-push
theory falsified.** #262 was flashed and run (unparked to attempt
Bluetooth, which sits behind the same gate): G2a still passes, but the
BTIF transport jams after exactly one frame ("BTIF TX stuck, LSR=0x20"
×13 — TEMT never sets), so the firmware push never reached the ROM;
the ROM is deaf on BTIF entirely, not opcode-selective. CPUPCR decays
to the known-bad 0x55AA55xx spin; 0x10001f00 still 0x11403200 vs
golden 0x6D403A00 — this delta was live-poked to the full golden value
on real hardware in #247 and eliminated as a cause (see #247 postmortem
below, boot.md #247 entry); it was never committed to soc/0003, which
is why it kept reappearing as "unexplained" in later write-ups. Full entry:
boot.md 2026-07-20 "#262 FLASHED AND TESTED". Resuming now means
root-causing the abnormal ROM execution state (what precondition does
vendor LK/boot set that we don't?), not pushing more protocol. Interim
networking: right-port USB ethernet (internet-enabled 2026-07-16)
covers connectivity; **Bluetooth is explicitly blocked on this same
gate.** **Resume path scoped 2026-07-20:** instrumented-vendor-kernel
harvest — printk-instrument the vendor 3.18 BTIF/WMT/CONSYS drivers and
capture a working firmware push + QUERY_STP wire trace (no userspace
race, unlike Hypothesis 1). Full plan, instrumentation points and flash
strategy: [docs/kali-harvest-plan.md](docs/kali-harvest-plan.md)
(batched with the B-23 speaker checklist and camera/LTE/mic/WiFi
captures in one Kali flash session).

Original parking note (2026-07-16), kept for history: build #262
implements the re-scoped G2b firmware push (protocol in research.md
"WMT Firmware-Push Protocol").

**Goal:** working internal WiFi (scan/associate/DHCP/SSH-over-WiFi) via
the on-die CONSYS block — the vendor gen2 stack is the only
implementation that has ever existed (~150 KLOC WiFi; no mainline
support; frank-w's same-core port broke at kernel 6.0, unfixed). Staged
with hard gates so we can stop cheaply (plan.md Phase 8 "WiFi plan"
Stage 2; approved plan in research.md "CONSYS Stage W0 harvest").

- **Stage W0 (DONE 2026-07-14, no flash):** vendor power-on sequence
  fully source-harvested (corrections: CONN_PWR_CON=0x280 not 0x32C; no
  clk-mt6797 change needed); LK proven to leave CONSYS cold (devmem on
  #225); firmware blobs + wmt binaries extracted from Android p27 to
  `docs/firmware-consys/`; golden-reference harvest script
  `scripts/consys-golden-harvest.sh` ready for the optional vendor-Kali
  boot (W0b — needs `boot2 planet/kali_boot.img` + slow 5.5GB
  `linux planet/linux.img` flash, then Debian restore).
- **Stage W1: Gate G2a PASSED (proven live by hand 2026-07-14).** Two
  fixes en route: build #234 = pwrap reset made optional (soc/0004 —
  mt6797 has no mainline reset provider, mt6797 pwrap caps demand one);
  then scpsys "Failed to power on domain conn" root-caused live to a
  STALE vendor CONN_PWR_CON define — real offset is SPM+0x32C, not
  0x280 (0x280 rejects writes; 0x32C idles at the 0x112 off-pattern and
  acks in PWR_STATUS bit1). Manual devmem sequence at 0x32C returned
  chip-ID **0x0279** with VCN rails off and no aux pokes. Build #236
  flashed and VERIFIED: full driver-level pass at 0.58s boot, chip-ID
  0x279 first read, CONN genpd on, zero regressions (display/keyboard/
  gadget SSH). **Stage W1 COMPLETE 2026-07-14.** New:
  scpsys CONN domain (pmdomain/0002), minimal MT6351 VCN regulator
  driver over pwrap (regulator/0002 — first Linux PMIC access in the
  project), consys spike driver (soc/0003), pwrap+mt6351+consys DTS
  (dts/0013), `configs/gemini-consys.config`. Pass/fail is one dmesg
  line, checkable over gadget SSH.
- **Stage W2 (in progress 2026-07-14): Gate G2b.** Desk harvest complete:
  the WMT handshake runs over **BTIF** (AP↔CONSYS FIFO @0x1100c000,
  mainline `CLK_INFRA_BTIF` gate exists) in STP *mand mode* (4-byte hdr
  `0x80|seq<<3, type<<4|len_hi, len_lo, 0x00` + payload + 2 zero CRC,
  WMT task=4) — the full 3.5-KLOC STP core is NOT needed for the gate.
  Builds ≤#236 left the MCU held in the WDT swsysrst (bit12, key
  0x88<<24); releasing it boots the ROM. **Build #237** (sha256
  `c646178a…`, `logs/2026-07-14-237-consys-w2-g2b-mcu-handshake/`)
  extends soc/0003: EMI remap (TOPCKGEN+0x1340 = base>>20 | BIT(12)) +
  zero the 343K ctrl window, BTIF PIO init, MCU release, then
  WMT_QUERY_STP (`01 04 01 00 04`) expecting ROM event `02 04 06 00 00
  04` = **Gate G2b PASS** dmesg line. On FAIL the RX hex dump is logged
  and state left up for devmem. dts/0013 gains the btif clock.
  **#237 booted (banner verified): G2a still passes; G2b failed at step
  one with `memory-region unresolved (-22)`** — code bug: `consys_mem`
  is a dynamic reserved-memory node (no `reg`), so
  `of_address_to_resource()` can't resolve it; must use
  `of_reserved_mem_lookup()` (kernel allocated it at 0x42600000 this
  boot). **Build #238** (`consys-g2b-emi-lookup-fix`, sha256
  `1766aeb3…`, `logs/2026-07-14-238-consys-g2b-emi-lookup-fix/`) fixes
  exactly that in soc/0003. **#238 booted: EMI/remap OK, but G2b still
  -110 (TX out, 0 RX). Live SSH session then proved the MCU ROM IS
  RUNNING** (CONSYS_CPUPCR 0x18070160 changes every read) — so power/
  clock/EMI/reset are all correct and only the BTIF channel is at
  fault. Root cause: BTIF FIFOCTRL clear bits are level-held (vendor
  pulses them; we left both FIFOs in reset, discarding the ROM's
  reply), plus the never-done BTIF_WAK (+0x64) ap_wakeup_consys pulse
  before TX. **Build #239** (`consys-g2b-btif-fifo-wakeup`, sha256
  `d18973f8…`) fixes both, adds a 500ms retry and CPUPCR/LSR/IIR
  logging on FAIL. **#239 booted: still -110, but the live session
  found the real root cause — the CONN domain's TOPAXI bus-protect
  mask is bits 17|18 (vendor clk-mt6797-pg.c, the actual runtime
  path), not the MT2701 2|8 our scpsys entry used; PROTECTSTA1 bit 18
  was never released, blocking all BTIF traffic into CONSYS while
  chip-ID/CPUPCR reads (different path) worked. Build #240**
  (`consys-conn-busprot-17-18`, sha256 `2ba9b70960b8a880…`) fixes
  pmdomain/0002. **#240 booted: protect fix VERIFIED (STA1 bit 18
  clear, first query frame drained fully — path open), but still
  G2B FAIL (-110): the ROM runs (CPUPCR alternates 0x55AA55xx
  idle-pattern with real addresses 0x428/0x3538) yet never services
  BTIF — its link FIFO swallowed the first frame and stayed full;
  live MCU re-reset + immediate re-query, OSC_EN(0x10001f00 bit9) and
  BTIF_WAK pulses all no-effect; EMI ctrl window untouched by ROM;
  HW_VER 0x8A00 healthy; ACR MBIST already set; PMIC DCXO CW00 bit5
  (XO_WCN) already 1. Every register vendor's power-on touches now
  matches. See boot.md 2026-07-14 #240 entry. Next: Stage W0b
  golden-reference harvest from the working vendor Kali 3.18 stack
  (user decision — needs 5.5GB linux.img reflash + restore), to
  capture healthy-idle CPUPCR, DCXO/pwrap DCXO_CONN bridge state, and
  ROM-boot EMI signature.** **W0b golden harvest COMPLETE 2026-07-15**
  (boot.md entry; logs 2026-07-15-242/243): healthy CPUPCR = 0x0009997A
  steady → our 0x55AA55xx is ABNORMAL; working system runs with
  CONN_PWR_CON=0x10D (bit 8 SET — vendor never touches SRAM_PDN, our
  scpsys entry was clearing it: top suspect); BTIF golden HANDSHAKE=0x3
  TRI_LVL=0x18; DCXO_CONN bridge all-zero (clock-buffer theory dead);
  ROM answers the query ~50ms after reset release on golden hardware;
  0x10001f00 golden 0x6D403A00 vs our 0x11403200 (bit 11 — reserve
  suspect). **Build #244** (`consys-g2b-golden-fixes`, sha256
  `7d681d06…`) = sram_pdn_bits→0 (pmdomain/0002) + BTIF golden config +
  mand-frame byte0 fix (soc/0003). Awaiting flash (+ Debian rootfs
  restore after the Kali detour). ROM-patch download (opcodes 0x08/0x01,
  ≤1000-byte frags) is the step after the query handshake proves the
  channel. **#244 was a silent no-op** (2026-07-15): the regenerated
  soc/0003 lost its Kconfig hunk (uncommitted Mac-tree edit) and the
  untracked spike.c, so `CONFIG_MTK_CONSYS_SPIKE` was dropped by
  olddefconfig and the driver never built — #244's boot tested nothing.
  Patch fixed (Kconfig + Makefile + full spike.c). **Build #247**
  (`consys-g2b-spike-kconfig-fix`, sha256 `3810bd14…`) = the real
  golden-fixes run. **#247 booted: G2a PASS, G2b still FAIL (-110).**
  Long live-debug session (boot.md #247 entry) eliminated, one variable
  at a time on the running system: SWSYSRST bit16, MCU_CFG_ACR bits
  24/25, full golden AP2CONN_OSC_EN 0x6D403A00, the vendor AFE/WBG
  analog table (0x180B6000 — step the spike never did), BTIF
  HANDSHAKE/WAK/FIFO-clear combinations, and the EMI-MPU theory
  (remapping to golden 0xBFA00000 window + MCU reset-cycle — no
  change). Signature refined: BTIF TX shifter hard-blocks (LSR 0x60→
  0x20 on first byte, FIFO-clear resets it, next byte re-sticks), i.e.
  the CONN-side BTIF peer never initializes; MCU ROM executes (real
  PCs interleaved with 0x55AA55xx sleep samples) but parks. Remaining
  hypotheses: golden *pre-patch* ROM-idle state was never captured
  (all golden numbers are post-firmware); vendor 3.18 CCF
  `clk_scp_conn_main` (scpsys) side effects beyond our sequence;
  co-clock/XO detail (`RG_VCN28_ON_CTRL=1` HW-mode before VCN28
  enable). Device left safe: MCU parked in reset, EMI mapping restored.
  **Postmortem conclusion (retro-flagged 2026-07-20):** this session live-poked
  and individually eliminated every register/sequencing hypothesis derivable
  from the golden Kali harvest — 0x10001f00 to the full golden 0x6D403A00,
  SWSYSRST bit16, MCU_CFG_ACR bits 24/25, the AFE/WBG analog table, every
  BTIF HANDSHAKE/WAK/FIFO-clear combination — none moved G2b off -110. None of
  these pokes were committed to soc/0003 (they were live devmem only), which
  is why later entries kept re-describing 0x10001f00 as "unexplained" — it
  was already tried and shown to do nothing in isolation. The one gap nothing
  has touched: the golden reference was captured *after* firmware was already
  resident, so there is no reference for ROM register state in the window
  between MCU-reset-release and firmware-push — exactly the window where our
  ROM goes abnormal (0x55AA55xx spin). That makes the kali-harvest-plan.md
  instrumented-vendor-kernel capture (docs/kali-harvest-plan.md) the only
  remaining action that targets an actually-uncaptured gap, not one guess
  among competing untested code changes (firmware-push retry, fuller STP
  init sequence, DMA) — those are all guesses against an already-exhausted
  register space and should not be attempted before the pre-firmware harvest.
- **Update 2026-07-16 — source audit of the real vendor CONSYS driver**
  (`mtk_wcn_consys_hw.c`, `wmt_core.c`, `wmt_ic_soc.c` in the 3.18 reference
  tree, not just the generic scpsys `clk-mt6797-pg.c` checked in earlier
  builds). Findings:
  - **Hypotheses 2 and 3 (from the #247 session) are now conclusively
    ruled out with source citations, not just build-log elimination.**
    `mtk_wcn_consys_hw_reg_ctrl()` is the real platform power-on function;
    its VCN18→VCN28(+`RG_VCN28_ON_CTRL=1` HW-mode switch, written *before*
    LDO enable)→`CONN2AP_SLEEP_MASK`→WDT-hold→`SPM_PWRON_CONFG_EN=0x0b160001`
    →CONN MTCMOS→chip-ID-poll→MBIST-bit sequence matches our
    regulator/0002 + soc/0003 patches step-for-step, same order, same
    values. The `mtk_wcn_consys_hw_gpio_ctrl()` companion (PIN_BGF_EINT/
    GPS_SYNC/GPS_LNA/I2S_GRP) is a dead end for us — confirmed via the
    device's own extracted DTB (`docs/vendor-dtb/gemini_kali_boot.dts`)
    that `btif@1100c000` has no pinctrl properties; BTIF is a pure
    internal AP↔CONSYS bus block on this SoC-integrated CONSYS design,
    not routed through external GPIO pins.
  - **New candidate found in `wmt_core_stp_init()`/`mtk_wcn_soc_sw_init()`
    (`wmt_core.c`, `wmt_ic_soc.c`):** the vendor's BTIF bring-up is not a
    single query/response — it's `init_table_1_2` (query, mand mode) →
    `init_table_4` (set STP options) → switch STP mode to
    `MTKSTP_BTIF_FULL_MODE` → sleep 10ms → `init_table_5` (query again, now
    in full mode). Our G2b spike sends only the equivalent of
    `init_table_1_2`'s single query and never performs the FULL_MODE
    switch — this on its own doesn't explain a zero-byte RX on the very
    first attempt (the first query should still be answerable in mand
    mode per the vendor's own `init_table_1_2` step), but it means our
    "single query is sufficient for the gate" assumption is not what the
    vendor actually does, and the fuller sequence is untested.
  - **Concrete bug found: our G2b PASS/FAIL check
    (`soc/0003`'s `wmt_query_stp_evt[]`) only matches the first 6 bytes of
    the expected reply.** The real vendor constant
    (`wmt_ic_soc.c:123`) is 10 bytes:
    `WMT_QUERY_STP_EVT_DEFAULT[] = {0x02,0x04,0x06,0x00,0x00,0x04,0x11,0x00,0x00,0x00}`
    — our `wmt_query_stp_evt[]` is missing the trailing `11 00 00 00`.
    Our TX command itself is byte-for-byte correct
    (`WMT_QUERY_STP_CMD = {0x01,0x04,0x01,0x00,0x04}`, confirmed against
    `wmt_ic_soc.c:122`). A prefix-only match wouldn't itself cause a
    zero-byte RX failure, but it means our RX path may not be reading/
    draining the full expected frame length, which is a plausible
    contributor to the observed "TX shifter blocks on retry" symptom
    (an under-drained previous reply jamming the link for the next
    attempt). Confirmed our own STP mand-mode frame-wrapping assumption
    (4-byte hdr + WMT payload + 2-byte zero CRC) is architecturally
    correct — the 5-byte `WMT_QUERY_STP_CMD` is the inner WMT payload,
    wrapped by the (skipped, ~3.5-KLOC) vendor STP core before it reaches
    BTIF; our hand-rolled equivalent framing was already sourced
    correctly in the W2 harvest.
  - **Fix implemented 2026-07-16 (not yet built/flashed):**
    `patches/v6.6/soc/0003-soc-mediatek-add-mt6797-consys-spike.patch`'s
    `wmt_query_stp_evt[]` widened to the full 10-byte
    `WMT_QUERY_STP_EVT_DEFAULT` (`02 04 06 00 00 04 11 00 00 00`). No other
    change needed: `wmt_cmd_evt()` already passes `sizeof(wmt_query_stp_evt)`
    through to the memcmp scan, and `btif_rx_drain()`'s 64-byte buffer /
    1000ms timeout already exceed 10 bytes, so the existing RX drain covers
    the wider match with no code changes beyond the constant. Verified
    `git apply --check` clean against a fresh v6.6 tree. Next: rebuild
    (build-pack), flash `boot2`, re-run Gate G2b, capture serial.
  - **Not yet attempted:** replicating the vendor's fuller BTIF bring-up
    sequence (`init_table_1_2` query → `init_table_4` set-STP-options →
    switch to `MTKSTP_BTIF_FULL_MODE` → 10ms settle → `init_table_5` second
    query) — our spike only ever sends the equivalent of `init_table_1_2`.
    Worth trying if the widened-constant fix alone doesn't flip G2b to PASS.
  - **Build #257 tested 2026-07-16 — widened constant confirmed NOT the
    root cause.** G2a still passes (chip ID 0x279). G2b still FAILs at -110
    (`ETIMEDOUT`) — both attempts report **RX 0 bytes**, so the fix (matching
    the full 10-byte event) never had a byte stream to compare against; this
    rules out "truncated match constant" as *the* cause, though the fix is
    still correct/retained (dead code otherwise). CPUPCR samples are
    changing between reads (`0x55aa55d2` → `0x55aa55d6` → `0x55aa55da`),
    confirming the MCU ROM is genuinely executing after reset release — it
    simply never answers the WMT_QUERY_STP command over BTIF. On retry, the
    *second* TX itself stalls (`BTIF TX stuck, LSR=0x20` — TEMT never sets),
    suggesting either the first frame is still sitting un-drained in the ROM
    side (no far-end consumer clearing the shifter) or our retry re-sends
    into a BTIF state the first failed exchange left wedged. This points
    back toward hypothesis territory: either (a) the ROM needs something
    from the not-yet-replicated `init_table_4`/`FULL_MODE` sequence before
    it'll answer even the first query — contradicted by vendor source
    showing `init_table_1_2` is sent standalone in mand mode first — or
    (b) our STP mand-mode frame is malformed/misaddressed in a way the ROM
    silently drops (wrong WMT task index, wrong CRC handling, wrong wakeup
    timing) even though byte-level construction matches `stp_core.c` on
    paper. **Next recommended step:** capture the TX frame bytes against a
    real vendor STP core trace/logic-analyzer if possible, or re-examine
    `stp_core.c`'s `stp_send_data_no_ps` mand-mode path byte-by-byte (only
    partially audited so far) for a subtler framing mismatch (e.g. CRC not
    actually zero, task-index bit position, or a required wakeup/ready
    handshake before the ROM's UART-equivalent ISR is listening).
  - **Update 2026-07-16 (cont.) — full BTIF hardware-init audit
    (`btif_plat.c` `hal_btif_hw_init()`, `mtk_btif.c` `_btif_send_data()`/
    `hal_btif_is_tx_allow()`, `mtk_btif_exp.c` `mtk_wcn_btif_wakeup_consys()`).**
    Traced (b) exhaustively — every BTIF register poke our spike's
    `btif_hw_init()` does was checked bit-for-bit against the real
    `hal_btif_hw_init()`: FAKELCR (both write `0x0`/normal mode), new-
    handshake-mode enable (`BTIF_HANDSHAKE_EN_HANDSHAKE`, matches), Rx/Tx
    FIFO clear-then-release sequence (matches, and is the fix from build
    #237/#238 - still correct), TRI_LVL trigger levels (matches, computed
    from the same `BTIF_TX_FIFO_THRE`/`BTIF_RX_FIFO_THRE` constants),
    loopback disabled, DMA disabled + auto-reset enabled (matches). One
    real difference found: the vendor **leaves Rx IER enabled**
    (`hal_btif_rx_ier_ctrl(p_btif, true)` at the end of hw_init) since it's
    interrupt-driven; our spike masks all IERs including Rx (`writel(0,
    BTIF_IER)`) since it polls LSR.DR instead - this is host-side-only
    register state (doesn't reach the MCU/bus), so it cannot explain the
    MCU never answering, but is worth ruling in/out empirically since it's
    now the only unexplained divergence at the BTIF hardware layer. The
    wakeup-pulse mechanism (`hal_btif_raise_wak_sig`: clear WAK, sleep
    64-96us, set WAK) is confirmed to exactly match our `btif_wakeup_consys()`,
    and per its own doc comment is only meaningful "once sleep command is
    sent to consys" (i.e. is a no-op after a cold MCU-ROM release, so its
    absence/presence shouldn't matter here). The CPU-reset-release register
    path (`mtk_wdt_swsysret_config((1<<12), ...)` = `AP_RGU_SWSYSRST` bit
    12, `0x10007018`) is confirmed identical between `mtk_wcn_consys_hw.c`'s
    two chip variants and our spike. **Conclusion: the entire AP-side
    hardware path (power-on, BTIF init, MCU release, STP mand-mode framing)
    now matches the vendor driver as closely as static source review can
    verify** - CPUPCR advancing proves the ROM is alive, yet it never
    answers on BTIF. This makes hypothesis 1 (parked pre-patch golden
    capture, per 2026-07-16 checklist) the most likely remaining
    explanation: the ROM may genuinely require something delivered by the
    proprietary WMT firmware/patch download path (not visible in the GPL
    driver source, since patch blobs and their loader protocol are closed)
    before it will respond to STP commands at all - i.e. Gate G2b as
    currently defined may be unreachable pre-firmware regardless of how
    correct our register sequencing is. Try widening the Rx IER divergence
    fix first (cheap, rules out a real if unlikely difference); if that
    doesn't change the outcome, the pre-patch-capture experiment (parked)
    becomes the most information-dense next step, since it would show
    whether the *vendor's own* pre-firmware ROM answers this same query at
    all, or whether even the real driver only gets a BTIF reply after WMT
    firmware is pushed.
  - **Build #259 tested 2026-07-16 — Rx IER fix confirmed NOT causal, as
    expected.** Matched vendor's `hal_btif_hw_init()` exactly (leaves
    `BTIF_IER_RXFEN` set instead of masking all IERs). Result: bit-identical
    failure signature to build #257 — G2a PASS (chip ID 0x279), G2b FAIL
    (-110), RX 0 bytes on both attempts, CPUPCR still advancing
    (`0x55aa55de → 0x55aa55e2 → 0x55aa55e6`), retry TX still stalls
    (`LSR=0x20`). Confirms the divergence was host-side-only as predicted
    and rules it out. **The entire AP-side hardware/software path is now
    verified correct against vendor source to the limit of static review.**
    Proceeding to hypothesis 1: the parked pre-patch golden capture
    (`logs/2026-07-16-b21-golden-prepatch-checklist.md`), simplified this
    time to flash `boot2`+`linux` with the vendor Kali stack directly
    (skip the risky `boot`-partition/Debian-rootfs combination that
    reboot-looped in the 2026-07-16 attempt) - this is the only remaining
    way to determine whether the real vendor driver's ROM answers
    WMT_QUERY_STP pre-firmware at all, or whether Gate G2b is structurally
    unreachable without the proprietary WMT patch blob.
- **Hypothesis 1 test (2026-07-16, vendor Kali stack, `boot2`+`linux`
  flashed with `planet/kali_boot.img`/`planet/linux.img`):** confirmed
  root cause for why every prior "extensive" W0b golden harvest
  (`scripts/consys-golden-harvest.sh`, builds #240/#247) only ever
  captured **post-firmware** CONSYS state, and why a true pre-firmware
  capture is not achievable from a live shell. dmesg shows
  `wmt_launcher` fires and completes both WMT firmware fragment
  downloads (`ROMv3_patch_1_1_hdr.bin`, `ROMv3_patch_1_0_hdr.bin`) by
  **11.7s uptime** — `wmt_launcher` is registered `class core` in
  `on init` inside the Android LXC's `init.connectivity.rc` (found at
  `/var/lib/lxc/android/rootfs/init.connectivity.rc` on the running
  device), Android init's earliest service class. No shell (serial,
  SSH, ADB) is reachable that early, so the firmware push always wins
  the race before any harvest script can run.
  Attempted fix: added `disabled` to the `service wmt_launcher` stanza
  and rebooted — **did not work**, because `/var/lib/lxc/android/rootfs`
  is a `tmpfs` re-populated from `/system/boot/android-ramdisk.img` by
  `pre-start.sh` on every container (re)start, so the live-tmpfs edit
  was discarded before the next boot. Attempted to patch the ramdisk
  image directly: blocked because `/system` is a loop-mounted image
  (`/data/system.img` on `/dev/loop0`) that refused `mount -o
  remount,rw` ("write-protected" at the loop-device level, not just the
  mount) — no changes were made, remount failed cleanly.
  **Conclusion (user decision 2026-07-16): stop here.** A genuine
  pre-firmware capture would require either fixing the loop-device
  write protection to patch the ramdisk properly, or a kill-loop racing
  `wmt_launcher`'s ~0.5s firmware-push window — both treated as
  disproportionate effort for this gate. **Hypothesis 1 is treated as
  confirmed by the timing evidence above without a direct pre/post
  diff:** firmware is resident on the MCU before any userspace
  observation point exists, so the AP-side path (verified correct
  against vendor source in the build #259 audit) cannot be the
  remaining blocker — G2b's WMT_QUERY_STP handshake is answered by
  ROM+firmware together, never ROM alone, and our spike (ROM-only,
  no firmware push) failing to get a reply is expected vendor-matching
  behaviour, not evidence of a driver bug.
- **Update 2026-07-20 — Kali harvest session closed; hypothesis 1
  strengthened by direct trace evidence, but no new pathway to WiFi.**
  Full session: [docs/kali-harvest-plan.md](docs/kali-harvest-plan.md).
  The instrumented vendor 3.18 kernel's boot capture
  (`logs/2026-07-20-H18-kali-harvest-boot.log`) recorded genuine
  two-way BTIF/WMT traffic on real hardware — 1198 `HARVEST-BTIF-RX`,
  1479 `HARVEST-BTIF-TX`, 584 `HARVEST-WMT-RX`, 1378 `HARVEST-WMT-TX`
  events — i.e. direct proof the vendor stack completes the firmware
  push and the ROM does answer over BTIF once firmware is resident.
  This upgrades hypothesis 1 from "confirmed by timing evidence
  without a direct pre/post diff" (2026-07-16 wording above) to
  "confirmed with a real working-trace reference" — but it is still
  not a pre-firmware capture (H18, like every prior harvest, starts
  after `wmt_launcher` has already fired), so the original gap stands:
  we still have no evidence of what the ROM does or needs *before*
  firmware is pushed.
  **Cross-checked live 2026-07-20 against the current build (#269,
  running on hardware today): the `mtk-consys-spike` diagnostic still
  reproduces the exact pre-harvest failure signature** — G2a passes
  (chip ID 0x279), WMT query gets 0 RX bytes, retry TX stalls with
  `BTIF TX stuck, LSR=0x20` — identical to the build #257/#259
  signature this section already documents. Nothing about the harvest
  changed the AP-side driver; it only added stronger evidence for a
  hypothesis that was already the leading explanation.
  **Net effect: there is still no pathway to WiFi/Bluetooth.** The
  harvest closed without attempting the one thing that would actually
  move this gate — pushing the real WMT firmware patch
  (`docs/firmware-consys/ROMv3_patch_1_0_hdr.bin` /
  `_1_1_hdr.bin`) through our own spike driver before judging G2b, per
  the Stage W3 re-scope already called for below.
  **Correction (2026-07-21): the paragraph above is stale.** The
  Stage W3 firmware-push spike *was* built and flashed, as build
  **#262** (`logs/2026-07-16-262-consys-g2b-fw-push/`, tested
  2026-07-20 — see boot.md "BUILD #262 FLASHED AND TESTED"), not
  "never built or flashed" as this entry originally claimed. Result:
  **G2b still FAILed.** Every TX after the first ROM-only query went
  `BTIF TX stuck, LSR=0x20` (THRE sets, TEMT never does — the PIO
  shift register cannot drain), so firmware push aborted before a
  single fragment was sent; this reproduced #240's finding that the
  CONSYS link FIFO swallows one frame and jams because the ROM never
  drains its RX FIFO. See the Stage W4 entry below for the next
  action this pointed to.
- **Update 2026-07-21 — H35: golden trace obtained (full handshake +
  holding shell, single boot, zero crashes), but same net conclusion —
  still no pre-firmware capture, no new pathway.** Root cause of the
  H18 gap (trace present but crashed before a shell) was rebuilt
  correctly this session: a fresh `git clone` of
  `gemini-android-kernel-3.18` directly on the VM (the Mac-side
  checkout had the same case-folding corruption bug already documented
  for linux-6.6, silently dropping `net/netfilter/xt_DSCP.c`) with all
  9 vendor patches applied, including 0009 (`ccci_ringbuf` alignment
  fix) alongside 0002 (harvest instrumentation) — the combination no
  prior H1–H32 attempt had actually built together. Flashed as `boot`
  (image `logs/2026-07-21-H33-kali-harvest-resync-full-fix/harvest_kali_boot.img`,
  sha256 `5ce203a95858d145b959c630ca8c0b40a2c7175a0da1c158e68ec19a630c8f93`)
  with vendor Kali rootfs (`planet/linux.img`) on `linux`; `boot2`/#269
  and the Debian rootfs untouched.
  `logs/2026-07-21-H35-kali-harvest-boot.log`
  (sha256 `39ecd122ba6898176fc4db38982f6186c711932979b7583c8edf8e3b6c247655`)
  is the resulting capture: single boot cycle, zero panic/BUG, zero
  DEVAPC violations, harvest trace running continuously 105.2s–138.3s
  (1214 BTIF-RX, 1491 BTIF-TX, 592 WMT-RX, 1382 WMT-TX, 3 SNAP events —
  roughly 3–4x H18's volume) spanning across the `kali login:` prompt at
  126.3s, with the capture continuing stable to 141s+ afterward (normal
  battery/thermal telemetry, no crash). This is the first capture with
  both the full CONSYS/BTIF/WMT firmware-push trace and a shell that
  actually holds, in one continuous boot — confirming the H18 crash was
  purely the `ccci_ringbuf` bug (now fixed) and not evidence of anything
  wrong in the CONSYS/BTIF handshake itself.
  **But this still does not open a new pathway to WiFi.** Like H18, the
  H35 trace starts with `wmt_launcher` already having fired (first
  HARVEST-SNAP line is `reg_ctrl-on-entry`, i.e. already inside the
  vendor power-on sequence) — it is a confirmation/volume upgrade of the
  same post-firmware reference evidence, not the pre-firmware capture
  the original gap was about. An interim finding: this vendor image's
  right USB port runs in gadget mode (`rndis0`, "USB Tethering" service,
  `link is not ready`) rather than host mode, so a USB ethernet dongle
  plugged into it is not recognized — the host-mode right-port DTS is a
  mainline-6.6-only addition (B-22), not present in this stock vendor
  kernel, so SSH/network access to this image requires serial only.
  **Conclusion unchanged from the 2026-07-20 update above:** resuming
  WiFi work means implementing a real firmware-push in our own AP-side
  driver and testing it against the now well-evidenced vendor reference
  (Stage W3 re-scope), not capturing more harvest traces.
- **Stage W4 (2026-07-21, not yet built/flashed): BTIF DMA transport.**
  #262's failure signature (`BTIF TX stuck, LSR=0x20`, THRE set but
  TEMT never sets on every TX after the first) means the PIO shift
  register cannot drain even once — a transport-level stall, not a
  missing-firmware problem. The one concretely-flagged, never-tested
  lead: the vendor BTIF driver hard-enables DMA for both directions
  (`ENABLE_BTIF_TX_DMA`/`ENABLE_BTIF_RX_DMA` in `mtk_btif.h`,
  `BTIF_DMA_EN_TX`/`BTIF_DMA_EN_RX` set in `hal_btif_hw_init()`), while
  every AP-side build through #262 ran BTIF in PIO only
  (`btif_hw_init()` explicitly forced `BTIF_DMA_EN` off) — flagged as
  a "first-class behavioral delta" in the 2026-07-20 harvest plan but
  never tried at the AP-driver level. The BTIF DMA channels
  (`btif_tx@11000a00`/`btif_rx@11000a80` in the vendor DTB) turned out
  to be the same "VFF ring" APDMA IP mainline
  `drivers/dma/mediatek/mtk-uart-apdma.c` already drives on newer MTK
  SoCs (register offsets match exactly), but this SoC's vendor DTB
  models each channel as a bare MMIO window with no OF-DMA-controller
  binding, so `patches/v6.6/soc/0003` pokes the VFF registers directly
  (sourced from vendor `btif_dma_plat.c`/`btif_dma_priv.h`) rather than
  going through the dmaengine framework. Implementation: `btif_tx()`/
  `btif_rx_drain()` rewritten around a new `struct btif_ctx` (BTIF
  regs + TX/RX DMA channel regs + `dma_alloc_coherent()` ring buffers),
  a `btif_dma_arm()` helper that re-arms each channel (warm reset,
  ADDR/LEN/THRE, WPT=RPT=0, enable) before every single TX/RX burst
  instead of carrying the vendor's persistent wraparound-capable ring —
  correct here because every burst (max 1005B fragment, max ~64B
  reply) is far smaller than the 4KB ring, so wraparound is provably
  never exercised. `patches/v6.6/dts/0013` gains a second clock
  (`CLK_INFRA_AP_DMA`, `clock-names = "apdma"`) matching the vendor
  DTB's `btif@1100c000` clocks = "btifc", "apdmac". `git apply --check`
  verified clean for both patches against a fresh v6.6 tree, together
  with every other patch in the tree (no cross-patch conflicts).
  **Built and tested 2026-07-21 (builds #274/#275): DMA mode does NOT
  fix G2b — same `-110` timeout as PIO (#262).** This cleanly rules out
  the PIO-vs-DMA transport delta; the remaining open suspect is an
  upstream precondition (`0x10001f00` bit 11, or an SPM/EMI sequencing
  step vendor LK does that our direct-boot path skips). Full trace:
  `consys-spike` completes cleanly with no hang/crash (`GATE G2B FAIL
  (-110) - state left up for devmem inspection`), confirmed via panel-
  console video captures (serial is blind here — dies at the B-15
  USB-mux point, ~t=0.45s, before `consys-spike` runs).
  **Unexpected second regression found and left unresolved:** both
  #274 and #275 (apdma clock made optional in #275 to isolate it —
  ruled out, same failure persists) hit `/dev/mmcblk0p29: Can't lookup
  blockdev` at boot, while the pre-DMA baseline (#269) mounts the eMMC
  rootfs cleanly. Root cause not found — leading candidates are the
  BTIF DMA channel register pokes themselves (`btif_dma_arm()`,
  `0x11000a00`/`0x11000a80`, never touched by any prior build) or the
  two `dma_alloc_coherent()` calls, neither investigated further.
  **Session closed 2026-07-21 without pursuing this further**: Stage W4
  had already answered its primary question (DMA doesn't fix G2b), so
  debugging a second, unrelated storage regression in service of an
  already-failed experiment wasn't judged worth more build cycles.
  Device reflashed back to build #269 (stable baseline). Full narrative:
  boot.md 2026-07-21 entries "Stage W4 BTIF DMA transport patch
  written", "Build #274 flashed", "Build #275 tested".
  **If B-21 is resumed, do not re-attempt DMA mode without first
  root-causing the mmc regression** — `patches/v6.6/soc/0003` and
  `patches/v6.6/dts/0013` on disk currently contain this DMA/apdma
  work; reverting to the pre-Stage-W4 PIO version (matching #269) may
  be the safer starting point if picking B-21 back up.
- **Stage W3:** go/no-go on the full gen2 port (frank-w 5.6→6.6 delta
  audit); if GO, port order = WMT core → AHB HIF → cfg80211 glue, WiFi
  only. Given the hypothesis-1 conclusion above, G2b as originally
  defined (ROM-only handshake) is not a fair pass/fail gate — Stage W3
  should re-scope the gate to require pushing the real WMT firmware
  patch (already extracted to `docs/firmware-consys/`) as part of the
  spike before judging G2b, or fold G2b into the full gen2 port
  decision directly.

**Risk:** highest-uncertainty workstream in the project; the gates exist
precisely because the gen2 port may prove uneconomical. NO-GO returns
WiFi to the (parked) USB path.

- **2026-07-16 — extracted firmware review (`docs/firmware-consys/`):**
  reviewed the CONSYS firmware set pulled from the vendor image before
  attempting the G2b re-scope. Contents:
  - `ROMv3_patch_1_0_hdr.bin` (211,908 B) / `ROMv3_patch_1_1_hdr.bin`
    (46,472 B) — MediaTek ALPS ROM-patch container format (`ALPS` magic
    at offset 0x0C, build timestamp `20180615091545a`, multi-segment
    offset/length table). Filenames match exactly what `wmt_launcher`
    was observed pushing in the boot-timing capture above, so this is
    confirmed to be the right firmware, not a guess.
  - `WIFI_RAM_CODE_6797` (451,904 B) — `MTKE` magic at offset 0x00,
    chip-specific to 6797 per filename; this is the WiFi RAM-code image
    loaded after the ROM patches.
  - `WMT_SOC.cfg` — plaintext board config (coex antenna mode, GPS LNA
    pin disabled, `co_clock_flag=0`); trivial, no parsing concerns.
  - `wmt_launcher` / `wmt_loader` — vendor userspace ELF binaries,
    reference only.
  - No corruption or chip-mismatch red flags found. Gap: no local parser
    for the ALPS ROM-patch segment table/checksums, and no public spec
    to validate against — sha256 of each file recorded above for future
    integrity checks.
- **2026-07-16 — WMT firmware-push protocol extracted and spike extended
  (Stage W3 / G2b re-scope, not yet built).** Full protocol writeup:
  research.md "WMT Firmware-Push Protocol". Key findings:
  - The SoC patch path is `mtk_wcn_soc_patch_dwn()` in `wmt_ic_soc.c`
    (NOT the `opfunc_flash_patch_*` ops in `wmt_core.c` — those are for
    external-flash chips). Our extracted blobs are directly downloadable:
    28-byte `WMT_PATCH` header (datetime/"ALPS"/HwVer 0x8a00 — matches
    the HW_VER we read live) + body pushed as 1000-byte `WMT_PATCH_CMD`
    fragments (`01 01 len flag`), evt `02 01 01 00 00` per fragment,
    `WMT_RESET` after each patch. Per-patch RAM address and download
    order come from header bytes 24-27 (launcher `srh_patch()`):
    `_1_1` = seq 1 → 0xF00A0000-style addr bytes `00 00 0a f0`;
    `_1_0` = seq 2 → `00 00 09 00`. Preceded by two 6797-specific
    reg-write commands (opcode 0x08) to 0x02090508/0x02090b2c, the DLM
    power-on writes, and MCU-clock speed-up/restore tables.
  - **Vendor source contradicts strong hypothesis 1:** `sw_init` sends
    `WMT_QUERY_STP` pre-patch in mand mode and ABORTS if unanswered —
    so on working hardware the ROM alone does answer the query. The
    firmware push therefore can't be what unlocks the query, but the
    re-scoped gate is still "push firmware, then query" and the push's
    opcode-0x08 commands double as a diagnostic (does the ROM ignore
    only opcode 0x04, or all BTIF traffic?).
  - **Implemented in soc/0003** (regenerated with all hunks, verified
    `git apply --check` clean + 3 diff sections present): ROM-only query
    kept (result logged as PASS/FAIL but non-fatal), then DLM + mcuclk
    tables (non-fatal, vendor-matching), both patches pushed in seq
    order via `request_firmware()`, final `WMT_QUERY_STP` = the
    re-scoped **Gate G2b** pass/fail line. Blobs are built into the
    kernel image via `CONFIG_EXTRA_FIRMWARE` (spike probes before
    rootfs mount) — `configs/gemini-consys.config` gained
    `CONFIG_EXTRA_FIRMWARE(_DIR)` and `scripts/build-pack.sh` now
    rsyncs `docs/firmware-consys/` into the VM. `btif_rx_drain()`
    gained a 30ms idle-exit so ~260 fragment acks don't serialize on
    the 1s timeout.
  - **Build #262** (`consys-g2b-fw-push`, sha256 `c8a958d2…`,
    `logs/2026-07-16-262-consys-g2b-fw-push/`) is this change, packed
    and banner-verified, firmware confirmed embedded in vmlinux.
    Awaiting flash of `boot2` + serial capture (boot.md #262 entry has
    the expected-outcome checklist).

## 🟢 B-22 — RESOLVED 2026-07-16: right-port USB host (MUSB) + left-port charging work simultaneously — opened 2026-07-15

**RESOLVED — build #255 verified on hardware.** With the charger plugged
into the LEFT port and the RTL8156 ethernet adapter in the RIGHT port at
the same boot: `bq25890-charger-0` reports `status=Charging`,
`online=1`; `enxec9a0c162365` (right-port RTL8156) shows live RX/TX
traffic with zero errors; the left port's `usb0` gadget (RNDIS) also came
up automatically (a bonus of restoring `mediatek,force-b-session-valid`
in dts/0015 — gadget mode auto-enumerates again whenever a host is
present on the left port, same as pre-B-19 behaviour). This is the first
time charge-left + ethernet-right has been confirmed working together —
the original goal of this blocker. Full chain of fixes across builds
#252→#255, in order: (1) full-speed cap to keep the right-port MUSB link
inside the pad chain's limits (usb/0002 `maximum-speed`), (2) vendor-
accurate `num_eps=6`/trimmed EP1-5 FIFO table replacing mainline's
MT8516-shaped 8-EP config (fixed the actual bulk-data TX-stuck/three-
strikes failure, build #254), (3) `multipoint` staying at mainline's
`true` default (build #253's `multipoint=false` regressed enumeration
itself — musbfsh doesn't support multipoint addressing but mainline's
default value doesn't harm it either), (4) retiring the LEFT port's
leftover B-19 host-mode DTS (`dts/0012` behaviour) back to
`dr_mode="peripheral"` (`dts/0015`, build #255) so BQ25896 isn't forced
into OTG-source mode at every boot, blocking charger input.

**FOLLOW-UP 2026-07-20 — full-speed cap (fix #1) retired, build #269
awaiting flash:** iperf3 measured ~7 Mbit/s both ways on the right-port
ethernet — the DTS cap itself is the ceiling. Fix #1 was based on the
signal-integrity theory that build #252 falsified; the real defect was
fixed by #2 (num_eps=6, build #254), and the cap was never re-tested.
`dts/0019` re-enabled `maximum-speed = "high-speed"`; details boot.md
BUILD #269.

**OUTCOME 2026-07-20 (final) — HS works, adapter-dependent; dts/0019
stays:** first #269 boot with an RTL8156 2.5G adapter enumerated HS then
died at t=131s (ep3/ep2 RX three-strikes → Babble → disconnect, MUSB host
wedged until reboot — no re-enumeration on replug). Initially read as
"the FS cap is load-bearing" and build #270 (full-speed revert) was
prepared — but a reboot with the SZNX 100M cdc_ether adapter overturned
that: SZNX runs at HS stably (zero link errors after >200 MB iperf3,
15-min babble watch clean). So HS is good with the SZNX; the RTL8156 is
the incompatible device (candidate factors: missing rtl8156b-2.fw
firmware — install `firmware-realtek` — or its 2.5G-class USB behaviour).
dts/0019 re-enabled; #270 kept unflashed as fallback
(`logs/2026-07-20-270-revert-right-port-full-speed/`). Throughput at HS:
43/51 Mbit/s single-stream, 40/64 Mbit/s -P4 — ceiling is
CONFIG_MUSB_PIO_ONLY + single-buffered 512B FIFOs (downlink RX overruns
→ heavy TCP retransmits), not the link. Known residual risk: a babbling
device wedges the port until reboot. Details boot.md "#269 OUTCOME
REVISED".

**UPDATE 2026-07-16 (latest) — root cause found for "charger plugged in
but not charging": LEFT port DTS still forced host mode, unconditionally
enabling the BQ25896 OTG boost at every boot.** User plugged a charger
into the left port to test the B-22 goal; `power_supply` sysfs stayed
`Discharging`/`online=0`. dmesg showed `bq25890-charger 0-006b: enabling
OTG boost (watchdog re-disabled)` firing at 3.156s on every boot,
regardless of charger presence — traced to `patches/v6.6/dts/0012` (the
now-superseded B-19 Stage C left-port-host overlay): the `ssusb` node
still had `dr_mode="otg"` + `role-switch-default-mode="host"` +
`vbus-supply=<&otg_vbus>`, so `mtu3`'s role-switch probe auto-enables the
OTG boost regulator unconditionally, putting the charger IC into
source/OTG mode — which cannot simultaneously sink an external charger.
This was pure leftover: host duty moved to the RIGHT port (usb1/MUSB) in
build #248, but the LEFT port's DTS was never reverted to
peripheral-only. **Fix: new patch `dts/0015`** (applies after 0014,
built as **BUILD #255**) restores `dr_mode="peripheral"` on `ssusb`,
drops the `xhci` child node and OTG/role-switch/vbus-supply properties,
and swaps `u2port0`'s `mediatek,force-usb-host` back to
`mediatek,force-b-session-valid` (B-20's original device-role force —
still needed since this PHY has no hardware VBUS/session sensing).
Banner `#255` verified, DTB grep confirms `dr_mode="peripheral"` (left)
alongside the right port's unaffected `dr_mode="host"` (dts/0014). Full
analysis: boot.md 2026-07-16 "charger plugged in but NOT charging".
**Not yet tested on hardware** — awaiting flash + charger-in-left-port
retest; if `status` reads `Charging`/`online=1` with an ethernet adapter
simultaneously in the right port, B-22 closes.

**Why the two ports must be treated as fully separate problems:** the left
and right USB-C ports are driven by two different, unrelated controller IP
blocks — they do not share hardware and cannot share driver settings.
- **Left port** = `xhci-mtk`/`mtu3` at `0x11271000` — MediaTek's modern
  USB3-capable SSUSB controller (xHCI + USB2 companion), driven by the
  mainline `xhci-mtk`/`mtu3` drivers. Both the RTL8156 and Naxiang
  adapters enumerate here cleanly (bus 2 in `lsusb`).
- **Right port** = `usb11`/MUSB at `0x11200000` — a much older, USB2-only
  host-only IP (vendor calls its driver "musbfsh"), driven by mainline's
  generic MUSB core + `drivers/usb/musb/mediatek.c` glue (bus 1 in
  `lsusb`). This is a legacy Mentor Graphics MUSB IP, architecturally
  unrelated to xHCI.
Because they are different silicon with different register layouts, each
enumerates fully independently (separate USB bus, separate root hub, no
shared FIFO or arbitration) — plugging a device into one port has zero
effect on the other. But it also means **the left port's working
configuration cannot simply be copied to the right port**: `xhci-mtk`/
`mtu3` has no equivalent of MUSB's `multipoint`/`num_eps`/per-endpoint FIFO
table concept at all, so there is nothing there to port over. The right
port's correct settings have to come from the vendor's musbfsh driver for
this specific legacy IP block (see the config mismatch below) — proven on
2026-07-16 (both the RTL8156 and Naxiang worked flawlessly on the left
port on the same boot/build #253 where the right port failed for both,
confirming the fault is specific to the right-port MUSB config, not the
adapters or a general USB fault).

**UPDATE 2026-07-16 (latest) — BUILD #254 CONFIRMED: right-port bulk data
works, root cause fixed:** tested on hardware — RTL8156 in the RIGHT port
enumerated full-speed, bound `cdc_ether`, got IP `192.168.100.146`, and
this exact SSH session was carried over that interface end-to-end
(`ip -s link`: 43510B/279pkts RX, 22182B/78pkts TX, zero errors/drops; no
watchdog timeout, no three-strikes, no babble, no TXPKTRDY-stuck). Left
port's gadget (`usb0`) was confirmed `DOWN`/no-carrier during the capture,
so there is no ambiguity about which port carried the traffic. **Root
cause: `num_eps=6` + the trimmed EP1-5 512B FIFO table (matching vendor
musbfsh_config_mt65xx) was the real fix for the original TX-stuck/
three-strikes bulk failure; `multipoint=false` (added alongside it in
#253) was an incorrect extra change that broke control-transfer
enumeration entirely — reverting multipoint to `true` (mainline default)
while keeping the EP/FIFO fix resolved everything.** Full analysis:
boot.md 2026-07-16 (later) "BUILD #254 flashed and tested: RIGHT-PORT BULK
DATA WORKS". **Remaining before this blocker can close:** the actual B-22
success gate — charger plugged into the LEFT port while the ethernet
adapter runs on the RIGHT port simultaneously, confirmed via
`power_supply` sysfs showing "Charging"/`online=1` while network traffic
keeps flowing on the right interface. Not yet tested (no charger was
connected during the #254 capture — battery read "Discharging").

**UPDATE 2026-07-16 (later) — BUILD #253 REGRESSED right-port enumeration
itself; BUILD #254 isolates the variable:** #253 (multipoint=false +
num_eps=6 combined) made things worse than #252: both the RTL8156 and
Naxiang, tried on the right port, got stuck in an endless
`device descriptor read/all, error -71` retry loop — never even
completing enumeration, let alone reaching bulk. #252's 8-EP/
multipoint=true baseline at least enumerated the Naxiang fully before
failing at bulk. On the SAME boot, both adapters worked flawlessly when
swapped to the LEFT port (confirmed over SSH, .145/.146) — proof the
regression is specific to the right-port musbfsh config change, not the
adapters or a general fault (see "why the two ports must be treated as
fully separate problems" above). **Build #254** (packed+banner-verified,
`logs/2026-07-16-254-b22-right-port-multipoint-revert/`) reverts
`multipoint` back to `true`, keeping only `num_eps=6`/the trimmed EP1-5
FIFO table, to determine which half of the vendor-config change was
responsible. Full analysis: boot.md 2026-07-16 "BUILD #254".

**UPDATE 2026-07-16 (earlier) — FS test failed, root-cause candidate found, BUILD #253 ready to flash:**
the #252 full-speed experiment ran (dmesg read live over SSH): Naxiang
enumerated at full speed on the right port, then bulk still died —
TX watchdog (zero completions), TX2 FIFO stuck `csr: 2003` (TXPKTRDY
never clears), `ep2 RX three-strikes`. HS-signal-integrity theory
FALSIFIED → MAC/glue layer. Root-cause candidate from vendor source:
mainline mediatek.c uses the MT8516 OTG config (`num_eps=8`,
`multipoint=true`, EP1–7 FIFO) but MT6797 usb11 is the musbfsh IP —
vendor `musbfsh_config_mt65xx` = **num_eps=6, multipoint=false, EP1–5
512B single-buffered**. multipoint=true addresses bulk via per-EP
TXFUNCADDR/busctl registers this hardware lacks (EP0 works via FADDR,
bulk dies — exact symptom match). **Build #253** (usb/0002 extended:
`mediatek,mt6797-musb` compatible → musbfsh config via match data;
dts/0014 compatible switched; FS cap + PIO kept, single variable) is
packed and banner-verified — flash `boot2` from
`logs/2026-07-16-253-b22-right-port-musbfsh-config/`, then Naxiang in
RIGHT port; success = no watchdog/three-strikes + traffic on .146 (check
the right interface's own counters, ARP-flux warning below), then the
gate: charger LEFT + ethernet RIGHT simultaneously. Full analysis:
boot.md 2026-07-16. If #253 works, later single-variable retests: drop
FS cap (HS may have been this bug all along), then DMA.

**Goal:** host on the RIGHT port (vendor `usb1@11200000`, MUSB) so the
left port is free for charging — device currently runs on battery when
the left port carries the ethernet adapter. Charge-left + host-right is
proven vendor behaviour and right-port VBUS is independent of the
BQ25896 boost (GPIO94+GPIO72 only; live-proven, boot.md 2026-07-15).

**Works (builds #249–#252):**
- `patches/v6.6/usb/0002`: mtk-musb glue — clocks made optional (MT6797
  has only infra icusb as "main") + DT `maximum-speed` honored
  (musb_dsps pattern).
- `patches/v6.6/dts/0014`: second generic-tphy-v1 @0x11210000
  (u2port1@11210800, `mediatek,force-usb-host`, clk26m ref — vendor
  usb11 PHY is byte-for-byte tphy-v1 layout, no new driver needed);
  `usb1@11200000` on `mediatek,mtk-musb` (SPI 73 level-low,
  CLK_INFRA_ICUSB, dr_mode="host", maximum-speed="full-speed"); 4 hogs:
  GPIO94/72 (VBUS) + GPIO70 hi/71 lo (vendor USB-OTG mux position; 70
  low = HDMI alt-mode).
- phy/0001's `mediatek,force-usb-host` extended (#250) to the FULL
  vendor host state: IDDIG=0 + FORCE_SESS_MSK + RG vbusvalid/avalid/
  bvalid + SESSEND=0 + SUSPENDM — without this musb loops
  `VBUS_ERROR in a_idle (<SessEnd)` (vendor musbfsh forces the same).
- Devices ENUMERATE on the musb bus (Naxiang cdc_ether, RTL8156).
- **Vendor babble recovery proven live over devmem:** DTM1(u2port1)=
  0x1121086C: write 0x3E10 (sessend pulse) → ~200ms → 0x3E2C (session
  restore) flips DEVCTL 0x99(b_idle, wedged)→0x5D(host) and the device
  re-enumerates instantly. Candidate for a proper hook in
  musb babble recovery or the tphy driver.

**Broken:**
1. **Bulk data never flows at high speed**: cdc bulk-IN dies
   `ep2 RX three-strikes error` ×N → `Babble` → OTG FSM falls to b_idle
   (recoverable only via the devmem pulse above or reboot). TX counter
   stays 0 (urbs submitted, zero completions). MAC state is
   textbook-healthy (DEVCTL 0x5D, INTRTXE/RXE + DMA unmask all correct).
   Working theory: HS signal integrity through the external
   SW7226/FUSB301a mux chain — hence #252's full-speed cap.
2. **Inventra DMA (build #251) hard-crashes the SoC** — repeated
   green-screen panics/hangs within minutes, no pstore record (ramoops
   IS bound and mounted at 44410000 — crashes are bus lockups that never
   reach the panic path). Suspected rogue/unclocked DMA bus master (the
   glue's "mcu" clock has no MT6797 equivalent). DMA stays OFF
   (MUSB_PIO_ONLY=y) until understood.
3. Mainline glue **unbind oopses** (NULL deref in devm_usb_phy_release)
   — never unbind musb; upstream bug, not chased.

**RESUME HERE (build #252 is flashed, banner verified, FS cap active):**
plug a USB device (Naxiang adapter, .146 static) into the RIGHT port and
test whether bulk finally flows at full speed: expect dmesg
"new full-speed USB device number N using musb-hdrc" (NOT high-speed),
then ping/SSH via it with the left adapter unplugged or the route
pinned (beware Linux ARP-flux false positives — the left interface
answers ARP for the right one's IP; check the RIGHT interface's RX/TX
counters, not just ping success). If FS bulk works → right port is
SSH-grade usable; write it up, then optionally chase HS (PHY eye/slew
tuning vs mux) and DMA later. If FS bulk ALSO three-strikes → the
problem is not HS signal integrity; next suspects = musb PIO IRQ
handling on this glue / the missing "mcu" bus clock.

**Static IPs (rootfs, by MAC):** RTL8156 = 192.168.100.145, Naxiang =
192.168.100.146. No default route on either. RTL8156-on-left passed no
traffic once on #252 with the newly-installed rtl8156b-2.fw — if it
recurs, delete /lib/firmware/rtl_nic/rtl8156b-2.fw (was working
fw-less). /root/h.sh + /root/s.sh have the left-port recovery/status
pokes; the babble-recovery devmem pair above is NOT yet in h.sh.

**Related:** B-19 (left port, RESOLVED — including these left-port
follow-ups: bq25890 boost does not self-resume after charger removal;
xhci misses disconnect events, recovers on next connect; two-crash
"reboot panic" mystery from #248 remains unexplained). CLAUDE.md Phase 8
table row updated 2026-07-15.

## 🟡 B-23 — PARKED 2026-07-20: loudspeaker silent on mainline despite vendor-exact enable replication — opened 2026-07-19

Headphones work (#267, all-mainline). Speaker is silent although every
element of the vendor mechanism was verified or replicated bit-exactly
on build #268: LOL analog path live (DAPM + register dump), enable =
GPIO243/244 3×2 µs low→high pulses ending high (AW8736-style, source-
confirmed in gemian 3.18 `AudDrv_GPIO_EXTAMP_Select` which reuses the
HPDEPOP pinctrl slots; pin identity triple-sourced from real DTB
`pins <0xf300>/<0xf400>`, DWS, and shipped-kernel disassembly), pulses
driven at register speed via /dev/mem with pad mode/dir/DOUT readback
confirmed, vendor call-order replicated mid-playback. All silent. Full
evidence: boot.md "#268 OUTCOME". No hidden i2c amp (full bus scan);
MAX98926 unstuffed. Speaker works on stock Android, so hardware is fine
— we are missing an invisible precondition (amp supply rail, board
rework pad, or an entirely different enable).

**Resume plan — HARVEST SESSION on vendor Kali 3.18 (root shell,
unlike Android):** next time `planet/linux.img` + Kali boot chain is
flashed, capture everything in ONE session while playing audio to the
speaker:

1. GPIO state of ALL 262 pins while speaker audible: `/sys/devices/virtual/misc/mtgpio/pin`
   (root can read it there) — diff speaker-on vs speaker-off vs
   headphone-on.
2. MT6351 full register dump speaker-on vs off (vendor debug nodes or
   /proc/asound; do NOT touch `*_access` sysfs nodes — they crash the
   vendor kernel, see memory).
3. dmesg with `Ext_Speaker_Amp_Change`/`Speaker_Amp_Change` pr_debug
   enabled (dynamic debug) to see which functions actually run and
   which aud_gpios entries have `gpio_prepare` true.
4. /proc/asound cards/pcm, active mixer control values (tinymix dump)
   in speaker mode.
5. PMIC LDO/BUCK enable states (charger/regulator sysfs, not *_access).
6. Anything else pending at that time (check open blockers before
   booting the harvest session so it is all done at once — reflashing
   Kali/Debian back is slow).

**Cost note:** harvest requires flashing `planet/linux.img` over the
Debian rootfs (p29) and restoring afterwards (`mtk w linux`, 5.5 GB,
slow) — that is why this is batched, per user decision 2026-07-20.

**Batched session planned 2026-07-20:** this checklist is folded into
the consolidated Kali harvest session (instrumented vendor kernel on
the `boot` partition, boot2/#269 untouched) together with B-21
BT/CONSYS, camera, LTE, mic and WiFi captures — see
[docs/kali-harvest-plan.md](docs/kali-harvest-plan.md).
