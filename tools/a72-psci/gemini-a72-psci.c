// SPDX-License-Identifier: GPL-2.0-only
/*
 * gemini-a72-psci — bring cpu8/cpu9 up the way Android does: prerequisites in
 * the kernel, then PSCI CPU_ON, and let ATF own the cluster.
 *
 * WHY THIS EXISTS, AND WHAT IT RETRACTS
 *
 * B-40 records PSCI CPU_ON as unusable, "by measurement rather than
 * suspicion": ATF's power_on_cl3() ends in a frequency check, the check reads
 * abist source 37, and a sweep of sources 32-47 found 37 "pinned at 629992 kHz
 * alongside seven other sources — it is not wired to the A72 clock here".
 * 629992 satisfies neither of ATF's two acceptance windows, so power_on_cl3
 * would retry forever.
 *
 * That conclusion came from a passive sweep, and the sweep had two faults:
 *
 *   1. CLK26CALI_1 (0x10000224) latches the PREVIOUS measurement. Every
 *      "source N" in that sweep was really source N-1. Measure twice and keep
 *      the second, and the sweep changes.
 *
 *   2. A passive sweep cannot tell "unwired" from "genuinely 630 MHz". The
 *      causal test can: move ARMCAXPLL2 (MCUMIXED 0x1001a224) and watch. Done
 *      on 2026-08-22 with cluster B unpowered:
 *
 *          ARMCAXPLL2 = 630 MHz  ->  src37 = 629992 kHz
 *          ARMCAXPLL2 = 500 MHz  ->  src37 = 499992 kHz
 *          ARMCAXPLL2 = 750 MHz  ->  src37 = 749988 kHz
 *
 *      Source 37 IS the big cluster's clock. It follows one for one. It was
 *      never "pinned"; it was never moved.
 *
 * 749988 is inside ATF's first-pass window (742500 +- 15000). So the check can
 * pass, and PSCI CPU_ON is back on the table.
 *
 * ATF's power_on_cl3(), read out of tee.img at file offset 0x371c:
 *
 *     MP2_CPUSYS_PWR_CON |= PWR_ON              SPM+0x218 bit 2
 *     udelay(2)
 *     while (!(CPU_PWR_STATUS & BIT(17))) ;     SPM+0x188        <- unbounded
 *     while (!(mcucfg(0x102222A0) & BIT(17))) ;                  <- unbounded
 *     assert(mcucfg(0x102224A0) == 0x00FF1100)      <- PLL DISABLED at entry
 *     assert(mcucfg(0x102224A4) == 0xB9B13B14)      <- 750 MHz PCW
 *     assert(mcucfg(0x102224AC) == 0x01B10100)
 *     assert(mcucfg(0x102224B0) == 0x00AF00AF)
 *     assert(mcucfg(0x102224B4) == 0x00000010)
 *     if (retry) mcucfg(0x102224A4) = 0xA6800000     <- ~500 MHz PCW
 *     mcucfg(0x102224A0) = 0x00FF0100 ; udelay(20)
 *     mcucfg(0x102224A0) = 0x00FF0101 ; udelay(1)
 *     mcucfg(0x102224A0) = 0x00FF1101 ; udelay(10)
 *     udelay(30)
 *     sema; ARMPLLDIV_MUXSEL |= 1 ; udelay(1)
 *           ARMPLLDIV_CKDIV = (v & ~0x1f) | 8 ; udelay(1)        ; sema
 *     sema; ARMPLLDIV_ARM_K1 = 0 ; ARMPLLDIV_MON_EN = ~0         ; sema
 *     f = abist(37)
 *     if (retry == 0 && 742500 <= f <= 757500) -> done
 *     if (retry != 0 && 495000 <= f <= 505000) -> done
 *     else: unwind (mux &= ~3, PLL disable, PWR_ON=0, wait bit17 clear,
 *           cycle B_EXT_BUCK_ISO) and retry from PWR_ON
 *
 * Note the asserts are hard (they land in ATF's assert handler), and note the
 * retry path writes 0xA6800000 over 0x102224A4 — which the NEXT iteration's
 * own assert will reject. So ATF gets exactly two attempts before it panics.
 * Both facts mean the entry state has to be ATF's expected reset state: this
 * module must not leave the big PLL enabled or the mux moved, which is exactly
 * what tools/a72-bringup's cluster_clock_up() does leave behind.
 *
 * WHAT THIS MODULE DOES
 *
 *   stage 1  prerequisites only (the vendor's cpu_power_on_buck), then report
 *            ATF's five assertion registers and the lag-free meter. No PSCI.
 *   stage 2  + replicate ATF's clock section and print what ATF's check would
 *            see, then put it all back so ATF's asserts still hold.
 *   stage 3  + raw PSCI CPU_ON to a park stub that writes a mailbox and then
 *            CPU_OFFs itself. Issued inline on a pinned CPU, so if ATF spins
 *            only that CPU is lost and the ATF log ring stays readable.
 *   stage 4  prerequisites, then leave the machine ready for a real
 *            `echo 1 > /sys/devices/system/cpu/cpu8/online`.
 *   stage 0  restore: cluster down, isolation re-asserted, VPROC2 off.
 *
 * SAFETY
 *
 *  - Secure MCUCFG accesses are clamped to ATF's own guard window.
 *  - Nothing here writes a CPU voltage except VPROC2 (BUCKB), which supplies
 *    only the A72s. BUCKA — the rail this kernel runs on — is never touched.
 *    Pin cpufreq before running anyway: it is the other master of i2c6.
 *  - Every poll this module does itself is bounded. The unbounded polls are
 *    ATF's, which is why stage 3 pins itself to one CPU first.
 *
 * Copyright (c) 2026 Gemini PDA Linux port
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/math64.h>
#include <linux/kthread.h>

#define P(fmt, ...) pr_emerg("a72psci: " fmt "\n", ##__VA_ARGS__)

/* ---- ATF's secure MCUCFG accessors ----------------------------------- */

