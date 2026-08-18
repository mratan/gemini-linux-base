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

---

# Gen3 WLAN driver → Linux 6.6 (Slice 9 / tracker issue #10)

The Gen3 WLAN driver (`wlan/gen3/`, the driver the vendor builds for
CONSYS_6797 — MT6630-class core, SDIO-like AHB HIF) + the Wi-Fi chardev
(`wlan/wmt_chrdev_wifi.c`), vendored from the same UBports tree/commit as the
WMT core (see PROVENANCE.md). Object list mirrors the vendor gen3 Makefile's
`WLAN_CHIP_ID=MT6797` selection (HIF `ahb_sdioLike/`, `plat/mt6797/`); the
SDIO HIF and gen2 are excluded. **gen2 is not vendored into the tree at all**
(the strongest form of "no gen2 built" — there is nothing to build), and the
Kbuild header documents the rule. Config fragment `configs/gemini-wlan-gen3.config`
enables the cfg80211 features the driver requires (`NL80211_TESTMODE`,
`CFG80211_WEXT`). Tags as above ([mechanical]/[semantic], [frank-w]/[novel]).

## Mechanical

- **G1 [frank-w] ftrace_event.h → trace_events.h** (v4.14). `gl_kal.h`.
- **G2 [novel] ACPI_STATE_D0..D3 macro collision** — the kernel's
  `<acpi/actypes.h>` (pulled transitively on 6.6) `#define`s these, clashing
  with the driver's private enum; `#undef` before the enum. `wlan_def.h`.
- **G3/G10 [frank-w] sched.h split** — `sched_clock()` →
  `<linux/sched/clock.h>`; `struct sched_param` → `<uapi/linux/sched/types.h>`;
  `sched_show_task()`/`show_stack()` → `<linux/sched/debug.h>`. Multiple files.
- **G4 [frank-w] set_fs()/KERNEL_DS removed (v5.10)** — file IO via
  `kernel_read()`/`kernel_write()` (kernel buffers, no addr-limit override);
  direct `f_op->read` and `vfs_read/write` call sites converted.
  `gl_kal.c`, `platform.c`.
- **G5 [frank-w] timer_setup() (v4.15)** — `init_timer()`+`.data` gone; OSAL
  timer trampoline recovers the glue via `from_timer()`. `gl_kal.c`.
- **G6 [frank-w] net_device.last_rx removed (v4.11)** — assignment dropped.
- **G9 [novel] MODULE_SUPPORTED_DEVICE removed (v5.12)** — dropped. `gl_init.c`.
- **G15 [novel] ndo_select_queue** — lost `accel_priv` (v4.19) and the
  `select_queue_fallback_t` (v5.2); now `(dev, skb, sb_dev)`. `gl_init.c`.
- **G17 [novel] hardened-usercopy false positive** — `count + 1` (size_t) can
  overflow, so the compiler couldn't prove the `copy_from_user` dest bound
  (`__bad_copy_to`); added an explicit clamp to `sizeof(buf) - 1`. `gl_proc.c`.
- **strnicmp → strncasecmp**, **ioremap_nocache → ioremap**,
  **class_create(THIS_MODULE,) → class_create()**,
  **dma_zalloc_coherent → dma_alloc_coherent**,
  **access_ok(VERIFY_*, …) → access_ok(…)** (v5.0) — as in the WMT section.

## Semantic — read before on-device work

- **G8 [frank-w] iwpriv handler fields** — `iw_handler_def`'s
  `.num_private/.private/.private_args/.num_private_args` exist only under
  `CONFIG_WEXT_PRIV` (a select-only symbol, off in our config); wrapped in
  `#ifdef CONFIG_WEXT_PRIV`. **Consequence: iwpriv commands are unavailable in
  the default build** (standard wext + nl80211 unaffected). `gl_wext.c`.
- **G11 [frank-w] cfg80211_roamed_bss → cfg80211_roamed** (v4.12) with a
  `struct cfg80211_roam_info`; bss/ie fields moved under `links[0]` in the
  v6.0 MLO rework. `gl_kal.c`.
- **G12 [frank-w] cfg80211_ops key/iface/station churn** — `add_key`/`get_key`/
  `del_key`/`set_default_key` gained `int link_id` (v6.0); `change_virtual_intf`
  dropped `u32 *flags` (v4.1); `add_virtual_intf` gained `unsigned char
  name_assign_type` (v4.1); `del_station` takes `struct station_del_parameters *`
  (v4.4); `tdls_mgmt` gained `int link_id`, `stop_ap`/`set_bitrate_mask` gained
  `link_id` (v6.0/v5.19). The driver is non-MLO so link_id is accepted and
  ignored. Applied across `gl_cfg80211.c`, `gl_p2p_cfg80211.c`, and headers.
- **G13 [frank-w] mgmt_frame_register → update_mgmt_frame_registrations** (v5.8)
  — the per-frame-type callback became a bitmask (`mgmt_frame_regs`);
  translated back into the PROBE_REQ/ACTION filter logic. Both AIS and P2P.
- **G14 [frank-w] WIPHY_FLAG_SUPPORTS_SCHED_SCAN removed (v3.15)** — advertised
  via `wiphy->max_sched_scan_reqs = 1` instead. `gl_init.c`.
- **G16 [novel] STATION_INFO_* → BIT_ULL(NL80211_STA_INFO_*)** (v4.0 filled
  bitmap); `ASSOC_REQ_IES` has no NL80211 filled bit — `assoc_req_ies` is
  passed directly to `cfg80211_new_sta`. `gl_cfg80211.c`, `gl_p2p_*`.
