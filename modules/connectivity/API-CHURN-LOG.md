# API-churn log: vendor 3.18 connectivity stack → Linux 6.6

One entry per class of change made to the vendored sources (see PROVENANCE.md
for what was vendored). Tags: **[mechanical]** = same behavior, new spelling;
**[semantic]** = behavior or wiring actually changes, read the entry.
**[frank-w]** = the BPI-R2 port hit and solved the same churn
(04-docs/mirrors/frank-w/ in the project repo has the catalog);
**[novel]** = post-5.10 churn or Gemini-specific, no precedent.

## Mechanical

- **M1 [frank-w] timeval → timespec64** — `struct timeval`,
  `do_gettimeofday()` removed (kernel 5.0 era; gone by 5.6). Replaced
  tree-wide with `struct timespec64` + `ktime_get_real_ts64()`;
  `tv_usec` reads became `tv_nsec / 1000`. Debug/timing paths only.
  Files: `btif/mtk_btif.c`, `btif/btif_dma_plat.c`, `btif/inc/mtk_btif.h`,
  `common_main/core/psm_core.c`, `common_main/linux/{osal,stp_dbg,wmt_dev}.c`
  (+ unbuilt `stp_uart.c`, `stp_sdio.c` touched by the same sweep).
- **M2 [novel] dma_zalloc_coherent → dma_alloc_coherent** — removed 5.0;
  dma_alloc_coherent zeroes since 5.0. `btif/mtk_btif.c`.
- **M3 [novel] class_create(THIS_MODULE, name) → class_create(name)** —
  module param dropped in 6.4. `btif/mtk_btif.c`,
  `common_main/linux/wmt_dev.c`, `common_detect/wmt_detect.c`.
- **M4 [frank-w] show_stack() → sched_show_task()** — moved headers in the
  4.11 split, gained a loglvl arg in 5.8, and is not exported to modules on
  6.6 at all; `osal_thread_show_stack()` now uses `sched_show_task()`
  (EXPORT_SYMBOL_GPL, same task-stack dump) and no longer `return`s a void
  expression. `common_main/linux/osal.c`.
- **M5 [frank-w] sched header split** — `local_clock()` needs
  `<linux/sched/clock.h>`. `common_main/linux/osal.c`.
- **M6 [frank-w] DRIVER_ATTR → DRIVER_ATTR_RW** — macro removed 4.13;
  show/store renamed to `flag_show`/`flag_store` per the macro contract.
  `btif/mtk_btif.c`.
- **M7 [frank-w] proc file_operations → proc_ops** — 5.6. Handler
  signatures unchanged. `common_main/linux/wmt_dbg.c`,
  `common_main/linux/wmt_dev.c`.
- **M8 [novel] explicit `<linux/pinctrl/consumer.h>`** for
  `devm_pinctrl_get()` — no longer pulled in transitively.
  `common_main/platform/mtk_wcn_consys_hw.c`.
- **M9 [novel] `<mt_clkbuf_ctl.h>` include dropped** — zero `clk_buf_*`
  users in `common_main/platform/mt6797.c`; header doesn't exist outside
  the vendor tree.
- **M10 [novel] ioremap_nocache → ioremap** — removed 5.6; ioremap is
  non-cached on arm64. 7 sites across `common_main/platform/`,
  `common_main/linux/`.

## Semantic — read before on-device work

- **W1 [frank-w] wakelocks → wakeup sources** (`shim/include/linux/wakelock.h`)
  — the vendor wrapper embedded `struct wakeup_source` and used
  `wakeup_source_init/trash` (removed by 5.19). The shim holds a
  `wakeup_source_register()`ed pointer instead; same wake_lock_* call sites.
  Matches BPI-Router-Linux commit `922ecdd3bb`. Behavior-equivalent, but
  allocation can now fail — `wake_lock_active(NULL-ws)` returns 0.
- **T1 [frank-w] timer_setup conversion** (`common_main/linux/osal.c`) —
  `init_timer()`/`timer_list.data` removed in 4.15. OSAL is the single choke
  point: a trampoline recovers `P_OSAL_TIMER` via `from_timer()` and calls
  the vendor handler with its original `ULONG` argument. No caller changed.
- **S1 [frank-w] genetlink registration** (`common_main/linux/stp_dbg.c`) —
  `GENL_ID_GENERATE` and `genl_register_family_with_ops()` removed (4.10);
  per-op `.policy` moved to the family (5.2). Family now carries
  ops/n_ops/policy/resv_start_op/module and is defined after the ops array.
  Used by the coredump netlink push to `stp_dump3` — verify with the native
  daemon in Slice 7.
