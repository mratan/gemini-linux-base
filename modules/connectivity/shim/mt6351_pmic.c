// SPDX-License-Identifier: GPL-2.0
/* Real MT6351 PMIC bit access for the vendored CONSYS stack (replaces the
 * former pr_warn_once stubs — API-CHURN-LOG.md N2, now WIRED).
 *
 * The only pmic_set_register_value() call live on the non-legacy vendor
 * path is consys_vcn28_hw_mode_ctrl(): RG_VCN28_ON_CTRL, the VCN28
 * HW-control-mode (SRCLKEN) switch written before/after VCN28 enable when
 * co_clock_flag=0 (this device's WMT_SOC.cfg) — the bit on B-21's
 * not-yet-eliminated list. The remaining table entries (VCN18,
 * VCN33_BT/WIFI ON_CTRL) are only reachable from the CONFIG_MTK_PMIC_LEGACY
 * branches, which this port does not compile; they are wired anyway so the
 * table is complete if those paths are ever revived.
 *
 * Access path: the MT6351 sits behind the SoC PMIC wrapper; the mainline
 * pwrap driver (mediatek,mt6797-pwrap) exposes a 16-bit regmap on its
 * device — the same regmap the Base tree's mt6351 VCN regulator driver
 * uses (patches/v6.6/regulator/0002, "dev_get_regmap(pdev->dev.parent)").
 * Register addr/mask/shift values are verbatim from the vendor
 * mt6797/include/mach/upmu_hw.h (UBports halium-9.0 @ 28ffb22d):
 *   RG_VCN18_ON_CTRL       LDO_VCN18_CON0  0x0A52 bit 3
 *   RG_VCN28_ON_CTRL       LDO_VCN28_CON0  0x0A0C bit 3
 *   RG_VCN33_ON_CTRL_BT    LDO_VCN33_CON3  0x0A98 bit 3
 *   RG_VCN33_ON_CTRL_WIFI  LDO_VCN33_CON4  0x0A9A bit 3
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <upmu_common.h>

struct shim_pmic_bit {
	unsigned int reg;
	unsigned int mask;
	unsigned int shift;
};

static const struct shim_pmic_bit shim_pmic_bits[] = {
	[MT6351_PMIC_RG_VCN18_ON_CTRL]      = { .reg = 0x0A52, .mask = 0x1, .shift = 3 },
	[MT6351_PMIC_RG_VCN28_ON_CTRL]      = { .reg = 0x0A0C, .mask = 0x1, .shift = 3 },
	[MT6351_PMIC_RG_VCN33_ON_CTRL_BT]   = { .reg = 0x0A98, .mask = 0x1, .shift = 3 },
	[MT6351_PMIC_RG_VCN33_ON_CTRL_WIFI] = { .reg = 0x0A9A, .mask = 0x1, .shift = 3 },
};

static struct regmap *shim_pwrap_regmap(void)
{
	static struct regmap *cached;
	struct device_node *np;
	struct platform_device *pdev;

	if (cached)
		return cached;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np) {
		pr_err_once("consys-shim: no mt6797-pwrap node — PMIC ON_CTRL writes unavailable\n");
		return NULL;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		pr_err_once("consys-shim: pwrap node has no device (driver not probed yet?)\n");
		return NULL;
	}
	cached = dev_get_regmap(&pdev->dev, NULL);
	put_device(&pdev->dev);
	if (!cached)
		pr_err_once("consys-shim: pwrap device exposes no regmap\n");
	return cached;
}

void pmic_set_register_value(int field, unsigned int val)
{
	const struct shim_pmic_bit *bit;
	struct regmap *regmap;
	int ret;

	if (field < 0 || field >= ARRAY_SIZE(shim_pmic_bits) ||
	    !shim_pmic_bits[field].mask) {
		WARN_ONCE(1, "consys-shim: unknown PMIC field %d", field);
		return;
	}
	bit = &shim_pmic_bits[field];

	regmap = shim_pwrap_regmap();
	if (!regmap) {
		pr_err("consys-shim: PMIC write dropped (field %d val %u) — no pwrap regmap\n",
		       field, val);
		return;
	}

	ret = regmap_update_bits(regmap, bit->reg, bit->mask << bit->shift,
				 (val & bit->mask) << bit->shift);
	if (ret)
		pr_err("consys-shim: PMIC write 0x%04x[%u]=%u failed (%d)\n",
		       bit->reg, bit->shift, val, ret);
	else
		pr_info("consys-shim: PMIC 0x%04x[%u]=%u\n",
			bit->reg, bit->shift, val & bit->mask);
}
