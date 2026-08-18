# Vendored connectivity stack — provenance

Slice 5 (tracker issue #6 on mratan/gemini-linux), vendored 2026-08-12.

| | |
|---|---|
| Source repo | https://gitlab.com/ubports/porting/community-ports/android9/planet-geminipda/kernel-planet-geminipda |
| Branch | `halium-9.0` (Linux 3.18.60, the Port source per CONTEXT.md/ADR-0001) |
| Commit | `28ffb22d8891b795c0d8847a3a29cf37eced6baa` |
| Copied trees | `drivers/misc/mediatek/connectivity/common/common_main` → `common_main/`; `.../common/common_detect` → `common_detect/`; `drivers/misc/mediatek/btif/common` → `btif/`; `.../connectivity/wlan/gen3` → `wlan/gen3/` and `.../wlan/wmt_chrdev_wifi.c` → `wlan/` (Slice 9) |
| Config baseline | `arch/arm64/configs/k97v1_64_bsp_defconfig` (the Gemini's own defconfig: `CONFIG_MTK_BTIF=y`, `CONFIG_MTK_COMBO=y`, `CONFIG_MTK_COMBO_CHIP_CONSYS_6797=y`; `CONFIG_MTK_CLKMGR` and `CONFIG_MTK_LM_MODE` unset — both facts encoded in Kbuild flags/shims) |

`shim/include/` holds two kinds of files, distinguishable by header comment:
- **Copied vendor headers** (unmodified, same tree/commit as above): `mt_io.h`,
  `sync_write.h`, `mt_lpae.h`, `aee.h`, `mt-plat/mtk_ram_console.h`,
  `mtk_wcn_cmb_stub.h`.
- **Written shims** (replacements for removed kernel APIs or absent vendor
  subsystems; each carries a header comment and an API-CHURN-LOG.md tag):
  `linux/wakelock.h`, `emi_mpu.h`, `upmu_common.h`, `mtk_rtc.h`.

Object selection (`Kbuild`) mirrors the vendor `CONFIG_MTK_COMBO=y` +
`CONSYS_6797` build: external-chip ICs (`wmt_ic_6620/6628/6630/6632`),
external transports (`hif_sdio`, `stp_sdio`, `stp_uart`), `stp_dbg_combo`,
and the LTE-coex `wmt_idc` path are excluded; from `common_detect`, only
`mtk_wcn_stub_alps.c` (cmb-stub callback registry, chip-id query) and
`wmt_gpio.c` (gpio_ctrl_info table) are built — plus, since 2026-08-18,
`wmt_detect.c` as its own `mtk_wmt_detect.ko` (the `/dev/wmtdetect` char
device the vendor `wmt_loader` drives), with the external-combo probe
paths (`sdio_detect.c`, `wmt_detect_pwr.c` — both need 3.18-only headers)
replaced by the SoC-only stubs in `wmt_detect_soc_stubs.c`. The `drv_init`
tree stays excluded (`MTK_WCN_REMOVE_KO=0`: our drivers are separate
modules loaded by insmod, not by the DO_MODULE_INIT ioctl).

### Gen3 WLAN (Slice 9, tracker issue #10)

`wlan/gen3/` object selection mirrors the vendor gen3 Makefile for
`WLAN_CHIP_ID=MT6797`: the `ahb_sdioLike/` HIF and `plat/mt6797/` are built;
the SDIO HIF (`os/linux/hif/sdio/`) is not. **gen2 is not vendored at all** —
the tree contains only `wlan/gen3/`, which is the strongest form of the "no
gen2 built anywhere" guard (issue #10); the Kbuild header states the rule.
`configs/gemini-wlan-gen3.config` enables the cfg80211 features the driver
hard-requires (`NL80211_TESTMODE`, `CFG80211_WEXT`). `wlan/gen3/lint/` was
dropped (vendor static-analysis noise, not built).

Modules produced (all pass modpost against the Base tree kernel):
`mtk_btif.ko`, `mtk_stp_wmt_soc.ko`, `wlan_gen3.ko`. (Slice 9 also built a
standalone `wmt_chrdev_wifi.ko`; it is now linked into `wlan_gen3.ko` — see the
next paragraph and API-CHURN-LOG G20.)

### Gen3 chardev merge (tracker issue #22)

The `/dev/wmtWifi` power gate (`wlan/wmt_chrdev_wifi.c`) and the gen3 driver
exported symbols to each other, forming a bidirectional `EXPORT_SYMBOL` cycle
that `depmod` cannot order (blocks runtime module loading; compile/modpost were
unaffected). The chardev is now compiled *into* `wlan_gen3.ko` rather than as a
separate module, collapsing every chardev↔gen3 symbol edge to an intra-module
link. Load order is now acyclic: `mtk_btif` → `mtk_stp_wmt_soc` → `wlan_gen3`.
Full rationale and the init/exit wiring are in API-CHURN-LOG.md **G20**.

All 6.6-compatibility modifications to vendored sources are logged in
`API-CHURN-LOG.md` — one entry per change, tagged mechanical/semantic and
frank-w-precedented/novel.