#define SIP_IDVFS_READ		0xC200035FU
#define SIP_IDVFS_WRITE		0xC200035EU
#define SIP_IDVFS_SRAMLDOSET	0xC20003BFU
#define MCUCFG_LO		0x10220000U

static bool in_window(u32 a)
{
	return (a & 0xFFFFC000U) == MCUCFG_LO;
}

static u32 sread(u32 addr)
{
	struct arm_smccc_res res;

	if (!in_window(addr)) {
		P("REFUSING secure read of 0x%08x — outside the ATF window", addr);
		return 0xDEADBEEF;
	}
	arm_smccc_smc(SIP_IDVFS_READ, addr, 0, 0, 0, 0, 0, 0, &res);
	return (u32)res.a0;
}

static long swrite(u32 addr, u32 val)
{
	struct arm_smccc_res res;

	if (!in_window(addr)) {
		P("REFUSING secure write of 0x%08x — outside the ATF window", addr);
		return -EINVAL;
	}
	arm_smccc_smc(SIP_IDVFS_WRITE, addr, val, 0, 0, 0, 0, 0, &res);
	return (long)res.a0;
}

/* ---- register map ---------------------------------------------------- */

#define SPM_PHYS		0x10006000UL
#define SPM_POWERON_CONFIG_EN	0x000
#define SPM_KEY			((0xb16U << 16) | 1U)
#define CPU_PWR_STATUS		0x188
#define MP0_CPUSYS_PWR_CON	0x210
#define MP2_CPUSYS_PWR_CON	0x218
#define MP2_CPU0_PWR_CON	0x240
#define CPU_EXT_BUCK_ISO	0x290

#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_CKISO		BIT(5)
#define SRAM_ISOINT_B		BIT(6)
#define SRAM_PDN		BIT(8)
#define SRAM_PDN_ACK		BIT(12)
#define MP2_CPUTOP_BIT		BIT(17)

#define MCUMIXED_PHYS		0x1001a000UL
#define ARMCAXPLL2_CON0		0x220
#define ARMCAXPLL2_CON1		0x224
#define ARMCAXPLL2_SHADOW	0x228
#define ARMPLLDIV_MUXSEL	0x270
#define ARMPLLDIV_CKDIV		0x274
#define ARMPLLDIV_ARM_K1	0x27c
#define ARMPLLDIV_MON_EN	0x284

#define TOPCKGEN_PHYS		0x10000000UL
#define CLK_DBG_CFG		0x10c
#define CLK_MISC_CFG_0		0x104
#define CLK26CALI_0		0x220
#define CLK26CALI_1		0x224

/*
 * The tail of power_on_cl3(), after its frequency check passes, holds two more
 * unbounded polls (tee.img 0x3dd8..0x3e80):
 *
 *     INFRACFG 0x10001234 &= ~0x444                     ; release MP2's bus
 *     while (readl(0x1000123c) & 0x444) ;               <- unbounded
 *     MCUCFG   0x1022220c &= ~1
 *     CCI      0x10396000 |= 3 | ((cluster + 1) << 4)   ; snoop + DVM enable
 *     while (readl(0x1039000c) & 1) ;                   <- unbounded (change pending)
 *     MCUCFG   0x10222590 |= 0x10000
 *
 * Both are plain non-secure reads, so a watcher on another CPU can say which
 * one ATF is sitting in without issuing an SMC of its own.
 */
#define INFRACFG_PHYS		0x10001000UL
#define TOPAXI_PROTECTEN	0x234
#define TOPAXI_PROTECTEN_STA1	0x23c
#define MP2_BUS_PROT_MASK	0x444u
#define CCI_PHYS		0x10390000UL
#define CCI_STATUS		0x0000c
#define CCI_MP2_SNOOP		0x06000

#define CSPM_PHYS		0x11015000UL
#define CSPM_POWERON_CONFIG	0x000
#define CSPM_SEMA		0x448
#define CSPM_KEY		0x0b160001U

#define DA9214_I2C_ADAPTER	2
#define DA9214_ADDR		0x68
#define DA9214_PAGE_CON		0x00
#define DA9214_BUCKB_CONT	0x5e
#define DA9214_VBUCKB_A		0xd9

/* the A72 big PLL / iDVFS block, and ATF's five assertions on it */
static const struct { u32 addr; u32 want; const char *what; } atf_assert[] = {
	{ 0x102224a0, 0x00FF1100, "big PLL CON0 (must be DISABLED at entry)" },
	{ 0x102224a4, 0xB9B13B14, "big PLL PCW  (750 MHz)" },
	{ 0x102224ac, 0x01B10100, "" },
	{ 0x102224b0, 0x00AF00AF, "" },
	{ 0x102224b4, 0x00000010, "" },
};

/* ---- module parameters ----------------------------------------------- */

static int stage = 1;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage,
	"0 restore; 1 prerequisites+survey; 2 +ATF clock rehearsal; "
	"3 +raw PSCI CPU_ON to a park stub; 4 prerequisites, leave armed");

static int cpu_target = 8;
module_param(cpu_target, int, 0444);

static int vproc2_mv = 1180;
module_param(vproc2_mv, int, 0444);
MODULE_PARM_DESC(vproc2_mv,
	"VPROC2 target in mV. At LK's 1000 the SPMC ack is intermittent and a "
	"failure latches until reboot; 1180 acked first try every time.");

static int settle_ms = 45000;
module_param(settle_ms, int, 0444);
MODULE_PARM_DESC(settle_ms, "rail settle after enabling VPROC2, in ms");

static int armpll2_khz;
module_param(armpll2_khz, int, 0444);
MODULE_PARM_DESC(armpll2_khz,
	"if non-zero, program ARMCAXPLL2 to this before handing over to ATF "
	"(750000 puts abist source 37 inside ATF's first-pass window)");

static int smc_cpu = 7;
module_param(smc_cpu, int, 0444);
MODULE_PARM_DESC(smc_cpu, "which CPU issues the PSCI call (it is the one lost if ATF spins)");

