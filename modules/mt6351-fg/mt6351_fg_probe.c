// SPDX-License-Identifier: GPL-2.0
/*
 * mt6351_fg_probe — read the MT6351's HARDWARE fuel gauge, to find out whether
 * mainline can use it instead of guessing capacity from a voltage curve.
 *
 * WHY
 *
 * This device has no battery power_supply at all. The only entry is
 * bq25890-charger-0, a charger IC with no `capacity` node, so the panel
 * indicator currently estimates state-of-charge from terminal voltage. That is
 * coarse by construction: on the flat 3.7-3.8 V plateau a 20 mV difference maps
 * to ~18 percentage points, and six consecutive reads at one voltage produced
 * raw values of 23, 41, 23, 28, 28, 28.
 *
 * The vendor 3.18 kernel does something completely different. From
 * drivers/misc/mediatek/power/mt6797/battery_meter_hal.c, Android reads a real
 * hardware gauge inside the MT6351 PMIC:
 *
 *   BATTERY_METER_CMD_GET_HW_FG_CAR      35-bit Coulomb Accumulation Register
 *   BATTERY_METER_CMD_GET_HW_FG_CURRENT  measured current + sign
 *   BATTERY_METER_CMD_GET_HW_OCV         open-circuit voltage, sampled at boot
 *   BATTERY_METER_CMD_SET_COLUMB_INTERRUPT
 *
 * i.e. it integrates actual charge in and out, seeded by a true OCV reading.
 * That is a fuel gauge; the voltage curve is a guess.
 *
 * ACCESS PATH
 *
 * The MT6351 sits behind the SoC PMIC wrapper. Its DT children are never
 * created (the MFD is one in name only), so the way to reach it is the pwrap's
 * own regmap, two levels up -- exactly what modules/connectivity/shim/
 * mt6351_pmic.c already does for the VCN LDO bits. This module reuses that path
 * verbatim rather than inventing a second one.
 *
 * This is a PROBE, not a driver: it only reads, and only exposes what it read,
 * so the numbers can be sanity-checked against a known charge state before
 * anyone writes a power_supply driver around them.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/delay.h>
#include <linux/bits.h>

/* MT6351 FGADC block — addresses from the vendor upmu_hw.h (mt6797) */
#define FGADC_CON0		0x0CA4	/* control/enable */
#define FGADC_CON1		0x0CA6	/* FG_CAR[34:19] */
#define FGADC_CON2		0x0CA8	/* FG_CAR[18:03] */
#define FGADC_CON3		0x0CAA
#define FGADC_CON11		0x0CBA	/* FG_CURRENT_OUT */
#define FGADC_CON12		0x0CBC
#define FGADC_CON13		0x0CBE
#define FGADC_CON14		0x0CC0
#define FGADC_CON15		0x0CC2
#define FGADC_CON16		0x0CC4
#define FGADC_CON17		0x0CC6
#define FGADC_CON18		0x0CC8

/* vendor battery_meter_hal.c: UNIT_FGCURRENT 158122 -> 158.122 uA per LSB */
#define UNIT_FGCURRENT_NA	158122

static struct regmap *fg_regmap;
static struct kobject *fg_kobj;

static struct regmap *pwrap_regmap(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct regmap *rm;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np) {
		pr_err("mt6351-fg: no mt6797-pwrap node\n");
		return NULL;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		pr_err("mt6351-fg: pwrap node has no device\n");
		return NULL;
	}
	rm = dev_get_regmap(&pdev->dev, NULL);
	put_device(&pdev->dev);
	if (!rm)
		pr_err("mt6351-fg: pwrap exposes no regmap\n");
	return rm;
}

static int rd(unsigned int reg, unsigned int *val)
{
	return regmap_read(fg_regmap, reg, val);
}

