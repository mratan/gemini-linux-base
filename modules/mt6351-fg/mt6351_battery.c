// SPDX-License-Identifier: GPL-2.0
/*
 * mt6351_battery — a real battery power_supply for the Gemini PDA, using the
 * MT6351 PMIC's hardware fuel gauge and the vendor's own battery profile.
 *
 * WHY THIS EXISTS
 *
 * This device has no battery power_supply at all. /sys/class/power_supply
 * contains exactly one entry, bq25890-charger-0 -- a charger IC with no
 * `capacity`, `charge_now` or `*_full` node -- so sfwbar's stock battery widget
 * can never find anything and upower is not installed for the same reason. The
 * panel indicator was therefore estimating state-of-charge from terminal
 * voltage alone, which is coarse by construction: on the flat 3.7-3.8 V plateau
 * a 20 mV difference maps to ~18 percentage points, and six consecutive reads
 * at one voltage produced 23, 41, 23, 28, 28, 28. Worse, terminal voltage sags
 * under load, so the panel read 100% on the charger and 4% ten minutes later.
 *
 * WHAT ANDROID DOES, AND WHAT THIS COPIES
 *
 * The vendor 3.18 tree (drivers/misc/mediatek/power/mt6797/battery_meter_hal.c)
 * does not guess. It reads a hardware gauge inside the MT6351:
 *
 *   GET_HW_FG_CAR       35-bit Coulomb Accumulation Register
 *   GET_HW_FG_CURRENT   measured current across a 10 mOhm sense resistor
 *   GET_HW_OCV          open-circuit voltage latched at wakeup
 *
 * and maps voltage to charge through a per-battery 82-point profile at four
 * temperatures, with a matching internal-resistance profile.
 *
 * This driver takes the same two inputs the vendor's software-OCV path uses --
 * measured current and terminal voltage -- and the vendor's own tables:
 *
 *   OCV = V_terminal + I * R_internal
 *
 * with I from the hardware gauge (negative while charging) and R interpolated
 * from r_profile_t2. That removes the load-sag error that makes a bare voltage
 * reading useless, which is the entire complaint. State of charge then comes
 * from battery_profile_t2 as 100 - depth-of-discharge.
 *
 * WHAT IT DOES NOT DO YET
 *
 * It does not integrate the CAR. Coulomb counting needs a trustworthy starting
 * point and a reset policy, and the compensated-OCV path above is already a
 * large improvement and is self-correcting. CAR is read and exported as
 * CHARGE_COUNTER so the next step has a measured baseline to build on.
 *
 * ACCESS PATH
 *
 * The MT6351's DT children are never created -- the MFD is one in name only --
 * so registers are reached through the pwrap's own regmap, two levels up. That
 * is the same path modules/connectivity/shim/mt6351_pmic.c already uses, reused
 * here rather than invented twice.
 *
 * It is named BAT0 deliberately: sfwbar's stock battery.source scans
 * /sys/class/power_supply for a directory whose name starts with "BAT".
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/bits.h>

#include "mt6351_battery_tables.h"

/* MT6351 FGADC block (vendor upmu_hw.h, mt6797) */
#define FGADC_CON0		0x0CA4
#define FGADC_CON1		0x0CA6	/* FG_CAR[34:19] */
#define FGADC_CON2		0x0CA8	/* FG_CAR[18:03] */
#define FGADC_CON11		0x0CBA	/* FG_CURRENT_OUT */

#define CON0_SW_READ_PRE	BIT(9)
#define CON0_LATCHDATA_ST	BIT(10)
#define CON0_SW_CLEAR		BIT(11)

/* battery_meter_hal.c: UNIT_FGCURRENT 158122 -> 158.122 uA per LSB */
#define UNIT_FGCURRENT_NA	158122
/* cust battery profile */
#define Q_MAX_MAH		4268

static struct regmap *fg_regmap;
static struct power_supply *bat_psy;
static struct power_supply *chg_psy;

static struct regmap *pwrap_regmap(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct regmap *rm;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	rm = dev_get_regmap(&pdev->dev, NULL);
	put_device(&pdev->dev);
	return rm;
}

/* ---- hardware gauge reads ------------------------------------------------ */

static int fg_current_ua(int *out)
{
	unsigned int v;
	s32 lsb;

	if (regmap_read(fg_regmap, FGADC_CON11, &v))
		return -EIO;
	lsb = (v & 0x8000) ? (s32)v - 0x10000 : (s32)v;
	/* negative = charging, matching the vendor's decode */
	*out = (int)div_s64((s64)lsb * UNIT_FGCURRENT_NA, 1000);
	return 0;
}

static int fg_latch_wait(bool want_set)
{
	unsigned int v;
	int i;

	for (i = 0; i < 1000; i++) {
		if (regmap_read(fg_regmap, FGADC_CON0, &v))
			return -EIO;
		if (!!(v & CON0_LATCHDATA_ST) == want_set)
			return 0;
		udelay(50);
	}
	return -ETIMEDOUT;
}

/* CAR is latched, not directly readable: a naive read returns 0. */
static int fg_car_raw(s64 *out)
{
	unsigned int hi, lo;
	u32 car;

	if (regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0200))
		return -EIO;
	fg_latch_wait(true);
	if (regmap_read(fg_regmap, FGADC_CON1, &hi) ||
	    regmap_read(fg_regmap, FGADC_CON2, &lo))
		return -EIO;
	car = (lo >> 11) | ((hi & 0x0FFF) << 5);
	*out = (hi & 0x8000) ? -(s64)car : (s64)car;
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0800);
	fg_latch_wait(false);
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0000);
	return 0;
}

/* ---- vendor profile interpolation --------------------------------------- */

