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
 * WHAT ANDROID DOES, AND WHERE THIS NOW GOES FURTHER
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
 * The first version of this driver reproduced only the software-OCV half of
 * that -- OCV = V_terminal + I * R_internal, then depth-of-discharge from the
 * profile -- and did so with four defects that between them meant it was not
 * actually doing it at all. Each is documented at the code that fixes it:
 *
 *   1. FG_CURRENT_OUT was read without the latch sequence the vendor wraps it
 *      in, so it never updated and I was 0. With I pinned at 0, OCV = V and
 *      the driver was the bare voltage lookup it exists to replace.
 *   2. the current register is sign-magnitude, not two's complement, so the
 *      sign of the load compensation was inverted -- which would have made
 *      things worse than no compensation the moment (1) was fixed.
 *   3. terminal voltage came from the bq25890 charger's ADC, which steps in
 *      20 mV. On this pack's plateau one LSB is about two percentage points,
 *      and the reported capacity visibly flickered 79, 81, 79, 81 with the
 *      battery doing nothing.
 *   4. neither board calibration was applied: R_FG_VALUE (10 mOhm against a
 *      20 mOhm base) and CAR_TUNE_VALUE (115). Together a factor of 2.3, so
 *      every current and every coulomb count was low by more than half.
 *
 * With those fixed this matches the vendor's method. It then goes past it in
 * three places, each because the vendor's choice was measurably worse on this
 * board:
 *
 *   * COULOMB COUNTING. The vendor's software-OCV path reports whatever the
 *     terminal voltage implies right now. This device lives on a charger, and
 *     under load its pack voltage wanders 150 mV as the charger disengages and
 *     re-engages -- measured, four cores of load: 4.174 -> 4.025 V and back,
 *     on which the OCV-only gauge read 87, 70, 87%. The CAR is integrated in
 *     hardware and moved smoothly through the same event. So the counter
 *     carries the motion and the profile supplies the absolute reference,
 *     combined in a complementary filter whose time constant depends on how
 *     much the OCV estimate is currently worth.
 *   * MEDIAN CURRENT. The vendor takes one instantaneous sample of
 *     FG_CURRENT_OUT and multiplies it by the internal resistance. On this
 *     board that sample swings +248, +267, +103, +94, -55, +114 mA under a
 *     load that is not changing -- switching-charger ripple, straight into the
 *     compensation term. A median of nine removes it.
 *   * BATSNS, AVERAGED. Same channel and same scaling the vendor uses
 *     (15 bits, 1800 mV, 1:3 divider = 0.165 mV per LSB) but averaged over
 *     eight conversions, which costs nothing and is what defeats (3) above.
 *
 * WHAT IT STILL DOES NOT DO, AND WHY
 *
 * ONE TEMPERATURE PROFILE. The vendor carries four (T0 -10 C, T1 0 C, T2 25 C,
 * T3 50 C) and interpolates between them; this uses T2 alone. The gap is small
 * and it is measurable: at the same depth of discharge the T2 and T3 tables sit
 * about 12 mV apart, which on this pack's plateau is roughly one percentage
 * point, and the device runs near 38 C -- so about half a point of error.
 *
 * It is left undone deliberately rather than approximated, because the only
 * temperature currently available is the bq25890's, and that driver's own
 * comment for it reads "convert TS percentage into rough temperature". Feeding
 * a rough number into a table interpolation manufactures precision that is not
 * there, and can easily land further from the truth than simply using T2.
 *
 * Doing it properly means what the vendor does: BATON on the PMIC AUXADC
 * (channel 3, r_val 2, 12-bit over 1800 mV, gated by PMIC_BATON_TDET_EN),
 * converted to a resistance through the pull-up network and then to degrees
 * through the vendor's NTC table -- and only then interpolating between two
 * 82-point profiles. That is the shape of the work; it is worth about one
 * percentage point.
 *
 * IT DOES NOT FORCE 100% WHEN THE CHARGER SAYS FULL, also deliberately.
 * Mainline leaves the BQ25896 at its 4.208 V constant-voltage default where the
 * vendor sets 4.352 V for this 4.35 V cell, so charging terminates with the
 * pack resting near 4.18 V -- genuinely about 85% full, which is what this
 * reports. Snapping to 100% at termination is the conventional lie and it would
 * hide an open hardware question behind a satisfying number.
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
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/sort.h>

#include "mt6351_battery_tables.h"

/* MT6351 FGADC block (vendor upmu_hw.h, mt6797) */
#define FGADC_CON0		0x0CA4
#define FGADC_CON1		0x0CA6	/* FG_CAR[34:19] */
#define FGADC_CON2		0x0CA8	/* FG_CAR[18:03] */
#define FGADC_CON11		0x0CBA	/* FG_CURRENT_OUT */