/*
 * CAR IS LATCHED, NOT DIRECTLY READABLE — this is why a naive read returns 0.
 *
 * Vendor sequence (battery_meter_hal.c, fgauge_read_columb_internal):
 *   1. CON0[15:8] = 0x02      set FG_SW_READ_PRE (bit 9)
 *   2. poll LATCHDATA_ST (bit 10) until 1
 *   3. read FG_CAR_18_03 / FG_CAR_34_19
 *   4. CON0[15:8] = 0x08      set FG_SW_CLEAR (bit 11)
 *   5. poll LATCHDATA_ST until 0
 * The low byte (FG_ON | calibration) must be preserved throughout, which is why
 * every write below is masked to 0xFF00.
 */
#define CON0_SW_READ_PRE	BIT(9)
#define CON0_LATCHDATA_ST	BIT(10)
#define CON0_SW_CLEAR		BIT(11)

static int fg_latch_wait(bool want_set)
{
	unsigned int v;
	int i;

	for (i = 0; i < 1000; i++) {
		if (rd(FGADC_CON0, &v))
			return -EIO;
		if (!!(v & CON0_LATCHDATA_ST) == want_set)
			return 0;
		udelay(50);
	}
	return -ETIMEDOUT;
}

static int fg_read_car(s64 *out, unsigned int *raw_hi, unsigned int *raw_lo)
{
	unsigned int hi, lo;
	u32 car;
	int ret;

	/* 1. latch */
	ret = regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0200);
	if (ret)
		return ret;
	ret = fg_latch_wait(true);
	if (ret)
		pr_warn("mt6351-fg: latch never went ready (%d), reading anyway\n", ret);

	if (rd(FGADC_CON1, &hi) || rd(FGADC_CON2, &lo))
		return -EIO;

	/* 2. assemble exactly as the vendor does */
	car = (lo >> 11) | ((hi & 0x0FFF) << 5);
	*out = (hi & 0x8000) ? -(s64)car : (s64)car;
	*raw_hi = hi;
	*raw_lo = lo;

	/* 3. clear */
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0800);
	fg_latch_wait(false);
	/* 4. leave the read/clear bits low again */
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0000);
	return 0;
}

static ssize_t regs_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	unsigned int con0, cur, hi = 0, lo = 0;
	s64 car = 0;
	s32 cur_signed;
	int n = 0;

	if (rd(FGADC_CON0, &con0) || rd(FGADC_CON11, &cur))
		return -EIO;

	fg_read_car(&car, &hi, &lo);

	cur_signed = (cur & 0x8000) ? (s32)cur - 0x10000 : (s32)cur;

	n += sysfs_emit_at(buf, n, "CON0            0x%04x\n", con0);
	n += sysfs_emit_at(buf, n, "CAR_34_19       0x%04x\n", hi);
	n += sysfs_emit_at(buf, n, "CAR_18_03       0x%04x\n", lo);
	n += sysfs_emit_at(buf, n, "CAR             %lld\n", car);
	n += sysfs_emit_at(buf, n, "CURRENT_OUT     0x%04x (%d LSB)\n", cur, cur_signed);
	n += sysfs_emit_at(buf, n, "CURRENT_uA      %lld\n",
			   div_s64((s64)cur_signed * UNIT_FGCURRENT_NA, 1000));
	return n;
}

static struct kobj_attribute regs_attr = __ATTR_RO(regs);

static int __init mt6351_fg_init(void)
{
	unsigned int id;

	fg_regmap = pwrap_regmap();
	if (!fg_regmap)
		return -ENODEV;

	/* prove the regmap talks to the PMIC at all before trusting FG values */
	if (rd(0x0000, &id) == 0)
		pr_info("mt6351-fg: pmic reg 0x0000 = 0x%04x\n", id);

	fg_kobj = kobject_create_and_add("mt6351_fg", kernel_kobj);
	if (!fg_kobj)
		return -ENOMEM;
	if (sysfs_create_file(fg_kobj, &regs_attr.attr)) {
		kobject_put(fg_kobj);
		return -ENOMEM;
	}
	pr_info("mt6351-fg: probe ready at /sys/kernel/mt6351_fg/regs\n");
	return 0;
}

static void __exit mt6351_fg_exit(void)
{
	if (fg_kobj)
		kobject_put(fg_kobj);
}

module_init(mt6351_fg_init);
module_exit(mt6351_fg_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read the MT6351 hardware fuel gauge (probe)");