- **N1 [novel] EMI MPU stub** (`shim/include/emi_mpu.h`) — the Base tree has
  no MTK EMI MPU driver; `emi_mpu_set_region_protection()` is a no-op, so
  the CONSYS EMI window runs unprotected. bsg100 tested and eliminated
  EMI-MPU as a G2b factor (blockers.md B-21 #247), and their 6.6 builds ran
  without it; acceptable for bring-up, revisit for hardening.
- **N2 [novel] MT6351 VCN ON_CTRL — WIRED (was stub)**
  (`shim/mt6351_pmic.c`, `shim/include/upmu_common.h`) —
  `pmic_set_register_value()` now performs real writes through the mainline
  pwrap driver's regmap (same access path as the Base tree's mt6351 VCN
  regulator driver). Register addr/mask/shift verbatim from vendor
  `mt6797/include/mach/upmu_hw.h`: VCN18 0x0A52, VCN28 0x0A0C,
  VCN33_BT 0x0A98, VCN33_WIFI 0x0A9A — all bit 3. The one live call on the
  non-legacy path is `consys_vcn28_hw_mode_ctrl()` → RG_VCN28_ON_CTRL (the
  B-21 suspect bit); the others sit in CONFIG_MTK_PMIC_LEGACY branches this
  port doesn't compile, wired anyway for completeness. Every write logs to
  dmesg. The VCN18/28/33 rails themselves go through the regulator API
  (vendor non-legacy path) against the Base tree's mt6351 driver — supply
  names verified to match dts/0013, and dts/0021 (new) adds the previously
  missing vcn33_bt/vcn33_wifi supplies to the consys node.
  Runtime caveat for the bring-up slice: the Base tree's spike driver
  (soc/0003) binds the same `mediatek,mt6797-consys` node — exactly one of
  spike/vendor-WMT may be enabled in a given build.
- **N3 [novel] RTC 32k GPS clock stub** (`shim/include/mtk_rtc.h`) —
  `rtc_gpio_enable_32k(RTC_GPIO_USER_GPS)` no-op; GPS is out of scope for
  the Prototype. Wi-Fi/BT paths never call it.
- **N4 [novel] AEE telemetry stubs** (`shim/consys_stubs.c`) —
  `aee_kernel_warning_api`, `aed_combo_exception_api`, `aee_kernel_dal_api`
  are Android Exception Engine hooks with no Debian equivalent; pr_warn_once
  no-ops. Exception *detection* (stp_dbg coredump paths) still runs — only
  the AEE hand-off is dropped.
- **N5 [novel] wmt_detect_get_chip_type() → WMT_CHIP_TYPE_SOC constant**
  (`shim/consys_stubs.c`) — the vendor flow sets this via /dev/wmtdetect
  ioctls from wmt_loader; on CONSYS_6797 it is a hardware constant.
  Removes a daemon-ordering dependency; revisit when common_detect's char
  device is brought up for the daemons (Slice 7).
- **N6 [novel] combo/SDIO dead-path stubs** (`shim/consys_stubs.c`) —
  `mtk_wcn_cmb_hw_{init,deinit,pwr_on,pwr_off,rst,state_show}`,
  `mtk_wcn_hif_sdio_{wmt_control,update_cb_reg}`, `stp_sdio_{rw_retry,
  txdbg_dump,dump_register}`, `g_stp_sdio_host_info`,
  `stp_dbg_combo_{core_dump,id_to_task}` — external-chip paths unreachable
  on SoC-integrated CONSYS; WARN_ONCE if ever hit. Related:
  `CFG_CORE_MT66{20,28,30,32}_SUPPORT` now really become 0 when the chip
  macro is absent (`wmt_core.h` — vendor defined 1 in both #if branches),
  compiling out the external-IC dispatch and the `wmt_ic_ops_*` externs.
- **N7 [novel] ⚠ mtk_wdt_swsysret_config() reimplemented**
  (`shim/consys_stubs.c`) — the CONSYS MCU reset hold/release (WDT_SWSYSRST
  bit 12, key 0x88<<24). Mainline mtk_wdt has no such API; the shim does the
  direct AP_RGU register write, same as the Base tree's spike driver did.
  Bring-up-critical: this is the "MCU held in reset until released" step
  from the B-21 timeline.

## Tracked warnings (not silenced; all pre-existing vendor code quality)

Clean-build inventory 2026-08-12 (5 total): enum/int signature mismatches
(`wmt_plat_alps.c:404`, `stp_core.c:1582`), enum conversion
(`btif_plat.c:510/512`), misleading indentation (`psm_core.c:1336`). None
introduced by the port.

## Not ported (excluded from the build instead)

- External-chip support (`wmt_ic_6620/6628/6630/6632.c`, `hif_sdio.c`,
  `stp_sdio.c`, `stp_uart.c`, `stp_dbg_combo.c`) — the Gemini's CONSYS is
  SoC-integrated; the vendor CONSYS_6797 build ships `wmt_ic_soc` + BTIF.
- `wmt_idc.c` (LTE coexistence) — `WMT_IDC_SUPPORT=0`, matches vendor flag;
  pulls eccci headers otherwise.
- `common_detect/` — vendored, not yet built (see PROVENANCE.md).