#define CON0_SW_READ_PRE	BIT(9)
#define CON0_LATCHDATA_ST	BIT(10)
#define CON0_SW_CLEAR		BIT(11)

/* MT6351 AUXADC. Channel 0 is BATSNS -- the battery sense line, which is what
 * the vendor's read_adc_v_bat_sense() asks for via
 * PMIC_IMM_GetOneChannelValue(PMIC_AUX_BATSNS_AP, times, 1).
 */
#define AUXADC_ADC23		0x0E2E	/* [14:0] CH0 value, [15] CH0 ready */
#define AUXADC_RQST0_SET	0x0E98	/* write-1-to-set; bit 0 = request CH0 */
#define AUXADC_RDY_CH0		BIT(15)
#define AUXADC_VAL_MASK		0x7FFF
/* pmic_auxadc.c: 15-bit converter, 1800 mV full scale, 1:3 divider on BATSNS */
#define AUXADC_FULL_RANGE_MV	1800
#define AUXADC_PRECISE		32768
#define AUXADC_BAT_DIV		3
/* The vendor averages VBAT (FG_VBAT_AVERAGE_SIZE 18). Eight is plenty here:
 * one LSB is 3*1800/32768 = 0.165 mV, so the quantisation this replaces is
 * already two orders of magnitude away.
 */
#define VBAT_SAMPLES		8
/*
 * FG_CURRENT_OUT is an INSTANTANEOUS sample, and on this board it is savage.
 * Logged at 2 s intervals under a steady two-core load it read, in sequence:
 * +248, +267, +103, +94, -55, +114, +73, +49, +69, +85 mA -- swinging sign,
 * on a load that did not change. That is the switching charger's ripple, and
 * the vendor's software-OCV path takes ONE such sample and multiplies it by
 * the internal resistance, so its load compensation inherits the whole of it.
 *
 * A median of nine kills the spikes without the lag of a mean, and it is
 * cheap: the samples come from nine latch cycles a few hundred microseconds
 * apart, all inside the one-second cache window.
 */
#define CUR_SAMPLES		9

/* battery_meter_hal.c: UNIT_FGCURRENT 158122 -> 158.122 uA per LSB */
#define UNIT_FGCURRENT_NA	158122
/*
 * Board calibration, from mt6797 mt_battery_meter.h. NEITHER OF THESE WAS
 * APPLIED, and together they are a factor of 2.3 -- so every current and every
 * coulomb count this driver has ever reported was low by more than half.
 *
 *   R_FG_VALUE 10       the sense resistor is 10 mOhm and UNIT_FGCURRENT is
 *                       calibrated against a 20 mOhm base, so half the volt
 *                       drop for the same current: multiply by 20/10.
 *   CAR_TUNE_VALUE 115  the board's own gauge trim, 1.15. (101 in the tree is
 *                       the MT6353 variant; this device is MT6351.)
 *
 * R_FG_BOARD_SLOPE == R_FG_BOARD_BASE == 1000 on this board, so the vendor's
 * K-current step is the identity and is not reproduced here.
 */
#define R_FG_VALUE		10
#define R_FG_BASE		20
#define CAR_TUNE_VALUE		115
/* CAR LSB is 359.86 uAh before the FG_OSR=8 divider (battery_meter_hal.c) */
#define CAR_LSB_UAH_X100	35986
#define CAR_OSR			8
#define CAR_RAW_MAX		0x1FFFF
/* cust battery profile */
#define Q_MAX_MAH		4268
#define Q_MAX_UAH		((s64)Q_MAX_MAH * 1000)

/*
 * Complementary-filter time constants, in seconds.
 *
 * The charge estimate is carried by the coulomb counter and pulled towards the
 * OCV estimate at a rate that depends on how much the OCV estimate is worth:
 *
 *   TAU_REST  nothing is flowing, so terminal voltage IS open-circuit voltage
 *             and the profile lookup is trustworthy. Converge in ~2 minutes.
 *             Note this includes a charger that has terminated: "at rest" is a
 *             statement about current, not about the cable.
 * There is deliberately no busy-state time constant. Charging holds terminal
 * voltage up on the constant-voltage plateau where it says nothing about
 * charge, and load drags it down by an IR term this pack's profile gets badly
 * wrong (312 mOhm measured against ~20 in the vendor table). In both cases the
 * honest weight to give the profile is zero.
 *
 * Measured on this device 2026-08-27, four cores of load applied at t=69 s:
 * terminal voltage went 4.174 -> 4.025 V and back, wandering 150 mV, while the
 * charger re-engaged and disengaged. A pure OCV gauge reported 87, 70, 87% on
 * that. The coulomb counter moved smoothly and monotonically throughout.
 */