static void __iomem *spm, *mcumixed, *topckgen, *cspm, *infracfg, *cci;

/* the two registers ATF polls without a bound after its frequency check */
static void bus_report(const char *when)
{
	P("  %-22s TOPAXI_PROTECTEN=%08x STA1=%08x (ATF waits for &0x444 == 0)",
	  when, readl(infracfg + TOPAXI_PROTECTEN),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	P("  %-22s CCI STATUS=%08x (ATF waits for bit0 == 0) MP2 snoop=%08x",
	  "", readl(cci + CCI_STATUS), readl(cci + CCI_MP2_SNOOP));
}

/* ---- the abist meter, with the latch lag removed ---------------------- */

/*
 * ATF's own meter, transcribed from tee.img 0x3b10..0x3bd8. The one change is
 * that every measurement is taken twice: CLK26CALI_1 returns the PREVIOUS
 * run's count, and the single-shot version of this is what produced B-40's
 * "source 37 is pinned" reading.
 */
static unsigned int meter_once(unsigned int src)
{
	u32 dbg = readl(topckgen + CLK_DBG_CFG);
	u32 misc = readl(topckgen + CLK_MISC_CFG_0);
	unsigned int t;
	int i;

	writel((dbg & 0xFFC0FFFCu) | (src << 16), topckgen + CLK_DBG_CFG);
	writel((misc & 0x00FFFFFFu) | 0x01000000u, topckgen + CLK_MISC_CFG_0);
	writel(0x1000, topckgen + CLK26CALI_0);
	writel(0x1010, topckgen + CLK26CALI_0);
	for (i = 0; i < 200 && (readl(topckgen + CLK26CALI_0) & 0x10); i++)
		udelay(20);
	udelay(500);
	t = readl(topckgen + CLK26CALI_1) & 0xffff;
	writel(dbg, topckgen + CLK_DBG_CFG);
	writel(misc, topckgen + CLK_MISC_CFG_0);
	writel(0x1010, topckgen + CLK26CALI_0);
	writel(0x1000, topckgen + CLK26CALI_0);
	return (t * 26000u) / 1024u * 2u;
}

static unsigned int meter(unsigned int src)
{
	meter_once(src);
	return meter_once(src);
}

#define ATF_WIN1_LO	742500u
#define ATF_WIN1_HI	757500u
#define ATF_WIN2_LO	495000u
#define ATF_WIN2_HI	505000u

static void meter_report(const char *when)
{
	unsigned int b = meter(37);

	P("  abist(%s): LL(34)=%u L(35)=%u src36=%u BIG(37)=%u kHz",
	  when, meter(34), meter(35), meter(36), b);
	P("  ATF's check on %u: pass1[%u..%u] %s   pass2+[%u..%u] %s",
	  b, ATF_WIN1_LO, ATF_WIN1_HI,
	  (b >= ATF_WIN1_LO && b <= ATF_WIN1_HI) ? "*** PASS ***" : "fail",
	  ATF_WIN2_LO, ATF_WIN2_HI,
	  (b >= ATF_WIN2_LO && b <= ATF_WIN2_HI) ? "*** PASS ***" : "fail");
}

/* ---- ARMCAXPLL2 ------------------------------------------------------- */

static unsigned int pll_khz(u32 con1)
{
	u64 f = 26000ull * (con1 & 0x3FFFFFu);

	return (unsigned int)(f >> 14) >> ((con1 >> 24) & 7);
}

static void armpll2_report(const char *when)
{
	u32 con1 = readl(mcumixed + ARMCAXPLL2_CON1);

	P("  ARMCAXPLL2 (%s): CON0=%08x CON1=%08x shadow=%08x -> %u kHz",
	  when, readl(mcumixed + ARMCAXPLL2_CON0), con1,
	  readl(mcumixed + ARMCAXPLL2_SHADOW), pll_khz(con1));
	P("  MUXSEL=%08x (B=[1:0]) CKDIV=%08x (B=[4:0])",
	  readl(mcumixed + ARMPLLDIV_MUXSEL), readl(mcumixed + ARMPLLDIV_CKDIV));
}

static void armpll2_set(unsigned int khz)
{
	u32 con1 = readl(mcumixed + ARMCAXPLL2_CON1);
	unsigned int posdiv = (con1 >> 24) & 7;
	u32 pcw = (u32)div_u64((u64)khz << (14 + posdiv), 26000);

	P("  ARMCAXPLL2 -> %u kHz: pcw 0x%06x (posdiv %u)", khz, pcw, posdiv);
	writel((con1 & ~0x3FFFFFu) | (pcw & 0x3FFFFFu), mcumixed + ARMCAXPLL2_CON1);
	udelay(200);
	armpll2_report("after set");
}

/* ---- CSPM semaphore, as ATF takes it around MCUMIXED ------------------ */

static bool cspm_sema_take(void)
{
	int i;

	writel(CSPM_KEY, cspm + CSPM_POWERON_CONFIG);
	for (i = 0; i < 200; i++) {
		writel(1, cspm + CSPM_SEMA);
		if (readl(cspm + CSPM_SEMA) & 1)
			return true;
		udelay(10);
	}
	P("  CSPM semaphore not granted in 2 ms (reads %08x) — proceeding",
	  readl(cspm + CSPM_SEMA));
	return false;
}

static void cspm_sema_give(void)
{
	writel(1, cspm + CSPM_SEMA);
}

/* ---- prerequisites: the vendor's cpu_power_on_buck() ------------------ */

static int da9214_rmw(struct i2c_adapter *ad, u8 reg, u8 val, u8 mask, u8 shift)
{
	union i2c_smbus_data d;
	int r;
	u8 cur;

	r = i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_READ, reg,
			   I2C_SMBUS_BYTE_DATA, &d);
	if (r < 0)
		return r;
	cur = d.byte;
	d.byte = (cur & ~(mask << shift)) | (val << shift);
	r = i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_WRITE, reg,
			   I2C_SMBUS_BYTE_DATA, &d);
	P("  DA9214[0x%02x] 0x%02x -> 0x%02x (%d)", reg, cur, d.byte, r);
	return r;
}

