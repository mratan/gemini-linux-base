# Vendored connectivity stack — provenance

Slice 5 (tracker issue #6 on mratan/gemini-linux), vendored 2026-08-12.

| | |
|---|---|
| Source repo | https://gitlab.com/ubports/porting/community-ports/android9/planet-geminipda/kernel-planet-geminipda |
| Branch | `halium-9.0` (Linux 3.18.60, the Port source per CONTEXT.md/ADR-0001) |
| Commit | `28ffb22d8891b795c0d8847a3a29cf37eced6baa` |
| Copied trees | `drivers/misc/mediatek/connectivity/common/common_main` → `common_main/`; `drivers/misc/mediatek/connectivity/common/common_detect` → `common_detect/`; `drivers/misc/mediatek/btif/common` → `btif/` |
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
`wmt_gpio.c` (gpio_ctrl_info table) are built — the rest (wmt_detect char
device, drv_init tree) waits until the daemons/gen3 slices need
`/dev/wmtdetect`.

All 6.6-compatibility modifications to vendored sources are logged in
`API-CHURN-LOG.md` — one entry per change, tagged mechanical/semantic and
frank-w-precedented/novel.