#define TAU_REST		120
#define REST_CURRENT_UA		30000
/*
 * A single update may never close more than half the gap to the OCV estimate,
 * however long it has been since the last one. Without this, an update after a
 * long idle period clamps dt to tau and the correction term becomes the whole
 * gap -- the estimate snaps to whatever the profile said at that instant, which
 * is exactly the behaviour coulomb counting is here to avoid.
 */
#define MAX_CORRECTION_FRAC	2
/*
 * And the REPORTED percentage moves at most one point per this many seconds,
 * whatever the estimate underneath does. Measured on 2026-08-27, before this
 * was added: a deep load transient walked the reported value 77 -> 59 -> 89 in
 * under a minute. Every one of those numbers had a defensible derivation and
 * the sequence was still useless -- a battery indicator that jumps thirty
 * points is not reporting charge, it is reporting weather. Real packs move at
 * a few percent per minute at most, so anything faster is the estimator
 * settling and should be spent smoothly rather than shown.
 */
#define SOC_SLEW_SECONDS	20

static struct regmap *fg_regmap;
static struct power_supply *bat_psy;
static struct power_supply *chg_psy;

/* The FGADC latch is a read-modify-write sequence on a single register and is
 * emphatically not reentrant; the vendor wraps it in fgadc_hal_lock().
 */
static DEFINE_MUTEX(fg_lock);

/* Raw-register tracing, for settling questions the decoded values cannot
 * answer -- e.g. whether the CAR direction bit is actually being driven.
 * echo 1 > /sys/module/mt6351_battery/parameters/fg_debug
 */
static bool fg_debug;
module_param(fg_debug, bool, 0644);
MODULE_PARM_DESC(fg_debug, "log raw FGADC registers on every snapshot");

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

/*
 * THE FGADC OUTPUT REGISTERS DO NOT UPDATE UNLESS YOU LATCH THEM.
 *
 * This is the defect that mattered. fg_current_ua() used to be a bare
 * regmap_read() of FGADC_CON11, with none of the sequence around it, so
 * FG_CURRENT_OUT was never refreshed and the read came back 0 essentially
 * always. Everything downstream inherited that: OCV = V + I*R with I == 0 is
 * just V, so this driver has been a plain voltage lookup -- precisely the thing
 * its own header says it exists to replace -- since the day it was written.
 * Six consecutive samples on 2026-08-27 read 0, 0, 20, 0, 0, 0 mA on a machine
 * that was drawing real current.
 *
 * The vendor sequence (battery_meter_hal.c, fgauge_read_current and
 * fgauge_read_columb_internal, identical in both):
 *
 *   1. CON0[15:8] = 0x02      set SW_READ_PRE
 *   2. wait for LATCHDATA_ST to go 1
 *   3. read the output registers
 *   4. CON0[15:8] = 0x08      clear the status
 *   5. wait for LATCHDATA_ST to go 0
 *   6. CON0[15:8] = 0x00      restore
 *
 * Current and CAR are latched by the SAME sequence, so this takes ONE snapshot
 * of both. That is a small improvement on the vendor, which latches twice and
 * therefore samples current and charge a few milliseconds apart.
 */
struct fg_snapshot {
	int	cur_ua;		/* + discharging, - charging */
	s64	car_uah;	/* + net charge in, - net charge out */
	bool	valid;
};

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

/*
 * The raw current register is SIGN-MAGNITUDE-ish, not two's complement, and
 * getting that wrong inverts the sign of the load compensation.
 *
 * The vendor decode is: 0 means zero; a raw value above 32767 means DIScharging
 * with magnitude 65535 - raw; anything else means CHARGING with magnitude raw.
 * Read as two's complement instead -- which is what this driver used to do --
 * a discharge of 16 LSBs reads as -16 and is taken for a charge. OCV = V + I*R
 * then SUBTRACTS the load drop instead of adding it, pushing the reported
 * charge the wrong way by twice the error. It never showed because I was
 * pinned at 0 by the missing latch above; it would have appeared the moment
 * that was fixed.
 */
static int fg_decode_current(unsigned int raw)
{
	bool discharging;
	s64 ua;
	u32 mag;

	if (raw == 0)
		return 0;
	if (raw > 32767) {
		discharging = true;
		mag = 65535 - raw;
	} else {
		discharging = false;
		mag = raw;
	}

	ua = (s64)mag * UNIT_FGCURRENT_NA;
	do_div(ua, 1000);				/* -> uA */
	ua = ua * R_FG_BASE;
	do_div(ua, R_FG_VALUE);				/* sense resistor */
	ua = ua * CAR_TUNE_VALUE;
	do_div(ua, 100);				/* board trim */

	return discharging ? (int)ua : -(int)ua;
}