static int vproc2_set(bool on)
{
	struct i2c_adapter *ad = i2c_get_adapter(DA9214_I2C_ADAPTER);
	int r;

	if (!ad) {
		P("no i2c adapter %d", DA9214_I2C_ADAPTER);
		return -ENODEV;
	}
	r = da9214_rmw(ad, DA9214_PAGE_CON, 0x0, 0xf, 0);
	if (r >= 0 && on && vproc2_mv) {
		u8 step = (vproc2_mv - 300) / 10;

		P("  VPROC2 target %d mV (VBUCKB_A = 0x%02x)", vproc2_mv,
		  step | 0x80);
		r = da9214_rmw(ad, DA9214_VBUCKB_A, step | 0x80, 0xff, 0);
	}
	if (r >= 0)
		r = da9214_rmw(ad, DA9214_BUCKB_CONT, on ? 0x1 : 0x0, 0x1, 0);
	i2c_put_adapter(ad);
	return r < 0 ? r : 0;
}

static void spm_report(const char *when)
{
	P("  %-22s MP0=%08x MP2=%08x ISO=%08x CPU_PWR_STATUS=%08x "
	  "MP2_CPU0=%08x MP2_CPU1=%08x", when,
	  readl(spm + MP0_CPUSYS_PWR_CON), readl(spm + MP2_CPUSYS_PWR_CON),
	  readl(spm + CPU_EXT_BUCK_ISO), readl(spm + CPU_PWR_STATUS),
	  readl(spm + MP2_CPU0_PWR_CON), readl(spm + MP2_CPU0_PWR_CON + 4));
}

static bool atf_assertions_hold(void)
{
	bool ok = true;
	int i;

	P("---- ATF's five assertions on the big PLL / iDVFS block ----");
	for (i = 0; i < ARRAY_SIZE(atf_assert); i++) {
		u32 v = sread(atf_assert[i].addr);
		bool good = (v == atf_assert[i].want);

		P("  0x%08x = %08x  want %08x  %s %s", atf_assert[i].addr, v,
		  atf_assert[i].want, good ? "ok" : "*** MISMATCH — ATF WOULD "
		  "PANIC ***", atf_assert[i].what);
		ok = ok && good;
	}
	return ok;
}

static int prerequisites(void)
{
	struct arm_smccc_res res;
	u32 before, after;

	P("==== prerequisites (vendor cpu_power_on_buck, psci.c:464) ====");
	spm_report("before");

	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);

	P("step 1: MP2_CPUSYS_PWR_CON |= PWR_RST_B");
	writel(readl(spm + MP2_CPUSYS_PWR_CON) | PWR_RST_B,
	       spm + MP2_CPUSYS_PWR_CON);

	P("step 2: dummy read of 0x102224a0 = %08x", sread(0x102224a0));

	P("step 3: VPROC2 (DA9214 BUCKB) on");
	if (vproc2_set(true)) {
		P("  VPROC2 ENABLE FAILED — stopping, the rest is meaningless");
		return -EIO;
	}
	P("step 3b: letting the rail settle for %d ms", settle_ms);
	msleep(settle_ms);

	P("step 4: CPU_EXT_BUCK_ISO %08x -> clear bits[1:0]",
	  readl(spm + CPU_EXT_BUCK_ISO));
	writel(readl(spm + CPU_EXT_BUCK_ISO) & ~0x3u, spm + CPU_EXT_BUCK_ISO);
	udelay(240);
	P("  CPU_EXT_BUCK_ISO now %08x", readl(spm + CPU_EXT_BUCK_ISO));

	before = sread(0x102222b0);
	arm_smccc_smc(SIP_IDVFS_SRAMLDOSET, 110000, 0, 0, 0, 0, 0, 0, &res);
	udelay(240);
	after = sread(0x102222b0);
	P("step 5: BigiDVFSSRAMLDOSet(110000) -> %ld, 0x102222b0 %08x -> %08x",
	  (long)res.a0, before, after);

	spm_report("after prerequisites");
	return 0;
}

/* ---- ATF's two unbounded polls, done here with a bound ---------------- */

/*
 * power_on_cl3() opens with
 *
 *     MP2_CPUSYS_PWR_CON |= PWR_ON ; udelay(2)
 *     while (!(CPU_PWR_STATUS  & BIT(17))) ;
 *     while (!(mcucfg(0x102222A0) & BIT(17))) ;
 *
 * and neither loop has a timeout, so if the SPMC does not answer, the SMC
 * never returns and the machine is lost. Doing it here first, bounded, has two
 * effects worth having: the failure is a report instead of a hang, and if it
 * succeeds the state ATF finds is already past both polls, so ATF cannot spin
 * there at all.
 *
 * The escape from a stuck SPMC is ATF's own, from its retry tail: drop PWR_ON,
 * wait for the domain to go down, then cycle B_EXT_BUCK_ISO (assert, 100 us,
 * deassert, 240 us). At VPROC2 = 1000 mV the ack is intermittent and a failure
 * latches; at 1180 mV it has acked first try every time.
 */