/* internal resistance at a given OCV, vendor units are 0.1 mOhm */
static int res_at_mv(int mv)
{
	int i;

	if (mv >= res_table[0].ocv_mv)
		return res_table[0].res;
	for (i = 1; i < ARRAY_SIZE(res_table); i++) {
		if (mv >= res_table[i].ocv_mv) {
			int v0 = res_table[i - 1].ocv_mv, v1 = res_table[i].ocv_mv;
			int r0 = res_table[i - 1].res,    r1 = res_table[i].res;
			if (v0 == v1)
				return r1;
			return r1 + (r0 - r1) * (mv - v1) / (v0 - v1);
		}
	}
	return res_table[ARRAY_SIZE(res_table) - 1].res;
}

/* depth-of-discharge (percent of Q_MAX) for an OCV */
static int dod_at_mv(int mv)
{
	int i;

	if (mv >= ocv_table[0].ocv_mv)
		return ocv_table[0].dod;
	for (i = 1; i < ARRAY_SIZE(ocv_table); i++) {
		if (mv >= ocv_table[i].ocv_mv) {
			int v0 = ocv_table[i - 1].ocv_mv, v1 = ocv_table[i].ocv_mv;
			int d0 = ocv_table[i - 1].dod,    d1 = ocv_table[i].dod;
			if (v0 == v1)
				return d1;
			return d1 + (d0 - d1) * (mv - v1) / (v0 - v1);
		}
	}
	return ocv_table[ARRAY_SIZE(ocv_table) - 1].dod;
}

static int charger_prop(enum power_supply_property psp, int *val)
{
	union power_supply_propval v;
	int ret;

	if (!chg_psy)
		chg_psy = power_supply_get_by_name("bq25890-charger-0");
	if (!chg_psy)
		return -ENODEV;
	ret = power_supply_get_property(chg_psy, psp, &v);
	if (ret)
		return ret;
	*val = v.intval;
	return 0;
}

/* Compensated state of charge, the vendor's software-OCV method. */
static int compute_capacity(int *soc, int *ocv_mv_out, int *cur_ua_out)
{
	int vbat_uv, cur_ua = 0, ocv_mv, r, dod, i;

	if (charger_prop(POWER_SUPPLY_PROP_VOLTAGE_NOW, &vbat_uv))
		return -EIO;
	fg_current_ua(&cur_ua);

	ocv_mv = vbat_uv / 1000;
	/*
	 * Two passes: R depends on OCV and OCV depends on R. The curve is
	 * shallow so this converges immediately; the vendor iterates likewise.
	 */
	for (i = 0; i < 2; i++) {
		r = res_at_mv(ocv_mv);			/* 0.1 mOhm */
		/* I is uA, r is 0.1 mOhm -> uA * 0.1 mOhm = 0.1 nV; /10^7 -> mV */
		ocv_mv = vbat_uv / 1000 + (int)div_s64((s64)cur_ua * r, 10000000);
	}

	dod = dod_at_mv(ocv_mv);
	*soc = clamp(100 - dod, 0, 100);
	*ocv_mv_out = ocv_mv;
	*cur_ua_out = cur_ua;
	return 0;
}

/* ---- power_supply -------------------------------------------------------- */

static enum power_supply_property bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static int bat_get_prop(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	int soc = 0, ocv = 0, cur = 0, tmp;
	s64 car = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (charger_prop(POWER_SUPPLY_PROP_STATUS, &tmp))
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else
			val->intval = tmp;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (compute_capacity(&soc, &ocv, &cur))
			return -EIO;
		val->intval = soc;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return charger_prop(POWER_SUPPLY_PROP_VOLTAGE_NOW, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		if (compute_capacity(&soc, &ocv, &cur))
			return -EIO;
		val->intval = ocv * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (fg_current_ua(&cur))
			return -EIO;
		/* power_supply convention: positive = charging */
		val->intval = -cur;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = Q_MAX_MAH * 1000;		/* uAh */
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (compute_capacity(&soc, &ocv, &cur))
			return -EIO;
		val->intval = Q_MAX_MAH * 1000 / 100 * soc;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		fg_car_raw(&car);
		val->intval = (int)car;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc bat_desc = {
	.name		= "BAT0",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= bat_props,
	.num_properties	= ARRAY_SIZE(bat_props),
	.get_property	= bat_get_prop,
};

static struct platform_device *pdev_self;

static int __init mt6351_bat_init(void)
{
	struct power_supply_config cfg = {};
	int soc = 0, ocv = 0, cur = 0;

	fg_regmap = pwrap_regmap();
	if (!fg_regmap) {
		pr_err("mt6351-battery: no pwrap regmap\n");
		return -ENODEV;
	}

	pdev_self = platform_device_register_simple("mt6351-battery", -1, NULL, 0);
	if (IS_ERR(pdev_self))
		return PTR_ERR(pdev_self);

	bat_psy = power_supply_register(&pdev_self->dev, &bat_desc, &cfg);
	if (IS_ERR(bat_psy)) {
		platform_device_unregister(pdev_self);
		return PTR_ERR(bat_psy);
	}

	if (!compute_capacity(&soc, &ocv, &cur))
		pr_info("mt6351-battery: BAT0 ready — soc=%d%% ocv=%dmV current=%duA\n",
			soc, ocv, cur);
	else
		pr_info("mt6351-battery: BAT0 ready (no reading yet)\n");
	return 0;
}

static void __exit mt6351_bat_exit(void)
{
	if (bat_psy)
		power_supply_unregister(bat_psy);
	if (chg_psy)
		power_supply_put(chg_psy);
	if (pdev_self)
		platform_device_unregister(pdev_self);
}

module_init(mt6351_bat_init);
module_exit(mt6351_bat_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6351 hardware fuel gauge battery (Gemini PDA)");
