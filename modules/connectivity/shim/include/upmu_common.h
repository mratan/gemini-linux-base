/* Shim: legacy MTK PMIC wrapper (MT6351) — NOT WIRED on the 6.6 Base tree.
 *
 * The vendor 3.18 tree pokes MT6351 VCN LDO control bits through the legacy
 * MTK PMIC framework (pmic_set_register_value()). The Base tree accesses the
 * PMIC through its minimal pwrap regulator driver instead, and how these
 * bits get wired is a runtime-slice decision (issue #10/#11 territory).
 *
 * !! RUNTIME WARNING (API-CHURN-LOG.md N2, semantic, novel) !!
 * These stubs make the calls compile but DO NOTHING. In particular
 * RG_VCN28_ON_CTRL=1 (HW clock-control mode, written before VCN28 enable)
 * is on the divergence-debug plan's not-yet-eliminated list for the G2b
 * handshake failure — this shim MUST be replaced by real pwrap writes
 * before any on-device CONSYS bring-up. The pr_warn_once below exists so a
 * booted system shows the gap in dmesg instead of failing silently.
 */
#ifndef __SHIM_UPMU_COMMON_H__
#define __SHIM_UPMU_COMMON_H__

#include <linux/printk.h>

enum shim_mt6351_pmic_reg {
	MT6351_PMIC_RG_VCN18_ON_CTRL,
	MT6351_PMIC_RG_VCN28_ON_CTRL,
	MT6351_PMIC_RG_VCN33_ON_CTRL_BT,
	MT6351_PMIC_RG_VCN33_ON_CTRL_WIFI,
};

static inline void pmic_set_register_value(int reg, unsigned int val)
{
	pr_warn_once("consys-shim: pmic_set_register_value(%d,%u) is a stub — VCN ON_CTRL bits NOT written (see modules/connectivity/API-CHURN-LOG.md N2)\n",
		     reg, val);
}

static inline void mt6351_upmu_set_rg_vcn33_on_ctrl(unsigned int val)
{
	pmic_set_register_value(MT6351_PMIC_RG_VCN33_ON_CTRL_BT, val);
}

#endif /* __SHIM_UPMU_COMMON_H__ */