static bool cputop_power_on(void)
{
	void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
	bool ack = false;
	int i, k;

	P("==== MP2 CPUTOP power-on (ATF's power_on_cl3 head, bounded) ====");
	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);

	for (k = 0; k < 8 && !ack; k++) {
		writel(readl(r) | PWR_ON, r);
		udelay(2);
		for (i = 0; i < 20000; i++) {
			if (readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) {
				ack = true;
				break;
			}
			udelay(1);
		}
		P("  attempt %d: CPU_PWR_STATUS bit17 %s after %d us "
		  "(MP2=%08x, MCUCFG 0x102222a0=%08x)", k + 1,
		  ack ? "SET" : "TIMEOUT", i, readl(r), sread(0x102222a0));
		if (ack)
			break;

		writel(readl(r) & ~PWR_ON, r);
		for (i = 0; i < 20000 &&
		     (readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT); i++)
			udelay(1);
		writel(readl(spm + CPU_EXT_BUCK_ISO) | 0x2u, spm + CPU_EXT_BUCK_ISO);
		udelay(100);
		writel(readl(spm + CPU_EXT_BUCK_ISO) & ~0x2u, spm + CPU_EXT_BUCK_ISO);
		udelay(240);
	}
	if (!ack) {
		P("  the SPMC never acked — ATF would spin here forever. STOPPING.");
		return false;
	}

	for (i = 0; i < 20000; i++) {
		if (sread(0x102222a0) & BIT(17))
			break;
		udelay(1);
	}
	P("  MCUCFG 0x102222a0 bit17 %s after %d us (=%08x)",
	  (sread(0x102222a0) & BIT(17)) ? "SET" : "TIMEOUT", i, sread(0x102222a0));

	spm_report("after CPUTOP power-on");
	P("  0x10222700 (spark vret) = %08x, writing 0x3f", sread(0x10222700));
	swrite(0x10222700, 0x3f);
	P("  -> %08x", sread(0x10222700));
	return !!(sread(0x102222a0) & BIT(17));
}

/* ---- stage 2: rehearse ATF's clock section, then put it back ---------- */

/*
 * Everything power_on_cl3() does between its two power-good polls and its
 * frequency check, run here so the check's input can be read without ATF
 * spinning on the answer. Undone afterwards, because ATF asserts on the entry
 * state of exactly these registers.
 */
static void atf_clock_rehearsal(void)
{
	u32 pll0, muxsel0, ckdiv0, k1_0, mon0;

	P("==== rehearsing ATF's power_on_cl3 clock section ====");
	pll0 = sread(0x102224a0);
	muxsel0 = readl(mcumixed + ARMPLLDIV_MUXSEL);
	ckdiv0 = readl(mcumixed + ARMPLLDIV_CKDIV);
	k1_0 = readl(mcumixed + ARMPLLDIV_ARM_K1);
	mon0 = readl(mcumixed + ARMPLLDIV_MON_EN);
	P("  saved: PLL=%08x MUXSEL=%08x CKDIV=%08x K1=%08x MON=%08x",
	  pll0, muxsel0, ckdiv0, k1_0, mon0);

	meter_report("before ATF's clock steps");

	swrite(0x102224a0, 0x00FF0100);
	udelay(20);
	swrite(0x102224a0, 0x00FF0101);
	udelay(1);
	swrite(0x102224a0, 0x00FF1101);
	udelay(10);
	udelay(30);
	P("  big PLL 0x102224a0 = %08x  PCW 0x102224a4 = %08x",
	  sread(0x102224a0), sread(0x102224a4));

	cspm_sema_take();
	writel(readl(mcumixed + ARMPLLDIV_MUXSEL) | 1, mcumixed + ARMPLLDIV_MUXSEL);
	udelay(1);
	writel((readl(mcumixed + ARMPLLDIV_CKDIV) & ~0x1fu) | 8,
	       mcumixed + ARMPLLDIV_CKDIV);
	udelay(1);
	cspm_sema_give();

	cspm_sema_take();
	writel(0, mcumixed + ARMPLLDIV_ARM_K1);
	writel(0xffffffff, mcumixed + ARMPLLDIV_MON_EN);
	udelay(1);
	cspm_sema_give();

	P("  MUXSEL=%08x CKDIV=%08x", readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV));
	meter_report("at ATF's decision point");

	P("  putting it back so ATF's entry assertions hold");
	cspm_sema_take();
	writel(muxsel0, mcumixed + ARMPLLDIV_MUXSEL);
	writel(ckdiv0, mcumixed + ARMPLLDIV_CKDIV);
	writel(k1_0, mcumixed + ARMPLLDIV_ARM_K1);
	writel(mon0, mcumixed + ARMPLLDIV_MON_EN);
	cspm_sema_give();
	swrite(0x102224a0, pll0);
	udelay(100);
	P("  restored: PLL=%08x MUXSEL=%08x CKDIV=%08x", sread(0x102224a0),
	  readl(mcumixed + ARMPLLDIV_MUXSEL), readl(mcumixed + ARMPLLDIV_CKDIV));
	atf_assertions_hold();
}

/* ---- stage 5: rehearse power_on_big()'s two core polls ---------------- */

/*
 * After power_on_cl3() returns, power_on_big() does, for the core (tee.img
 * 0x47f4..0x488c):
 *
 *     MP2_CPUn_PWR_CON &= ~PWR_RST_B
 *     MP2_CPUn_PWR_CON |= PWR_ON ; udelay(2)
 *     while (!(CPU_PWR_STATUS & BIT(15 - cpu))) ;          <- unbounded
 *     while (!(mcucfg(0x10222430 + idx*4) & BIT(17))) ;    <- unbounded
 *     MP2_CPUn_PWR_CON |= PWR_RST_B
 *
 * and nothing else: no PWR_ON_2ND, no ISO, no SRAM, no clock bits. Those two
 * polls are the only places left where a PSCI CPU_ON can spin once the cluster
 * is already powered, so measure them here, bounded, and put the core back.
 *
 * The core MUST be left powered down afterwards. power_on_big() reads
 * MP2_CPUn_PWR_CON first and, if PWR_ON is already set, prints "The required
 * Big core:%d was powered on" and RETURNS — never writing the boot address and
 * never releasing reset. A pre-powered core is not a shortcut, it is a no-op.
 */