/*
 * CAR is a free-running 35-bit accumulator; only bits [34:3] are exposed, as a
 * 17-bit magnitude plus a direction bit.
 *
 * Two vendor details this driver did not have, both of which made the exported
 * CHARGE_COUNTER meaningless:
 *
 *   * 0x1FFFF is a SENTINEL for "no reading", not a value. So is 0. The device
 *     was reporting a constant -131060 from a raw of 0x1FFF4, which sits one
 *     LSB shy of the sentinel -- a register that is not being driven.
 *   * the discharge magnitude is 0x1FFFF - raw, not raw. Decoding it as raw
 *     turns a value near the rail into an enormous negative number instead of
 *     the small one it is, which is exactly what was observed.
 *
 * The result is scaled to real uAh here rather than left in LSBs, because
 * POWER_SUPPLY_PROP_CHARGE_COUNTER is defined in uAh and anything else is a
 * number that looks like an answer and is not one.
 */
static s64 fg_decode_car(unsigned int hi, unsigned int lo, bool *valid)
{
	u32 raw = (lo >> 11) | ((hi & 0x0FFF) << 5);
	bool discharging = !!(hi & 0x8000);
	s64 uah;
	u32 mag;

	*valid = true;
	if (raw == 0 || raw == CAR_RAW_MAX) {
		/* Not an error -- the vendor treats both as a legitimate zero. */
		return 0;
	}
	mag = discharging ? (CAR_RAW_MAX - raw) : raw;

	uah = (s64)mag * CAR_LSB_UAH_X100;
	do_div(uah, 100);				/* LSB 359.86 uAh */
	do_div(uah, CAR_OSR);				/* FG_OSR = 8 */
	uah = uah * R_FG_BASE;
	do_div(uah, R_FG_VALUE);
	uah = uah * CAR_TUNE_VALUE;
	do_div(uah, 100);

	return discharging ? -uah : uah;
}

static int fg_read_snapshot(struct fg_snapshot *s)
{
	unsigned int cur_raw, car_hi, car_lo;
	int ret;

	s->valid = false;
	mutex_lock(&fg_lock);

	ret = regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0200);
	if (ret)
		goto out;
	ret = fg_latch_wait(true);
	if (ret)
		goto restore;

	ret = regmap_read(fg_regmap, FGADC_CON11, &cur_raw);
	if (!ret)
		ret = regmap_read(fg_regmap, FGADC_CON1, &car_hi);
	if (!ret)
		ret = regmap_read(fg_regmap, FGADC_CON2, &car_lo);

restore:
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0800);
	fg_latch_wait(false);
	regmap_update_bits(fg_regmap, FGADC_CON0, 0xFF00, 0x0000);
out:
	mutex_unlock(&fg_lock);
	if (ret)
		return ret;

	if (fg_debug)
		pr_info("mt6351-battery: raw CON11=%04x CON1=%04x CON2=%04x (car17=%05x dir=%d)\n",
			cur_raw & 0xFFFF, car_hi, car_lo,
			(car_lo >> 11) | ((car_hi & 0x0FFF) << 5),
			!!(car_hi & 0x8000));

	s->cur_ua = fg_decode_current(cur_raw & 0xFFFF);
	s->car_uah = fg_decode_car(car_hi, car_lo, &s->valid);
	s->valid = true;
	return 0;
}

/*
 * Battery voltage from the PMIC's own AUXADC, not from the charger.
 *
 * VOLTAGE_NOW used to be read straight off bq25890-charger-0, whose ADC steps
 * in 20 mV. On the part of the discharge curve this pack sits on, 20 mV is
 * about two percentage points of charge -- so the reported capacity flickered
 * 79, 81, 79, 81 with the battery doing nothing at all, purely from one ADC
 * LSB. That is not a smoothing problem, it is the wrong instrument.
 *
 * The vendor reads BATSNS on the PMIC AUXADC instead: 15 bits over 1800 mV
 * through a 1:3 divider, so 0.165 mV per LSB. Two orders of magnitude finer,
 * and it is the line the battery profile was characterised against.
 */
