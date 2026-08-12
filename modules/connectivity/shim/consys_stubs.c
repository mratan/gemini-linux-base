// SPDX-License-Identifier: GPL-2.0
/* Stubs for vendor-kernel services absent on the 6.6 Base tree, and for the
 * excluded external-combo-chip paths. Every stub here is either (a) a debug/
 * telemetry hook with no functional role in CONSYS bring-up, or (b) a
 * combo-chip (external SDIO/UART chip) path that cannot execute on the
 * Gemini's SoC-integrated CONSYS — guarded by WARN_ONCE so any unexpected
 * call is loud in dmesg instead of silently wrong.
 * See API-CHURN-LOG.md N4/N5/N6.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>

#include "osal_typedef.h"
#include "osal.h"
#include "wmt_detect.h"
#include "stp_sdio.h"
#include "hif_sdio.h"
#include "mtk_wcn_cmb_hw.h"
#include "mtk_wcn_consys_hw.h"

/* --- N7: MTK watchdog SWSYSRST hold/release (REAL implementation) ------ */
/* mt6797.c holds the CONSYS MCU in reset via WDT_SWSYSRST bit 12 around
 * power-on (mtk_wdt_swsysret_config((1<<12), on)). The vendor kernel gets
 * this from its own mtk-wdt driver; mainline mtk_wdt.c has no such API, and
 * the Base tree's CONSYS spike poked the register directly for the same
 * step (bsg100 research.md "AP_RGU +0x18: WDT swsysret bit12 with key
 * 0x88<<24"). This is that poke, wrapped in the vendor signature.
 */
#define SHIM_AP_RGU_PHYS	0x10007000UL
#define SHIM_WDT_SWSYSRST	0x18
#define SHIM_WDT_SWSYSRST_KEY	0x88000000U

int mtk_wdt_swsysret_config(int bit, int set_value)
{
	void __iomem *rgu = ioremap(SHIM_AP_RGU_PHYS, 0x100);
	u32 val;

	if (!rgu) {
		pr_err("consys-shim: cannot map AP_RGU for swsysret\n");
		return -ENOMEM;
	}
	val = readl(rgu + SHIM_WDT_SWSYSRST) & 0x00ffffff;
	if (set_value)
		val |= (u32)bit;
	else
		val &= ~(u32)bit;
	writel(val | SHIM_WDT_SWSYSRST_KEY, rgu + SHIM_WDT_SWSYSRST);
	iounmap(rgu);
	return 0;
}

/* (mtk_wcn_wmt_chipid_query and board_sdio_ctrl come from the vendored
 * common_detect/mtk_wcn_stub_alps.c, which is built as part of this module.) */

/* --- N4: AEE (Android Exception Engine) telemetry — no-op on Debian ---- */
void aee_kernel_warning_api(const char *file, const int line, const int db_opt,
			    const char *module, const char *msg, ...)
{
	pr_warn_once("consys-shim: AEE warning from %s (no AEE on this system)\n",
		     module ? module : "?");
}

void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			     int phy_size, const char *detail, const int db_opt)
{
	pr_warn_once("consys-shim: AEE combo exception suppressed (no AEE on this system)\n");
}

void aee_kernel_dal_api(const char *file, const int line, const char *msg)
{
	pr_warn_once("consys-shim: AEE DAL message suppressed (no AEE on this system)\n");
}

/* --- N5: chip type — this hardware is SoC-integrated CONSYS, always ----- */
ENUM_WMT_CHIP_TYPE wmt_detect_get_chip_type(void)
{
	/* The vendor flow derives this via /dev/wmtdetect ioctls from
	 * wmt_loader; on the Gemini (CONSYS_6797) the answer is a hardware
	 * constant. Hardcoding removes a daemon-ordering dependency.
	 */
	return WMT_CHIP_TYPE_SOC;
}

/* --- N6: external-combo-chip paths (dead on SOC; loud if ever hit) ------ */
MTK_WCN_STP_SDIO_HIF_INFO g_stp_sdio_host_info;

VOID stp_sdio_dump_register(VOID)
{
	WARN_ONCE(1, "consys-shim: stp_sdio_dump_register on SOC path");
}

INT32 mtk_wcn_cmb_hw_pwr_on(VOID)
{
	WARN_ONCE(1, "consys-shim: cmb_hw_pwr_on on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_cmb_hw_pwr_off(VOID)
{
	WARN_ONCE(1, "consys-shim: cmb_hw_pwr_off on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_cmb_hw_init(P_PWR_SEQ_TIME pPwrSeqTime)
{
	WARN_ONCE(1, "consys-shim: cmb_hw_init on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_cmb_hw_deinit(VOID)
{
	WARN_ONCE(1, "consys-shim: cmb_hw_deinit on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_hif_sdio_wmt_control(WMT_SDIO_FUNC_TYPE func_type,
				   MTK_WCN_BOOL is_on)
{
	WARN_ONCE(1, "consys-shim: hif_sdio_wmt_control on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_hif_sdio_update_cb_reg(INT32 (*ts_update)(VOID))
{
	WARN_ONCE(1, "consys-shim: hif_sdio_update_cb_reg on SOC path");
	return -ENODEV;
}

INT32 mtk_wcn_cmb_hw_state_show(VOID)
{
	return 0;
}

INT32 stp_dbg_combo_core_dump(INT32 dump_sink)
{
	WARN_ONCE(1, "consys-shim: stp_dbg_combo_core_dump on SOC path");
	return -ENODEV;
}

PUINT8 stp_dbg_combo_id_to_task(UINT32 id)
{
	WARN_ONCE(1, "consys-shim: stp_dbg_combo_id_to_task on SOC path");
	return NULL;
}

INT32 stp_sdio_rw_retry(ENUM_STP_SDIO_HIF_TYPE_T type, UINT32 retry_limit,
			MTK_WCN_HIF_SDIO_CLTCTX clt_ctx, UINT32 offset,
			PUINT32 pData, UINT32 len)
{
	WARN_ONCE(1, "consys-shim: stp_sdio_rw_retry on SOC path");
	return -ENODEV;
}

VOID stp_sdio_txdbg_dump(VOID)
{
	WARN_ONCE(1, "consys-shim: stp_sdio_txdbg_dump on SOC path");
}

INT32 mtk_wcn_cmb_hw_rst(VOID)
{
	WARN_ONCE(1, "consys-shim: cmb_hw_rst on SOC path");
	return -ENODEV;
}