static void atf_core_poll_rehearsal(int cpu)
{
	int idx = cpu - 8;
	void __iomem *r = spm + MP2_CPU0_PWR_CON + idx * 4;
	u32 spmc = 0x10222430U + idx * 4;
	bool ok1 = false, ok2 = false;
	int i;

	P("==== rehearsing power_on_big()'s two core polls for cpu%d ====", cpu);
	P("  MP2_CPU%d_PWR_CON = %08x  core SPMC 0x%08x = %08x", idx,
	  readl(r), spmc, sread(spmc));

	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);
	writel(readl(r) & ~PWR_RST_B, r);
	writel(readl(r) | PWR_ON, r);
	udelay(2);

	for (i = 0; i < 20000; i++) {
		if (readl(spm + CPU_PWR_STATUS) & BIT(15 - cpu)) {
			ok1 = true;
			break;
		}
		udelay(1);
	}
	P("  poll 1: CPU_PWR_STATUS bit%d %s after %d us (=%08x)", 15 - cpu,
	  ok1 ? "SET" : "*** TIMEOUT — ATF WOULD SPIN HERE ***", i,
	  readl(spm + CPU_PWR_STATUS));

	for (i = 0; i < 20000; i++) {
		if (sread(spmc) & BIT(17)) {
			ok2 = true;
			break;
		}
		udelay(1);
	}
	P("  poll 2: core SPMC 0x%08x bit17 %s after %d us (=%08x)", spmc,
	  ok2 ? "SET" : "*** TIMEOUT — ATF WOULD SPIN HERE ***", i, sread(spmc));
	P("  MP2_CPU%d_PWR_CON now %08x (a running A53 core = %08x)", idx,
	  readl(r), readl(spm + 0x220));

	P("  putting the core back down — a pre-powered core makes power_on_big "
	  "return without releasing it");
	writel(readl(r) & ~PWR_RST_B, r);
	writel(readl(r) | PWR_CLK_DIS, r);
	writel(readl(r) & ~(PWR_ON | PWR_ON_2ND), r);
	udelay(100);
	P("  MP2_CPU%d_PWR_CON = %08x  CPU_PWR_STATUS = %08x  SPMC = %08x", idx,
	  readl(r), readl(spm + CPU_PWR_STATUS), sread(spmc));
	P("  VERDICT: PSCI CPU_ON %s spin in power_on_big",
	  (ok1 && ok2) ? "will NOT" : "*** WILL ***");
}

/* ---- the CSPM semaphore ATF also polls without a bound ---------------- */

static void cspm_sema_report_and_free(void)
{
	u32 v;

	writel(CSPM_KEY, cspm + CSPM_POWERON_CONFIG);
	v = readl(cspm + CSPM_SEMA);
	P("  CSPM_SEMA = %08x %s", v,
	  (v & 1) ? "— HELD; ATF polls this without a bound, releasing it"
		  : "— free, ATF can take it");
	if (v & 1) {
		writel(1, cspm + CSPM_SEMA);
		P("  CSPM_SEMA now %08x", readl(cspm + CSPM_SEMA));
	}
}

/* ---- ATF's own log ring, read while ATF is still inside the SMC ------- */

/*
 * BL31 keeps a text ring at physical 0x7FF40000 — the tee_reserved_mem LK puts
 * in our device tree too — with a four-word header:
 *
 *     +0x00  buffer base PA      (0x7FF40100)
 *     +0x04  buffer size         (0x00029F00)
 *     +0x08  write pointer PA
 *     +0x0c  start PA
 *
 * and it is a *debug* build, so power_on_cl3/power_on_big narrate themselves:
 * "power on CPU%lu", "big armpll = %d Khz, retry = %u.", "%s sparkvretcntrl=%x",
 * "The required Big core:%d is not existed", "PANIC at PC".
 *
 * The first attempt at this read it from userspace after the fact, which does
 * not work twice over: the ring is zeroed on every boot, and once a CPU is
 * stuck at EL3 the machine stops accepting new logins — every fresh sshd does
 * a seccomp filter, which JITs, which calls kick_all_cpus_sync(), which IPIs
 * the CPU that will never answer. So the reader has to already be running, in
 * the kernel, on a different CPU, printing to netconsole.
 *
 * It is a kthread started BEFORE the SMC. Stage 3 deliberately never returns
 * from module_init when ATF spins, so the module is never freed and the thread
 * keeps its code.
 */
#define ATF_RING_PHYS	0x7FF40000UL
#define ATF_RING_MAP	0x2A000UL

static void __iomem *atf_ring;
static u32 atf_seen;