static int auxadc_read_ch0(unsigned int *out)
{
	unsigned int v;
	int i, ret;

	ret = regmap_write(fg_regmap, AUXADC_RQST0_SET, 0x1);
	if (ret)
		return ret;

	for (i = 0; i < 200; i++) {
		ret = regmap_read(fg_regmap, AUXADC_ADC23, &v);
		if (ret)
			return ret;
		if (v & AUXADC_RDY_CH0) {
			*out = v & AUXADC_VAL_MASK;
			return 0;
		}
		udelay(100);
	}
	return -ETIMEDOUT;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* Median of CUR_SAMPLES latched current reads; also returns the last CAR. */
static int fg_current_median(int *out, s64 *car_uah, bool *car_valid)
{
	struct fg_snapshot s;
	int v[CUR_SAMPLES];
	int i, n = 0;

	for (i = 0; i < CUR_SAMPLES; i++) {
		if (fg_read_snapshot(&s))
			continue;
		v[n++] = s.cur_ua;
		*car_uah = s.car_uah;
		*car_valid = s.valid;
	}
	if (!n)
		return -EIO;
	sort(v, n, sizeof(v[0]), cmp_int, NULL);
	*out = v[n / 2];
	return 0;
}

static int auxadc_vbat_uv(int *uv)
{
	u64 acc = 0;
	unsigned int raw;
	int i, n = 0, ret;

	for (i = 0; i < VBAT_SAMPLES; i++) {
		ret = auxadc_read_ch0(&raw);
		if (ret)
			continue;
		acc += raw;
		n++;
	}
	if (!n)
		return -EIO;

	/* uV = raw * div * full_range_mV * 1000 / 2^15 */
	acc = div_u64(acc, n);
	acc = acc * AUXADC_BAT_DIV * AUXADC_FULL_RANGE_MV * 1000;
	*uv = (int)div_u64(acc, AUXADC_PRECISE);
	return 0;
}

/* ---- vendor profile interpolation --------------------------------------- */

/* internal resistance at a given OCV, vendor units are 0.1 mOhm */
/*
 * THE VENDOR RESISTANCE PROFILE IS LOW BY A FACTOR OF ~16, AND EVERYTHING
 * DERIVED FROM OCV INHERITS THAT ERROR.
 *
 * res_table says 17-25 mOhm across the whole pack. Measured on this cell on
 * 2026-08-27 across 105 samples of a real discharge: 660 mA at 3867 mV against
 * 1221 mA at 3691 mV, i.e. dV/dI = 312 mOhm. 312 / 19.5 = 16.
 *
 * The consequence is not subtle, because OCV = V + I*R and this machine seeds
 * its coulomb counter from an OCV lookup taken during the boot storm at over an
 * amp. Replaying three real seeds through these tables (as-shipped vs scaled),
 * against a charge known from the anchored counter:
 *
 *   vbat   I       true   as-shipped   x16
 *   3683   1441mA   82%      14%       77%
 *   3896    863mA   89%      57%       84%
 *   4070    101mA   89%      76%       80%
 *
 * The as-shipped column reproduces what the driver actually printed on those
 * boots (13%, 56%, 75%), so the replay is faithful and this is the real cause
 * of a gauge that comes up 70 points low after a reboot on battery.
 *
 * WHY 16 AND NOT 18. Fitting the scale to those three points prefers 18 (worst
 * error 9 pp against 16's 9 pp, mean 4.0 against 6.3). 16 is shipped anyway,
 * for two reasons: it is what the 105-sample measurement gives, where 18 is
 * fitted to three points; and 16's residuals are -5/-5/-9, all PESSIMISTIC,
 * while 18 turns one optimistic. A fuel gauge that reads low is a nuisance; one
 * that reads high strands you.
 *
 * WHAT THIS DOES NOT FIX. The third row above is -9 pp at every scale >= 16: at
 * 101 mA the I*R term is tiny whatever R is, so that residual is the OCV table
 * or surface charge, not resistance. Anyone chasing the last few points should
 * start there, not here.
 *
 * A FLAT SCALE IS THE MINIMUM-ASSUMPTION CHOICE. Real R varies with charge and
 * temperature and the vendor's CURVE SHAPE may be wrong too, but three points
 * cannot justify re-shaping it. Scaling preserves the shape and fixes the
 * magnitude, which is the part that is demonstrably wrong.
 */
#define R_PROFILE_SCALE 16

static int res_at_mv(int mv)
{
	int i;

	if (mv >= res_table[0].ocv_mv)
		return res_table[0].res * R_PROFILE_SCALE;
	for (i = 1; i < ARRAY_SIZE(res_table); i++) {
		if (mv >= res_table[i].ocv_mv) {
			int v0 = res_table[i - 1].ocv_mv, v1 = res_table[i].ocv_mv;
			int r0 = res_table[i - 1].res,    r1 = res_table[i].res;
			if (v0 == v1)
				return r1 * R_PROFILE_SCALE;
			return (r1 + (r0 - r1) * (mv - v1) / (v0 - v1)) *
			       R_PROFILE_SCALE;
		}
	}
	return res_table[ARRAY_SIZE(res_table) - 1].res * R_PROFILE_SCALE;
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

/* ---- state ---------------------------------------------------------------
 *
 * One cached reading, refreshed at most once a second. A panel widget asks for
 * capacity, voltage, current and charge in quick succession; without this each
 * of those would take its own FGADC latch and eight AUXADC conversions over
 * the pwrap, and they would each see a slightly different instant.
 */
struct fg_state {
	int	vbat_uv;
	int	cur_ua;		/* + discharging, - charging */
	int	ocv_mv;
	s64	car_uah;	/* raw counter, + net charge in */
	s64	q_uah;		/* tracked charge remaining */
	int	soc;
	int	soc_ocv;	/* what OCV alone would have said */
	bool	car_valid;
};

/* Coulomb-counter tracking state. */
static s64 tracked_q_uah;
static s64 car_ref_uah;
static bool tracking;
static unsigned long tracked_at;
static int soc_reported = -1;
static bool seeded_at_rest;

static struct fg_state cached;
static unsigned long cached_at;
static bool cached_ok;
static DEFINE_MUTEX(state_lock);

static int read_state_locked(struct fg_state *st)
{
	struct fg_snapshot snap;
	int r, i, ret;

	ret = auxadc_vbat_uv(&st->vbat_uv);
	if (ret) {
		/* The charger's ADC is coarse but it is a real fallback. */
		ret = charger_prop(POWER_SUPPLY_PROP_VOLTAGE_NOW, &st->vbat_uv);
		if (ret)
			return ret;
	}

	if (fg_current_median(&snap.cur_ua, &snap.car_uah, &snap.valid)) {
		snap.cur_ua = 0;
		snap.car_uah = 0;
		snap.valid = false;
	}
	st->cur_ua = snap.cur_ua;
	st->car_uah = snap.car_uah;
	st->car_valid = snap.valid;

	/*
	 * Two passes: R depends on OCV and OCV depends on R. The curve is
	 * shallow so this converges immediately; the vendor iterates likewise.
	 */
	st->ocv_mv = st->vbat_uv / 1000;
	for (i = 0; i < 2; i++) {
		r = res_at_mv(st->ocv_mv);		/* 0.1 mOhm */
		/* uA * 0.1 mOhm = 0.1 nV; /10^7 -> mV. I is + on discharge,
		 * so this ADDS the load drop back, which is the whole point.
		 */
		st->ocv_mv = st->vbat_uv / 1000 +
			     (int)div_s64((s64)st->cur_ua * r, 10000000);
	}

	st->soc_ocv = clamp(100 - dod_at_mv(st->ocv_mv), 0, 100);

	/*
	 * COULOMB COUNTING, corrected by OCV. This is the part the vendor's
	 * software-OCV path does not do and the part that makes the number
	 * usable on a machine that lives on a charger.
	 *
	 * The counter supplies the motion -- every microamp-hour in or out of
	 * the pack, integrated in hardware at a 103.5 uAh quantum, immune to
	 * the ripple that makes the instantaneous current unusable. The OCV
	 * lookup supplies the absolute reference, because a counter alone
	 * drifts and has no idea where it started.
	 */
	{
		s64 q_ocv = div_s64((s64)st->soc_ocv * Q_MAX_UAH, 100);
		unsigned long now = jiffies;
		int chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
		bool at_rest;
		u32 dt, tau, dt_report;

		dt_report = jiffies_to_msecs(now - tracked_at) / 1000;
		if (!tracking)
			dt_report = 0;

		charger_prop(POWER_SUPPLY_PROP_STATUS, &chg_status);
		/*
		 * "At rest" means terminal voltage IS open-circuit voltage, and
		 * that is a statement about CURRENT, not about the cable.
		 *
		 * The condition here was DISCHARGING && small current, which is
		 * correct and useless: this device lives on a charger and
		 * essentially never reports DISCHARGING, so the fast correction
		 * never engaged and the estimate coasted on whatever the seed
		 * happened to be -- seeded, in one observed case, during a load
		 * dip, and then held ten points low for as long as the machine
		 * stayed plugged in.
		 *
		 * A charger that has TERMINATED is not pushing current. Once
		 * status is Full and the gauge agrees that nothing is flowing,
		 * the pack is as genuinely at rest as it would be unplugged.
		 * The constant-voltage plateau that makes voltage a liar
		 * applies while CHARGING, so that is the case to exclude -- and
		 * only that one.
		 */
		at_rest = (chg_status != POWER_SUPPLY_STATUS_CHARGING) &&
			  (abs(st->cur_ua) < REST_CURRENT_UA);

		if (!tracking || !st->car_valid ||
		    (at_rest &&
		     abs(tracked_q_uah - q_ocv) > div_s64(Q_MAX_UAH, 4))) {
			/*
			 * First reading, or -- AT REST ONLY -- the counter and
			 * the profile have diverged past any plausible drift,
			 * so re-seed rather than defend a number that is
			 * already wrong.
			 *
			 * THE at_rest GATE IS LOAD-BEARING AND WAS MISSING.
			 *
			 * Without it this fires under load, and under load the
			 * OCV estimate is exactly what cannot be trusted. On
			 * battery on 2026-08-27 the pack sagged, OCV = V + I*R
			 * under-compensated (the vendor R profile is far too
			 * small for this cell at ~1 A), the profile said
			 * something like 15% for a pack near 90%, the
			 * divergence sailed past a quarter of Q_MAX, and the
			 * estimate re-seeded ONTO THE BAD NUMBER. Reported
			 * charge then walked down 26%, 16%, 0% while the cell
			 * was fine -- the guard never saw a low voltage at all.
			 *
			 * A divergence under load is precisely the case where
			 * the coulomb counter should be believed and the
			 * profile ignored. That is what the counter is for.
			 */
			tracked_q_uah = q_ocv;
			car_ref_uah = st->car_uah;
			tracking = true;
			seeded_at_rest = at_rest;
		} else if (!seeded_at_rest) {
			/*
			 * NOT YET ANCHORED: the seed we have was taken under
			 * load, so its ABSOLUTE value is not to be trusted --
			 * but count coulombs from it anyway.
			 *
			 * Still true, and why we do not anchor here: an OCV
			 * lookup is only worth anything when nothing is flowing.
			 * Four seconds into boot this device draws 709 mA,
			 * terminal voltage is 3.976 V, and the profile says 65%
			 * for a pack really at 85%. Anchoring there locks in a
			 * twenty-point error and then defends it.
			 *
			 * WHAT CHANGED, 2026-08-31. This branch used to re-take
			 * q_ocv every cycle -- "follow the OCV estimate directly,
			 * no better than the vendor but no worse". On a machine
			 * that rests often that is nearly harmless, because the
			 * anchor arrives within minutes. This machine is a
			 * pocket terminal: at_rest needs |I| < 30 mA and a
			 * running desktop draws ~950 mA, so unplugged and in use
			 * it NEVER rests and never anchors. Measured 2026-08-30,
			 * on battery, this branch running the whole time:
			 *
			 *   capacity fell 11% -> 0% in 197 s, i.e. 17.9 s per
			 *   point against the 20 s slew ceiling -- the reported
			 *   value descending as fast as it was allowed to, while
			 *   the pack still delivered 950 mA for another fifteen
			 *   minutes at "0%".
			 *
			 * It was chasing an OCV computed with the vendor
			 * r_profile, which the note further down measures at
			 * ~20 mOhm against a real 312. Under ~1 A that lookup
			 * lands far down the curve from where the pack is.
			 *
			 * So do here what the anchored branch does: integrate the
			 * hardware counter, which does not care what the
			 * resistance is. The offset stays wrong until a quiet
			 * sample turns up; the SHAPE is right immediately, and
			 * monotonic. A steady twenty-point offset is a bad
			 * gauge. A number that reaches zero with a quarter of the
			 * pack left is a machine that powers off in your hand.
			 *
			 * VERIFIED 2026-08-31 by forcing this branch: a probe
			 * build with REST_CURRENT_UA 0 never satisfies at_rest,
			 * so it stays here for ever. Old code vs new, same pack,
			 * plugged in and idle at ~4.2 V and ~0 A:
			 *
			 *   new  charge_now 3798520 x8, steady, while vbat
			 *        wobbled 4188..4201 mV
			 *   old  charge_now 3841200 / 3798520 / 3755840,
			 *        stepping in lockstep with vbat
			 *
			 * Those three old values are exactly 42680 uAh apart --
			 * 1% of Q_MAX. A 10 mV wobble moved the reported charge
			 * a whole point, and that is at the FLAT top of the
			 * curve with nothing flowing, the best case OCV ever
			 * gets. Down at 3.7 V under 950 mA it is far worse, and
			 * that is the 11% -> 0% walk above.
			 */
			if (at_rest) {
				tracked_q_uah = q_ocv;
				car_ref_uah = st->car_uah;
				seeded_at_rest = true;
				pr_info("mt6351-battery: anchored at %lld uAh (%d%%) on a resting sample\n",
					tracked_q_uah,
					(int)div_s64(tracked_q_uah * 100, Q_MAX_UAH));
			} else {
				s64 d = st->car_uah - car_ref_uah;

				car_ref_uah = st->car_uah;
				/* Same wrap rejection as the anchored branch. */
				if (abs(d) < div_s64(Q_MAX_UAH, 2))
					tracked_q_uah += d;
				tracked_q_uah = clamp_t(s64, tracked_q_uah,
							0, Q_MAX_UAH);
			}
		} else {
			s64 d = st->car_uah - car_ref_uah;

			car_ref_uah = st->car_uah;
			/* A jump larger than half the pack is the counter
			 * wrapping or being reset, not real charge. */
			if (abs(d) < div_s64(Q_MAX_UAH, 2))
				tracked_q_uah += d;

			/*
			 * CORRECT ONLY AT REST. Not "weakly otherwise" -- at
			 * all.
			 *
			 * The OCV estimate is V + I*R, and on this pack R is
			 * not what the vendor profile says. Measured on
			 * 2026-08-27 across 105 samples of a real discharge:
			 * 660 mA at 3867 mV against 1221 mA at 3691 mV, which
			 * is dV/dI = 312 mOhm. The vendor r_profile puts it
			 * near 20. So at 1 A the compensation adds ~20 mV when
			 * it owes ~310, and the profile lookup lands far down
			 * the curve from where the pack really is.
			 *
			 * A weak pull toward a number that wrong is still a
			 * pull toward a number that wrong; it just takes
			 * longer to get there. It was visibly dragging the
			 * reported charge down during the discharge that
			 * produced those samples.
			 *
			 * The coulomb counter has no such problem: it
			 * integrates in hardware and does not care what the
			 * resistance is. So under load, coast on it, and let
			 * the profile speak only when nothing is flowing --
			 * which on this machine is most of the time, because
			 * it lives on a charger and rests at every
			 * termination.
			 */
			dt = at_rest ? jiffies_to_msecs(now - tracked_at) / 1000 : 0;
			tau = TAU_REST;
			if (dt > tau / MAX_CORRECTION_FRAC)
				dt = tau / MAX_CORRECTION_FRAC;
			if (dt)
				tracked_q_uah += div_s64((q_ocv - tracked_q_uah) *
							 dt, tau);
			tracked_q_uah = clamp_t(s64, tracked_q_uah, 0, Q_MAX_UAH);
		}
		tracked_at = now;

		st->q_uah = tracked_q_uah;
		st->soc = (int)div_s64(tracked_q_uah * 100 + Q_MAX_UAH / 2,
				       Q_MAX_UAH);
		st->soc = clamp(st->soc, 0, 100);

		/* Slew-limit what the world sees. The estimate above may step;
		 * the reported percentage may not.
		 */
		if (soc_reported < 0 || !seeded_at_rest) {
			/* Before the anchor there is nothing to protect: the
			 * value is the OCV estimate and should track it. */
			soc_reported = st->soc;
		} else {
			int budget = 1 + (int)(dt_report / SOC_SLEW_SECONDS);

			if (st->soc > soc_reported + budget)
				soc_reported += budget;
			else if (st->soc < soc_reported - budget)
				soc_reported -= budget;
			else
				soc_reported = st->soc;
		}
		st->soc = soc_reported;
	}
	return 0;
}

static int fg_state(struct fg_state *out)
{
	int ret = 0;

	mutex_lock(&state_lock);
	if (!cached_ok || time_after(jiffies, cached_at + HZ)) {
		ret = read_state_locked(&cached);
		if (!ret) {
			cached_at = jiffies;
			cached_ok = true;
		}
	}
	if (!ret)
		*out = cached;
	mutex_unlock(&state_lock);
	return ret;
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
	struct fg_state st;
	int tmp;

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
	default:
		break;
	}

	if (fg_state(&st))
		return -EIO;

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = st.soc;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = st.vbat_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = st.ocv_mv * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* power_supply convention: positive = charging */
		val->intval = -st.cur_ua;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = Q_MAX_MAH * 1000;		/* uAh */
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		/* the tracked charge itself, not soc re-multiplied by Q_MAX --
		 * that round trip threw away everything below one percent */
		val->intval = (int)st.q_uah;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = (int)st.car_uah;
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
	struct fg_state st;

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

	if (!fg_state(&st))
		pr_info("mt6351-battery: BAT0 ready — soc=%d%% (ocv-only %d%%) vbat=%duV ocv=%dmV I=%duA CAR=%lld uAh\n",
			st.soc, st.soc_ocv, st.vbat_uv, st.ocv_mv, st.cur_ua,
			st.car_uah);
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