- **G18 [novel] cross-module EXPORT_SYMBOL** — `gConEmiPhyBase`,
  `mtk_wcn_consys_hw_wifi_paldo_ctrl` (both in the WMT module, consumed by the
  gen3 AHB HIF) and `g_IsNeedDoChipReset` (gen3, consumed by the chardev) are
  now exported so the four modules link. **Superseded in part by G20:** the
  `g_IsNeedDoChipReset` export (and the chardev's reverse exports back to gen3)
  were removed when the chardev was merged into `wlan_gen3.ko`; the two WMT-core
  exports remain (that edge, mtk_stp_wmt_soc → wlan_gen3, is acyclic).
- **G20 [novel] ⚠ chardev merged into wlan_gen3.ko — EXPORT_SYMBOL cycle broken**
  (tracker issue #22, follow-up to Slice 9/#10). As two separate modules,
  `wmt_chrdev_wifi.ko` and `wlan_gen3.ko` exported symbols to *each other* —
  chardev → gen3: `g_IsNeedDoChipReset` (gl_rst.c, read by the chardev's
  `WMT_CHECK_DO_CHIP_RESET`); gen3 → chardev: `wifi_reset_start`, `wifi_reset_end`
  (gl_rst.c reset FSM) and `register_set_p2p_mode_handler` (gl_init.c). That
  bidirectional edge is a true cycle: neither can insmod first and `depmod`
  cannot emit a `modules.dep` load order (harmless to compile/modpost, so #10
  stayed green, but blocks runtime loading). **Fix:** `wlan/wmt_chrdev_wifi.o`
  is now linked into `wlan_gen3.ko` (Kbuild: appended to `wlan_gen3-y`, kept out
  of the gen3 `WLAN_GEN3_CFLAGS` foreach so it keeps its own
  `CREATE_NODE_DYNAMIC` flag and is *not* built with `-DMT6797`/HIF includes).
  All four symbols become intra-module direct links, so their four `EXPORT_SYMBOL`
  lines were dropped. A single `.ko` may have only one module entry point, so the
  chardev is compiled with `-DMTK_WCN_BUILT_IN_DRIVER` (per-object only — *not*
  tree-wide, which would also disable gl_init.c's `module_init`), exposing
  `mtk_wcn_wmt_wifi_init`/`_exit`; `initWlan()` calls init first (so /dev/wmtWifi
  and the reset mutex exist before bus registration or any reset callback) and
  `exitWlan()` calls exit last, with unwind on the `glRegisterBus` failure path.
  Result: `mtk_btif` → `mtk_stp_wmt_soc` → `wlan_gen3` is acyclic and depmod
  emits a valid order. Runtime note for bring-up: loading `wlan_gen3.ko` now
  *also* creates /dev/wmtWifi (previously a separate insmod); userspace power-on
  sequence via /dev/wmtWifi is otherwise unchanged.
- **G19 [novel] sched_setscheduler unexported (v5.9)** — the Wi-Fi kthread
  priority set uses `sched_set_fifo_low()` (exported); **the vendor's exact
  numeric priority/policy is not reproduced**, only "elevated RT". `gl_init.c`.
- **Also: station_parameters.ht_capa/vht_capa/supported_rates moved into
  `link_sta_params`** (v5.19); **cfg80211_disconnected** gained
  `locally_generated` (v4.1); **cfg80211_sched_scan_stopped/_stop** gained
  `u64 reqid` (v4.11); **cfg80211_vendor_event_alloc** gained `wdev` (v4.1);
  **nla_parse_nested** gained an `extack` arg (v4.12);
  **sched_scan_request.interval → scan_plans[0].interval** (v4.4);
  **sockaddr.sa_data** is now a flexible array (zero the whole struct).
- **N9 [novel] CPU-boost hint no-op** — `kalBoostCpu()` used the MTK PPM driver
  (`mach/mt_ppm_api.h`), absent mainline; now a no-op (throughput hint only,
  not correctness). `plat/mt6797/plat_priv.c`.
- **N10 [novel] firmware paths: basename-strip before request_firmware()** —
  the vendor `wmt_launcher`/`wmt_loader` pass launcher-prefixed absolute
  paths (`-p /lib/firmware` → `/lib/firmware/ROMv3_...`); the vendor
  original opened them via VFS, but the port's `request_firmware()`
  resolves names relative to the firmware search path, doubling the prefix
  (`/lib/firmware/lib/firmware/...` — previously worked around with a
  self-symlink on the image). `wmt_dev_patch_get()` and
  `wmt_dev_is_file_exist()` now strip to the basename. `linux/wmt_dev.c`.
- **N11 [novel] /dev/wmtdetect chardev as mtk_wmt_detect.ko (SoC-only)** —
  `common_detect/wmt_detect.c` built as its own module so the real vendor
  `wmt_loader` replaces the `gemini_chipid_seed` interim shim. External
  combo-chip probe paths (`sdio_detect.c`, `wmt_detect_pwr.c`: 3.18-only
  `<mt_boot.h>`/`<mtk_rtc.h>`, no such hardware on this device) are stubbed
  by `wmt_detect_soc_stubs.c` ("no external chip" answers); `drv_init` tree
  stays out (`MTK_WCN_REMOVE_KO=0` — modules are insmodded, not
  ioctl-inited).

## Tracked warnings (gen3, not silenced — all pre-existing vendor quality)

Clean-build inventory 2026-08-12: 42 total, dominated by enum-conversion (7),
discarded-qualifiers (4), int-conversion (3), address (3); plus
memset-elt-size (2) and stringop-overread (1) worth a look during runtime
bring-up. None introduced by the port; runtime behavior is out of scope for
this slice.