static void atf_ring_dump(const char *when)
{
	u32 base, size, wptr, from, i, n;
	static char line[240];
	unsigned int col = 0;

	if (!atf_ring)
		return;
	base = readl(atf_ring + 0x00);
	size = readl(atf_ring + 0x04);
	wptr = readl(atf_ring + 0x08);
	if (base != ATF_RING_PHYS + 0x100 || size > ATF_RING_MAP ||
	    wptr < base || wptr > base + size) {
		P("  ATF ring header looks wrong: base=%08x size=%08x wptr=%08x",
		  base, size, wptr);
		return;
	}
	n = wptr - base;
	if (!atf_seen)
		atf_seen = n > 0x600 ? n - 0x600 : 0;	/* first dump: recent tail */
	if (n <= atf_seen) {
		P("  ATF ring (%s): nothing new (%u bytes total)", when, n);
		return;
	}
	from = atf_seen;
	P("  ---- ATF ring (%s): bytes %u..%u ----", when, from, n);
	for (i = from; i < n; i++) {
		char c = readb(atf_ring + 0x100 + i);

		if (c == '\n' || c == '\r' || col == sizeof(line) - 1) {
			line[col] = 0;
			if (col)
				P("  ATF| %s", line);
			col = 0;
			continue;
		}
		line[col++] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	if (col) {
		line[col] = 0;
		P("  ATF| %s", line);
	}
	atf_seen = n;
}

static struct task_struct *ringwatch;

static int ringwatch_fn(void *unused)
{
	int n;

	for (n = 0; n < 120 && !kthread_should_stop(); n++) {
		ssleep(5);
		P("==== ring watch tick %d (cpu%d) ====", n, smp_processor_id());
		atf_ring_dump("watch");
		spm_report("watch");
		/*
		 * Non-secure reads ONLY. A secure accessor is an SMC, and
		 * issuing one while another CPU is parked at EL3 is how the
		 * watcher would join it. These four are enough to say which of
		 * ATF's unbounded polls it is sitting in:
		 *
		 *   MP2_CPUn_PWR_CON PWR_ON set -> it is past power_on_cl3 and
		 *                                  inside power_on_big
		 *   MUXSEL[1:0]/CKDIV moved      -> it reached the clock steps
		 *   CSPM_SEMA bit0 clear         -> it is stuck on the semaphore
		 *   abist(37)                    -> what its check is reading
		 */
		P("  watch: MUXSEL=%08x CKDIV=%08x CSPM_SEMA=%08x abist37=%u kHz",
		  readl(mcumixed + ARMPLLDIV_MUXSEL),
		  readl(mcumixed + ARMPLLDIV_CKDIV),
		  readl(cspm + CSPM_SEMA), meter(37));
		bus_report("watch");
	}
	return 0;
}

static void ringwatch_start(void)
{
	atf_ring = ioremap(ATF_RING_PHYS, ATF_RING_MAP);
	if (!atf_ring) {
		P("  could not map ATF's log ring at 0x%lx", ATF_RING_PHYS);
		return;
	}
	atf_ring_dump("before CPU_ON");

	ringwatch = kthread_create(ringwatch_fn, NULL, "a72-ringwatch");
	if (IS_ERR(ringwatch)) {
		P("  could not start the ring watcher");
		ringwatch = NULL;
		return;
	}
	kthread_bind(ringwatch, 0);
	wake_up_process(ringwatch);
	P("  ring watcher running on cpu0; it survives a spin on cpu%d", smc_cpu);
}

/* ---- stage 3: the park stub and the raw PSCI call --------------------- */

/*
 *   movz x0, #mbox_lo ; movk x0, #mbox_hi, lsl #16
 *   movz w1, #0x72a7  ; movk w1, #0xa72a, lsl #16
 *   str  w1, [x0]     ; dsb sy
 *   movz x0, #0x0002  ; movk x0, #0x8400, lsl #16     ; PSCI CPU_OFF
 *   smc  #0
 *   1: b 1b
 *
 * It calls CPU_OFF rather than spinning so a successful run leaves ATF's own
 * idea of the core consistent, and a real cpu_up() afterwards is not answered
 * with ALREADY_ON. The b-to-self is only there for the case where CPU_OFF
 * returns, which by the spec it must not.
 */
static const u32 stub[] = {
	0xd2800000,	/* movz x0, #0        (patched: mailbox low) */
	0xf2a00000,	/* movk x0, #0, lsl16 (patched: mailbox high) */
	0x528e54e1,	/* movz w1, #0x72a7 */
	0x72b4e541,	/* movk w1, #0xa72a, lsl #16 */
	0xb9000001,	/* str  w1, [x0] */
	0xd5033f9f,	/* dsb  sy */
	0xd2800040,	/* movz x0, #0x0002 */
	0xf2b08000,	/* movk x0, #0x8400, lsl #16 */
	0xd4000003,	/* smc  #0 */
	0x14000000,	/* b .  */
};
#define A72_MAGIC	0xa72a72a7u
#define MOVZ_X0(i)	(0xd2800000u | (((i) & 0xffffu) << 5))
#define MOVK_X0_16(i)	(0xf2a00000u | (((i) & 0xffffu) << 5))
#define B_SELF		0x14000000u

#define PSCI_CPU_ON_64	0xC4000003U

static struct platform_device *pdev;
static u32 *coh;
static dma_addr_t coh_dma;
static u32 *mbox;
static phys_addr_t park_pa;

static int make_mailbox_and_park(void)
{
	u32 *park;
	int w;

	pdev = platform_device_register_simple("gemini-a72-psci",
					       PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(pdev)) {
		P("no staging device");
		return -ENODEV;
	}
	if (dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		P("no 32-bit DMA mask");
		return -ENODEV;
	}
	coh = dma_alloc_coherent(&pdev->dev, 2 * PAGE_SIZE, &coh_dma, GFP_KERNEL);
	if (!coh) {
		P("no coherent region");
		return -ENOMEM;
	}
	mbox = coh;
	park = (u32 *)((u8 *)coh + PAGE_SIZE);
	park_pa = coh_dma + PAGE_SIZE;

	*mbox = 0x5a5a5a5a;
	if (*mbox != 0x5a5a5a5a) {
		P("mailbox will not hold a value — it could not tell us anything");
		return -EIO;
	}
	*mbox = 0;

	for (w = 0; w < PAGE_SIZE / 4; w++)
		park[w] = B_SELF;
	memcpy(park, stub, sizeof(stub));
	park[0] = MOVZ_X0((u32)coh_dma);
	park[1] = MOVK_X0_16((u32)coh_dma >> 16);
	dma_wmb();

	if ((park_pa >> 32) || (coh_dma >> 32)) {
		P("coherent region landed above 4 GB — refusing");
		return -EIO;
	}
	P("mailbox PA 0x%llx, park PA 0x%llx (uncached); stub %08x %08x %08x %08x",
	  (unsigned long long)coh_dma, (unsigned long long)park_pa,
	  park[0], park[1], park[2], park[3]);
	return 0;
}

static void raw_cpu_on(int cpu)
{
	struct arm_smccc_res res;
	u64 mpidr = (cpu == 9) ? 0x201 : 0x200;
	int i;

	P("==== raw PSCI CPU_ON ====");
	P("  pinning this thread to cpu%d — if ATF spins, that is the CPU lost",
	  smc_cpu);
	if (set_cpus_allowed_ptr(current, cpumask_of(smc_cpu)))
		P("  could not pin to cpu%d; continuing on cpu%d", smc_cpu,
		  smp_processor_id());
	schedule();
	P("  running on cpu%d", smp_processor_id());

	spm_report("immediately before CPU_ON");
	atf_assertions_hold();
	armpll2_report("immediately before CPU_ON");
	ringwatch_start();
	P("  CPU_ON(mpidr=0x%llx, entry=0x%llx) — from here ATF owns the cluster",
	  mpidr, (unsigned long long)park_pa);

	arm_smccc_smc(PSCI_CPU_ON_64, mpidr, park_pa, 0, 0, 0, 0, 0, &res);

	P("  *** CPU_ON RETURNED %ld ***", (long)res.a0);
	spm_report("after CPU_ON");
	atf_ring_dump("after CPU_ON");

	for (i = 0; i < 200; i++) {
		if (*mbox == A72_MAGIC) {
			P("  *** +%d ms MAILBOX = %08x — THE A72 EXECUTED ***",
			  (i + 1) * 10, *mbox);
			break;
		}
		mdelay(10);
	}
	P("final: mailbox=%08x  %s", *mbox,
	  *mbox == A72_MAGIC ? "*** A72 RAN ***" : "no execution seen");
	spm_report("final");
	P("  MCUCFG 0x102222a0=%08x boot@0x10222290=%08x", sread(0x102222a0),
	  sread(0x10222290));
	atf_ring_dump("final");
	if (ringwatch) {
		kthread_stop(ringwatch);
		ringwatch = NULL;
	}
}

/* ---- stage 0: put it all back ---------------------------------------- */

static void restore(void)
{
	void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
	int i;

	P("==== restore ====");
	spm_report("before restore");
	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);

	for (i = 0; i < 2; i++) {
		void __iomem *c = spm + MP2_CPU0_PWR_CON + i * 4;

		writel(readl(c) | PWR_ISO, c);
		writel(readl(c) | SRAM_CKISO, c);
		writel(readl(c) & ~SRAM_ISOINT_B, c);
		writel(readl(c) | SRAM_PDN, c);
		writel(readl(c) & ~PWR_RST_B, c);
		writel(readl(c) | PWR_CLK_DIS, c);
		writel(readl(c) & ~(PWR_ON | PWR_ON_2ND), c);
	}

	writel(readl(r) | PWR_ISO, r);
	writel(readl(r) | SRAM_CKISO, r);
	writel(readl(r) & ~SRAM_ISOINT_B, r);
	writel(readl(r) | SRAM_PDN, r);
	writel(readl(r) & ~PWR_RST_B, r);
	writel(readl(r) | PWR_CLK_DIS, r);
	writel(readl(r) & ~(PWR_ON | PWR_ON_2ND), r);
	for (i = 0; i < 2000 && (readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT); i++)
		udelay(1);

	P("re-asserting CPU_EXT_BUCK_ISO");
	writel(readl(spm + CPU_EXT_BUCK_ISO) | 0x3u, spm + CPU_EXT_BUCK_ISO);
	udelay(100);

	P("turning VPROC2 off");
	vproc2_set(false);
	spm_report("after restore");
}

