/* Shim: legacy MTK PMIC wrapper (MT6351) — WIRED to the pwrap regmap.
 *
 * The vendor 3.18 tree pokes MT6351 VCN LDO control bits through the legacy
 * MTK PMIC framework (pmic_set_register_value()). Here the same calls land
 * on the mainline pwrap driver's regmap — see shim/mt6351_pmic.c for the
 * register table (verbatim from the vendor upmu_hw.h) and the access path.
 * API-CHURN-LOG.md N2. The one call live on the non-legacy vendor path is
 * RG_VCN28_ON_CTRL (VCN28 HW clock-control mode, a B-21 suspect bit).
 */
#ifndef __SHIM_UPMU_COMMON_H__
#define __SHIM_UPMU_COMMON_H__

enum shim_mt6351_pmic_reg {
	MT6351_PMIC_RG_VCN18_ON_CTRL,
	MT6351_PMIC_RG_VCN28_ON_CTRL,
	MT6351_PMIC_RG_VCN33_ON_CTRL_BT,
	MT6351_PMIC_RG_VCN33_ON_CTRL_WIFI,
};

void pmic_set_register_value(int field, unsigned int val);

static inline void mt6351_upmu_set_rg_vcn33_on_ctrl(unsigned int val)
{
	/* Only referenced from the CONSYS_BT_WIFI_SHARE_V33 branch, which is
	 * 0 for mt6797; kept for compile completeness of that #if block. */
	pmic_set_register_value(MT6351_PMIC_RG_VCN33_ON_CTRL_BT, val);
}

#endif /* __SHIM_UPMU_COMMON_H__ */