/* ---- entry ------------------------------------------------------------ */

static int __init a72psci_init(void)
{
	P("==== gemini-a72-psci stage=%d cpu=%d vproc2=%d mV settle=%d ms ====",
	  stage, cpu_target, vproc2_mv, settle_ms);

	spm = ioremap(SPM_PHYS, 0x1000);
	mcumixed = ioremap(MCUMIXED_PHYS, 0x1000);
	topckgen = ioremap(TOPCKGEN_PHYS, 0x1000);
	cspm = ioremap(CSPM_PHYS, 0x1000);
	infracfg = ioremap(INFRACFG_PHYS, 0x1000);
	cci = ioremap(CCI_PHYS, 0x10000);
	if (!spm || !mcumixed || !topckgen || !cspm || !infracfg || !cci) {
		P("ioremap failed");
		goto out;
	}

	if (stage == 0) {
		restore();
		goto out;
	}

	armpll2_report("at entry");
	meter_report("at entry");

	if (prerequisites())
		goto out;

	if (armpll2_khz)
		armpll2_set(armpll2_khz);

	meter_report("after prerequisites");

	/*
	 * The 0x102224xx block reads all-zero until the cluster domain is
	 * powered — measured, with VPROC2 up and CPU_EXT_BUCK_ISO clear, while
	 * its neighbour 0x102222b0 read real values through the same accessor.
	 * So ATF's five assertions can only be evaluated on the far side of its
	 * two power-good polls, which is also where ATF evaluates them.
	 */
	if (stage < 2)
		goto out;

	if (!cputop_power_on())
		goto out;

	if (!atf_assertions_hold())
		P("  NOTE: ATF would panic with the block in this state");

	meter_report("cluster powered, before ATF's clock steps");

	if (stage == 2) {
		atf_clock_rehearsal();
		goto out;
	}

	if (stage == 5) {
		atf_clock_rehearsal();
		atf_core_poll_rehearsal(cpu_target);
		cspm_sema_report_and_free();
		bus_report("stage 5");
		P("Every unbounded poll in ATF's path has now been measured "
		  "bounded. Stage 3 is the real call.");
		goto out;
	}

	if (stage == 3) {
		cspm_sema_report_and_free();
		bus_report("before CPU_ON");
		if (make_mailbox_and_park()) {
			P("could not build the instrument — refusing to fire");
			goto out;
		}
		raw_cpu_on(cpu_target);
		goto out;
	}

	if (stage == 4) {
		atf_clock_rehearsal();
		P("prerequisites are in place, VPROC2 is UP and the CPUTOP domain "
		  "is powered past both of ATF's unbounded polls.");
		P("Now: echo 1 > /sys/devices/system/cpu/cpu%d/online", cpu_target);
	}

out:
	P("==== end ====");
	return -EAGAIN;	/* never actually stay loaded */
}

module_init(a72psci_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mt6797 A72 bring-up through PSCI, with ATF's own frequency gate satisfied");
