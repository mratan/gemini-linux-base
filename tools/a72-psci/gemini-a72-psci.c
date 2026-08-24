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
 *   stage 1  prerequisites only (the vendor's cpu_power_on_buck) and the
 *            lag-free meter. ATF's assertion registers are NOT readable here:
 *            the 0x102224xx block is dark until the cluster is powered.
 *   stage 2  + power the cluster, check ATF's five assertions, replicate its
 *            clock section and print what its check would see, then put it all
 *            back so those assertions still hold.
 *   stage 5  + rehearse every remaining unbounded poll in ATF's path, bounded,
 *            and restore. `rehearse_cci=1` adds the one that can stall the
 *            interconnect.
 *   stage 3  + raw PSCI CPU_ON to a park stub that writes a mailbox and then
 *            CPU_OFFs itself. Issued inline on a pinned CPU, so if ATF spins
 *            only that CPU is lost and the ATF log ring stays readable. It
 *            REFUSES to fire unless ATF's assertions hold and the meter is
 *            inside one of ATF's acceptance windows.
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
#include <asm/cpufeature.h>
#include <linux/cpuhotplug.h>
#include <linux/nmi.h>
#include <linux/kernel_stat.h>
#include <asm/sysreg.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/math64.h>
#include <linux/kthread.h>
#include <linux/completion.h>

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
#define CPU_PWR_STATUS_2ND	0x18c
#define MP0_CPUSYS_PWR_CON	0x210
#define MP2_CPUSYS_PWR_CON	0x218
#define MP2_CPU0_PWR_CON	0x240
#define CPU_IDLE_STA		0x174
#define CPU_IDLE_STA2		0x178
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
#define SRAM_SLEEP_B		BIT(16)
#define SRAM_SLEEP_B_ACK	BIT(19)
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
#define CSPM_PCM_CON0		0x018
#define CSPM_PCM_FSM_STA	0x178
#define CSPM_SEMA		0x448
#define CSPM_KEY		0x0b160001U
#define CSPM_CON0_KEY		(0xb16U << 16)
#define CSPM_PCM_SW_RESET	BIT(15)

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
	"0 restore; 1 prerequisites + survey (no cluster power, so ATF's "
	"assertion registers are not yet readable); 2 + cluster power and ATF's "
	"clock rehearsal; 5 + every remaining unbounded poll rehearsed bounded; "
	"3 + raw PSCI CPU_ON to a park stub; 4 prerequisites, leave armed for "
	"`echo 1 > cpuN/online`");

static int cpu_target = 8;
module_param(cpu_target, int, 0444);
MODULE_PARM_DESC(cpu_target, "8 or 9 — nothing else is an A72");

static int vproc2_mv = 1180;
module_param(vproc2_mv, int, 0444);
MODULE_PARM_DESC(vproc2_mv,
	"VPROC2 target in mV. At LK's 1000 the SPMC ack is intermittent and a "
	"failure latches until reboot; 1180 acked first try every time.");

static int settle_ms = 45000;
module_param(settle_ms, int, 0444);
MODULE_PARM_DESC(settle_ms, "rail settle after enabling VPROC2, in ms");

static int vproc2_already_on;
module_param(vproc2_already_on, int, 0444);
MODULE_PARM_DESC(vproc2_already_on,
	"skip all DA9214 traffic because stage 1 already enabled and verified "
	"VPROC2; required once the unpaused PCM owns i2c6");

static int armpll2_khz;
module_param(armpll2_khz, int, 0444);
MODULE_PARM_DESC(armpll2_khz,
	"if non-zero, program ARMCAXPLL2 to this before handing over to ATF "
	"(750000 puts abist source 37 inside ATF's first-pass window)");

static int smc_cpu = 7;
module_param(smc_cpu, int, 0444);
MODULE_PARM_DESC(smc_cpu, "which CPU issues the PSCI call (it is the one lost if ATF spins)");

static int force_clock_from_watcher;
module_param(force_clock_from_watcher, int, 0444);
MODULE_PARM_DESC(force_clock_from_watcher,
	"while ATF retries its frequency measurement, repeatedly force cluster "
	"B's non-secure mux/divider to ATF's own values from the watcher kthread");

static int stop_pcm_from_watcher;
module_param(stop_pcm_from_watcher, int, 0444);
MODULE_PARM_DESC(stop_pcm_from_watcher,
	"reset/stop the paused CPUHVFS PCM from the watcher after ATF has entered "
	"its frequency retry, removing the clock owner without touching abist");

static int prime_atf_meter;
module_param(prime_atf_meter, int, 0444);
MODULE_PARM_DESC(prime_atf_meter,
	"work around ATF's one-sample meter lag: 1 rehearses source37 on a "
	"pre-powered CPUTOP; 2 latches the already-750MHz source36 and works "
	"with CPUTOP still off");

static int release_mp2_sram_from_watcher;
module_param(release_mp2_sram_from_watcher, int, 0444);
MODULE_PARM_DESC(release_mp2_sram_from_watcher,
	"after the primed frequency check reaches poll 5, apply the vendor "
	"MTCMOS SRAM/isolation release sequence to MP2 from the watcher");

static int full_cputop_mtcmos;
module_param(full_cputop_mtcmos, int, 0444);
MODULE_PARM_DESC(full_cputop_mtcmos,
	"assert PWR_ON_2ND one microsecond after PWR_ON and complete MP2's "
	"SRAM_SLEEP_B/isolation sequence during the bounded CPUTOP pre-power");

static int handoff_b_pcm_from_watcher;
module_param(handoff_b_pcm_from_watcher, int, 0444);
MODULE_PARM_DESC(handoff_b_pcm_from_watcher,
	"for the Android-like unpaused/CLUSTER_EN state, pause B to release "
	"ATF's SPMC poll, then unpause it once ATF reaches the clock/bus stage");

static int enable_cci_from_watcher;
module_param(enable_cci_from_watcher, int, 0444);
MODULE_PARM_DESC(enable_cci_from_watcher,
	"at post-frequency poll 5, enable MP2's CCI snoop/DVM port before ATF's "
	"cold-start ordering would normally do so");

static int power_core_from_watcher;
module_param(power_core_from_watcher, int, 0444);
MODULE_PARM_DESC(power_core_from_watcher,
	"at poll 5, assert the target core's PWR_ON after the bounded MP2 MTCMOS "
	"release and report whether a live core withdraws bus protection");

static int pwrap_latch;
module_param(pwrap_latch, int, 0444);
MODULE_PARM_DESC(pwrap_latch,
	"hold the PMIC wrapper's SPI controller in reset across the EXT_BUCK_ISO "
	"change, as the vendor's cpu_power_on_buck() does (WDT_SWSYSRST bit 11)");

static char *late_caps;
module_param(late_caps, charp, 0444);
MODULE_PARM_DESC(late_caps,
	"comma-separated arm64 capability numbers to mark present system-wide "
	"before onlining an A72, so verify_local_cpu_capabilities() does not "
	"reject it for carrying an erratum the A53 boot CPUs do not have");

static int disable_big_spark;
module_param(disable_big_spark, int, 0444);
MODULE_PARM_DESC(disable_big_spark,
	"after onlining, clear bit 0 of the core's SPMC (0x10222430/4) to undo "
	"ATF's big_spark2_core(core,1) retention enable");

static int also_cpu = -1;
module_param(also_cpu, int, 0444);
MODULE_PARM_DESC(also_cpu,
	"after the first big core is up and kept alive, online the other one too "
	"(the second-core path: ATF skips power_on_cl3 because big_on != 0)");

static int spin_ms;
module_param(spin_ms, int, 0444);
MODULE_PARM_DESC(spin_ms,
	"after onlining, run a bound kthread on the A72 for this many ms so it "
	"never reaches WFI; proves the core does real work and isolates idle");

static int cspm_cluster_on_after;
module_param(cspm_cluster_on_after, int, 0444);
MODULE_PARM_DESC(cspm_cluster_on_after,
	"after onlining, perform the vendor's cpuhvfs_notify_cluster_on(B): set "
	"CLUSTER_EN and clear SW_PAUSE on CSPM SW_RSV2 (needs a running PCM)");

static int mp2_sync_dcm = -1;
module_param(mp2_sync_dcm, int, 0444);
MODULE_PARM_DESC(mp2_sync_dcm,
	"before CPU_ON, drive MCUCFG_SYNC_DCM_MP2_CONFIG (0x10222274): 0 = DCM "
	"off (what Android has during power_on_cl3), 1 = on, -1 = leave alone");

static int clear_miscdbg;
module_param(clear_miscdbg, int, 0444);
MODULE_PARM_DESC(clear_miscdbg,
	"before CPU_ON, secure-read MCUCFG MISCDBG (0x1022220c) and clear its "
	"AXI quiesce bits 0|4 that ATF's power-off sets and its power-on never "
	"undoes; 2 also clears them in MP2_AXI_CONFIG (0x1022222c)");

static int probe_axi_from_watcher;
module_param(probe_axi_from_watcher, int, 0444);
MODULE_PARM_DESC(probe_axi_from_watcher,
	"after poll-5 cluster/core release, inspect MP2's non-secure AXI-config "
	"aliases and clear ACINACTM if either sub-block is finally accessible");

static int spmc_sw_top_on;
module_param(spmc_sw_top_on, int, 0444);
MODULE_PARM_DESC(spmc_sw_top_on,
	"run ATF source's bounded software SPMC TOP power sequence before CPU_ON; "
	"this directly drives SRAM/PRE1/PRE2/PDB rather than the broken HW FSM");

static void __iomem *spm, *mcumixed, *topckgen, *cspm, *csram, *infracfg, *cci, *mp2cfg;
static void __iomem *rgu;		/* mapped only for the pwrap latch */

#define RGU_PHYS		0x10007000u
#define WDT_SWSYSRST		0x018

#define CSPM_SW_RSV0		0x608
#define CSPM_SW_RSV2		0x610
#define CSPM_HWSTA_LL		0x614
#define CSPM_HWSTA_L		0x618
#define CSPM_HWSTA_B		0x61c
#define CPUHVFS_FW_DONE		BIT(15)
#define CSRAM_SW_RSV2		0x328
#define CSRAM_PAUSE_SRC		0x330
#define CPUHVFS_SW_PAUSE	BIT(13)
#define CPUHVFS_CLUSTER_EN	BIT(14)

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
static unsigned int meter_once_div(unsigned int src, unsigned int div)
{
	u32 dbg = readl(topckgen + CLK_DBG_CFG);
	u32 misc = readl(topckgen + CLK_MISC_CFG_0);
	unsigned int t;
	int i;

	writel((dbg & 0xFFC0FFFCu) | (src << 16), topckgen + CLK_DBG_CFG);
	writel((misc & 0x00FFFFFFu) | ((div & 0xffu) << 24),
	       topckgen + CLK_MISC_CFG_0);
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

static unsigned int meter_once(unsigned int src)
{
	return meter_once_div(src, 1);
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
	/* CPUHVFS hardware mode reloads CON1 from this PCW shadow. */
	writel(pcw & 0x3fffffu, mcumixed + ARMCAXPLL2_SHADOW);
	/* CON1 bit31 is the PCW-change trigger and self-clears after latching. */
	writel((con1 & ~0x3FFFFFu) | (pcw & 0x3FFFFFu) | BIT(31),
	       mcumixed + ARMCAXPLL2_CON1);
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

/*
 * Only ever release a semaphore this module actually took. CSPM_SEMA is a
 * write-1-to-toggle register, so an unconditional "give" after a failed "take"
 * ends SOMEBODY ELSE's critical section — and the thing it protects here is
 * ARMPLLDIV, the divider feeding the clusters this kernel is running on.
 */
static void cspm_sema_give(bool held)
{
	if (held)
		writel(1, cspm + CSPM_SEMA);
	else
		P("  not releasing the CSPM semaphore: we never had it");
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

/*
 * Read the A72 rail back from the DA9214 itself.
 *
 * The core stops executing while SPM still reports it powered — but SPM's
 * PWR_CON only reflects the SoC-internal MTCMOS switch, not the external buck
 * that actually feeds VPROC2. If BUCKB has been disabled or its voltage pulled
 * down (over-current protection is a real possibility: the vendor's
 * cpu_power_on_buck() brackets this whole sequence with an OCP reset_flag, and
 * ATF carries an mt_ocp_api driver we have never initialised), the core would
 * halt exactly like this: powered per SPM, clocked, not in WFI, not at EL3.
 *
 * The regulator framework caches its own idea of the voltage and will happily
 * report the value it last asked for, so read the chip (§2.8).
 */
static void vproc2_report(const char *when)
{
	struct i2c_adapter *ad = i2c_get_adapter(2);
	union i2c_smbus_data d;
	int cont = -1, vsel = -1, page;

	if (!ad) {
		P("  VPROC2 (%s): no i2c6 adapter", when);
		return;
	}
	page = i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_WRITE,
			      DA9214_PAGE_CON, I2C_SMBUS_BYTE_DATA, &d);
	if (!i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_READ,
			    DA9214_BUCKB_CONT, I2C_SMBUS_BYTE_DATA, &d))
		cont = d.byte;
	if (!i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_READ,
			    DA9214_VBUCKB_A, I2C_SMBUS_BYTE_DATA, &d))
		vsel = d.byte;
	i2c_put_adapter(ad);
	P("  VPROC2 (%s): BUCKB_CONT=0x%02x VBUCKB_A=0x%02x -> %d mV%s",
	  when, cont, vsel, vsel < 0 ? -1 : 300 + (vsel & 0x7f) * 10,
	  (cont >= 0 && !(cont & 1)) ? "   *** BUCKB IS OFF ***" : "");
	(void)page;
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
		u8 step;

		if (vproc2_mv < 600 || vproc2_mv > 1180) {
			P("  REFUSING VPROC2 = %d mV (outside 600..1180)", vproc2_mv);
			i2c_put_adapter(ad);
			return -EINVAL;
		}
		step = (vproc2_mv - 300) / 10;

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

	if (vproc2_already_on) {
		/*
		 * Stage 1 enabled and chip-read-verified this rail before CSPM was
		 * started. Once the PCM is unpaused it is another i2c6 master, and
		 * Linux has not yet ported the vendor's pause-around-transfer
		 * semaphore. Even a redundant DA9214 read can collide and fail.
		 */
		P("step 3: VPROC2 already on — NO i2c6 traffic while PCM owns the bus");
	} else {
		P("step 3: VPROC2 (DA9214 BUCKB) on");
		if (vproc2_set(true)) {
			P("  VPROC2 ENABLE FAILED — stopping, the rest is meaningless");
			return -EIO;
		}
		P("step 3b: letting the rail settle for %d ms", settle_ms);
		msleep(settle_ms);
	}

	/*
	 * The vendor latches the PMIC wrapper's SPI controller in reset across
	 * the external-buck isolation change:
	 *
	 *   mtk_wdt_swsysret_config(MTK_WDT_SWSYS_RST_PWRAP_SPI_CTL_RST, 1);
	 *   ... EXT_BUCK_ISO ...
	 *   mtk_wdt_swsysret_config(MTK_WDT_SWSYS_RST_PWRAP_SPI_CTL_RST, 0);
	 *
	 * (psci.c:cpu_power_on_buck, and identically in ATF's own compiled-out
	 * power_on_cl3 prologue). That bit is 0x800 in WDT_SWSYSRST 0x10007018,
	 * key 0x88000000. It is the last line of the authoritative prerequisite
	 * we had never implemented. The WDT_SWSYSRST bit map has no big-cluster
	 * reset bit, so this is an interlock and not the missing target-side
	 * reset — but "run the vendor sequence exactly" is the rule here.
	 */
	if (pwrap_latch) {
		rgu = ioremap(RGU_PHYS, 0x100);
		if (rgu) {
			u32 w = readl(rgu + WDT_SWSYSRST);

			P("step 3c: WDT_SWSYSRST %08x -> latch PWRAP_SPI_CTL_RST", w);
			writel(w | 0x88000800u, rgu + WDT_SWSYSRST);
		} else {
			P("step 3c: cannot map the RGU — skipping the pwrap latch");
		}
	}

	P("step 4: CPU_EXT_BUCK_ISO %08x -> clear bits[1:0]",
	  readl(spm + CPU_EXT_BUCK_ISO));
	writel(readl(spm + CPU_EXT_BUCK_ISO) & ~0x3u, spm + CPU_EXT_BUCK_ISO);

	if (pwrap_latch && rgu) {
		u32 w = readl(rgu + WDT_SWSYSRST);

		writel((w & ~0x800u) | 0x88000000u, rgu + WDT_SWSYSRST);
		P("step 4b: WDT_SWSYSRST unlatched -> %08x",
		  readl(rgu + WDT_SWSYSRST));
		iounmap(rgu);
		rgu = NULL;
	}

	udelay(240);
	P("  CPU_EXT_BUCK_ISO now %08x", readl(spm + CPU_EXT_BUCK_ISO));

	before = sread(0x102222b0);
	arm_smccc_smc(SIP_IDVFS_SRAMLDOSET, 110000, 0, 0, 0, 0, 0, 0, &res);
	udelay(240);
	after = sread(0x102222b0);
	P("step 5: BigiDVFSSRAMLDOSet(110000) -> %ld, 0x102222b0 %08x -> %08x",
	  (long)res.a0, before, after);

	/*
	 * The SPMC's own state BEFORE anything asserts PWR_ON. Recorded because
	 * the ack is not reliable across boots: 0x102222a0 has been seen to
	 * latch at 0x042001xx (2026-08-22 early) and at 0x0e500100 (late), and
	 * neither clears through ATF's isolation-cycling retry. Reading it at
	 * rest is what separates "the SoC came up in a bad state" from "our
	 * power-on put it there" — and nothing had ever read it at rest.
	 */
	P("step 6: SPMC at rest, before any PWR_ON");
	P("  0x102222a0 = %08x   (ATF polls bit17 here; good boots show 0x004001xx)",
	  sread(0x102222a0));
	P("  0x102222b0 = %08x  0x102222b4 = %08x  0x102224a0 = %08x",
	  sread(0x102222b0), sread(0x102222b4), sread(0x102224a0));
	P("  0x10222430 = %08x  0x10222434 = %08x  0x10222700 = %08x",
	  sread(0x10222430), sread(0x10222434), sread(0x10222700));
	/*
	 * MISCDBG and MP2_AXI_CONFIG, read through ATF rather than non-secure.
	 * ATF's power_off_cl3 leaves MISCDBG bits 0|4 set to quiesce the MP2
	 * interface; its power_on_cl3 clears only bit 0, and only after the
	 * protection release that poll 5 hangs on. Whether those bits are
	 * actually set here decides whether that is our wall.
	 */
	P("  0x1022220c = %08x (MISCDBG, ATF sets bits 0|4 to quiesce MP2 AXI)",
	  sread(0x1022220c));
	P("  0x1022222c = %08x (MP2_AXI_CONFIG)", sread(0x1022222c));
	P("  0x10222274 = %08x (MP2 sync DCM; bit0 = bus-clock gating enabled)",
	  sread(0x10222274));
	P("  INFRACFG PROTECTEN1 = %08x  PROTEXTSTA3 = %08x",
	  readl(infracfg + TOPAXI_PROTECTEN), readl(infracfg + TOPAXI_PROTECTEN_STA1));

	spm_report("after prerequisites");
	return 0;
}

/* ---- ATF's software SPMC TOP sequence, with every source poll bounded -- */

#define SPMC_TOP		0x102222a0u
#define SPMC_FSM_MASK		(0x1fu << 23)
#define SPMC_FSM_ON		BIT(23)
#define SPMC_PDB_ACK		BIT(21)
#define SPMC_PRE2_ACK		BIT(20)
#define SPMC_PRE1_ACK		BIT(19)
#define SPMC_HOT_RESET		BIT(16)
#define SPMC_SRAMPD_MASK	(0x3fu << 10)
#define SPMC_SRAM_ISOINTB	BIT(9)
#define SPMC_ISO		BIT(8)
#define SPMC_SRAM_SLEEPB	BIT(7)
#define SPMC_LOGIC_PDB		BIT(6)
#define SPMC_PRE2_PDB		BIT(5)
#define SPMC_PRE1_PDB		BIT(4)
#define SPMC_FSM_OVERRIDE	BIT(3)
#define SPMC_SW_PWR_ON		BIT(2)
#define SPMC_PWR_OVERRIDE_EN	BIT(1)

static bool spmc_wait(u32 mask, bool set, const char *what)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (!!(sread(SPMC_TOP) & mask) == set) {
			P("  SPMC %-18s %s after %d us (= %08x)", what,
			  set ? "SET" : "CLEAR", i, sread(SPMC_TOP));
			return true;
		}
		udelay(1);
	}
	P("  SPMC %-18s TIMEOUT (= %08x)", what, sread(SPMC_TOP));
	return false;
}

static bool spmc_software_top_on(void)
{
	static const u8 gray[32] = {
		0x01, 0x03, 0x02, 0x06, 0x07, 0x05, 0x04, 0x0c,
		0x0d, 0x0f, 0x0e, 0x0a, 0x0b, 0x09, 0x08, 0x18,
		0x19, 0x1b, 0x1a, 0x1e, 0x1f, 0x1d, 0x1c, 0x14,
		0x15, 0x17, 0x16, 0x12, 0x13, 0x11, 0x10, 0x30,
	};
	u32 v, orig, pll0, mux0, div0;
	bool held;
	int i;

	if (!spmc_sw_top_on)
		return true;
	P("==== ATF-source software SPMC TOP power-on ====");
	v = sread(SPMC_TOP);
	P("  entry TOP_SPMC=%08x MP2=%08x STATUS=%08x/%08x", v,
	  readl(spm + MP2_CPUSYS_PWR_CON), readl(spm + CPU_PWR_STATUS),
	  readl(spm + CPU_PWR_STATUS_2ND));
	if (v & SPMC_PDB_ACK) {
		P("  SPMC TOP already has PDB all-on ACK");
		goto clock_and_bus;
	}
	if ((v & SPMC_FSM_MASK) || (v & SPMC_FSM_OVERRIDE)) {
		P("  REFUSING software SPMC takeover from non-OFF/non-HW state");
		return false;
	}

	v &= ~(SPMC_SRAMPD_MASK | SPMC_LOGIC_PDB | SPMC_PRE1_PDB |
	       SPMC_PRE2_PDB | SPMC_SRAM_SLEEPB | SPMC_SRAM_ISOINTB);
	v |= SPMC_ISO | SPMC_HOT_RESET;
	swrite(SPMC_TOP, v);
	swrite(SPMC_TOP, v | SPMC_FSM_OVERRIDE);
	if (!spmc_wait(SPMC_FSM_MASK, true, "FSM override"))
		return false;

	v = sread(SPMC_TOP) | SPMC_HOT_RESET | SPMC_PWR_OVERRIDE_EN |
	    SPMC_SW_PWR_ON;
	swrite(SPMC_TOP, v);
	writel(readl(spm + MP2_CPUSYS_PWR_CON) & ~PWR_CLK_DIS,
	       spm + MP2_CPUSYS_PWR_CON);
	writel(readl(spm + MP2_CPUSYS_PWR_CON) | PWR_RST_B,
	       spm + MP2_CPUSYS_PWR_CON);

	/* Source uses & 0xFFFF03FF; only low control bits feed each gray write. */
	orig = sread(SPMC_TOP) & 0x000003ffu;
	for (i = 0; i < ARRAY_SIZE(gray); i++)
		swrite(SPMC_TOP, orig | ((u32)gray[i] << 10));

	v = sread(SPMC_TOP) | SPMC_PRE1_PDB;
	swrite(SPMC_TOP, v);
	if (!spmc_wait(SPMC_PRE1_ACK, true, "PRE1 ACK"))
		return false;
	v = sread(SPMC_TOP) | SPMC_PRE2_PDB;
	swrite(SPMC_TOP, v);
	if (!spmc_wait(SPMC_PRE2_ACK, true, "PRE2 ACK"))
		return false;
	v = sread(SPMC_TOP) | SPMC_LOGIC_PDB;
	swrite(SPMC_TOP, v);
	if (!spmc_wait(SPMC_PDB_ACK, true, "PDB ACK"))
		return false;
	v = sread(SPMC_TOP) | SPMC_SRAM_SLEEPB;
	swrite(SPMC_TOP, v);
	v = (sread(SPMC_TOP) & ~SPMC_ISO) | SPMC_SRAM_ISOINTB;
	swrite(SPMC_TOP, v);
	v = sread(SPMC_TOP) & ~SPMC_HOT_RESET;
	swrite(SPMC_TOP, v);
	udelay(2);

clock_and_bus:
	/*
	 * Hand the fully sequenced controls back to the shipping HW FSM before
	 * presenting SPM PWR_ON. Pure SW override reached every internal ACK but
	 * never asserted CPU_PWR_STATUS on this production configuration.
	 */
	v = sread(SPMC_TOP);
	if (v & SPMC_FSM_OVERRIDE) {
		v &= ~(SPMC_FSM_OVERRIDE | SPMC_PWR_OVERRIDE_EN | SPMC_SW_PWR_ON);
		swrite(SPMC_TOP, v);
		udelay(10);
		P("  SPMC handoff to HW FSM: %08x", sread(SPMC_TOP));
	}
	writel(readl(spm + MP2_CPUSYS_PWR_CON) | PWR_ON,
	       spm + MP2_CPUSYS_PWR_CON);
	for (i = 0; i < 2000 &&
	     !(readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT); i++)
		udelay(1);
	P("  software TOP physical request: MP2=%08x STATUS=%08x wait=%d us",
	  readl(spm + MP2_CPUSYS_PWR_CON), readl(spm + CPU_PWR_STATUS), i);
	if (i == 2000)
		P("  CPU_PWR_STATUS did not mirror SW override; continuing with explicit SPM shadows");
	v = readl(spm + MP2_CPUSYS_PWR_CON) | PWR_ON | PWR_ON_2ND |
	    SRAM_ISOINT_B | PWR_RST_B;
	v &= ~(PWR_ISO | SRAM_PDN | SRAM_CKISO | PWR_CLK_DIS);
	writel(v, spm + MP2_CPUSYS_PWR_CON);
	udelay(10);
	P("  software TOP SPM shadow release: MP2=%08x", readl(spm + MP2_CPUSYS_PWR_CON));

	/* big_sw_on_seq() clocks the live TOP before withdrawing pwrdnreqn. */
	pll0 = sread(0x102224a0);
	mux0 = readl(mcumixed + ARMPLLDIV_MUXSEL);
	div0 = readl(mcumixed + ARMPLLDIV_CKDIV);
	swrite(0x102224a0, 0x00ff0100);
	udelay(20);
	swrite(0x102224a0, 0x00ff0101);
	udelay(10);
	swrite(0x102224a0, 0x00ff1101);
	udelay(30);
	held = cspm_sema_take();
	writel((mux0 & ~0x3u) | 1u, mcumixed + ARMPLLDIV_MUXSEL);
	writel((div0 & ~0x1fu) | 8u, mcumixed + ARMPLLDIV_CKDIV);
	cspm_sema_give(held);
	udelay(20);

	v = readl(infracfg + TOPAXI_PROTECTEN);
	writel(v | MP2_BUS_PROT_MASK, infracfg + TOPAXI_PROTECTEN);
	for (i = 0; i < 2000 &&
	     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK) !=
	     MP2_BUS_PROT_MASK; i++)
		udelay(1);
	P("  software TOP protect asserted: EN=%08x STA1=%08x wait=%d us",
	  readl(infracfg + TOPAXI_PROTECTEN),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);

	v = readl(infracfg + TOPAXI_PROTECTEN);
	writel(v & ~BIT(10), infracfg + TOPAXI_PROTECTEN);
	for (i = 0; i < 2000 &&
	     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & BIT(10)); i++)
		udelay(1);
	P("  software TOP bit10 release: EN=%08x STA1=%08x wait=%d us",
	  readl(infracfg + TOPAXI_PROTECTEN),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);
	if (i < 2000) {
		writel(readl(infracfg + TOPAXI_PROTECTEN) & ~BIT(6),
		       infracfg + TOPAXI_PROTECTEN);
		for (i = 0; i < 2000 &&
		     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & BIT(6)); i++)
			udelay(1);
		writel(readl(infracfg + TOPAXI_PROTECTEN) & ~BIT(2),
		       infracfg + TOPAXI_PROTECTEN);
		for (i = 0; i < 2000 &&
		     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & BIT(2)); i++)
			udelay(1);
	}
	P("  software TOP on complete: SPMC=%08x MP2=%08x STA1=%08x final wait=%d us",
	  sread(SPMC_TOP), readl(spm + MP2_CPUSYS_PWR_CON),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);

	held = cspm_sema_take();
	writel(mux0, mcumixed + ARMPLLDIV_MUXSEL);
	writel(div0, mcumixed + ARMPLLDIV_CKDIV);
	cspm_sema_give(held);
	swrite(0x102224a0, pll0);
	udelay(100);
	P("  software TOP clock restored: PLL=%08x MUX=%08x DIV=%08x",
	  sread(0x102224a0), readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV));
	return i < 2000;
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
		udelay(1);
		if (full_cputop_mtcmos)
			writel(readl(r) | PWR_ON_2ND, r);
		udelay(1);
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

		writel(readl(r) & ~(PWR_ON | PWR_ON_2ND), r);
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

	if (full_cputop_mtcmos) {
		for (i = 0; i < 20000; i++) {
			if ((readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) &&
			    (readl(spm + CPU_PWR_STATUS_2ND) & MP2_CPUTOP_BIT))
				break;
			udelay(1);
		}
		writel(readl(r) & ~PWR_ISO, r);
		writel(readl(r) & ~SRAM_PDN, r);
		for (i = 0; i < 2000 && (readl(r) & SRAM_PDN_ACK); i++)
			udelay(1);
		writel(readl(r) & ~SRAM_SLEEP_B, r);
		udelay(1);
		writel(readl(r) | SRAM_SLEEP_B, r);
		for (i = 0; i < 2000 && !(readl(r) & SRAM_SLEEP_B_ACK); i++)
			udelay(1);
		P("  full MTCMOS memory wake: MP2=%08x SLEEP_ACK wait=%d us", readl(r), i);
		ndelay(900);
		writel(readl(r) | SRAM_ISOINT_B, r);
		ndelay(100);
		writel(readl(r) & ~SRAM_CKISO, r);
		writel(readl(r) & ~PWR_CLK_DIS, r);
		writel(readl(r) | PWR_RST_B, r);
	}

	spm_report("after CPUTOP power-on");
	/*
	 * Re-read the two registers that decide the MISCDBG-quiesce and
	 * sync-DCM hypotheses. Reading them with the cluster unpowered is
	 * worthless: MCUCFG's cluster-domain registers answer 0 when the domain
	 * is down (the same trap that made 0x102224xx look all-zero), so a
	 * clear bit there proves nothing. Powered, this is the real value.
	 */
	P("  --- MCUCFG cluster-domain registers, now that MP2 is powered ---");
	P("  0x1022220c = %08x (MISCDBG: bit0|bit4 = MP2 AXI quiesce)", sread(0x1022220c));
	P("  0x1022222c = %08x (MP2_AXI_CONFIG)", sread(0x1022222c));
	P("  0x10222274 = %08x (MP2 sync DCM: bit0 = bus-clock gating)", sread(0x10222274));
	P("  PROTECTEN1 = %08x  PROTEXTSTA3 = %08x",
	  readl(infracfg + TOPAXI_PROTECTEN),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
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
static void atf_core_poll_rehearsal(int cpu);
static u32 atf_bus_rehearsal(void);

/*
 * Stage 5's whole payload, run at ATF's own decision point: the bus release and
 * (optionally) the CCI snoop enable that follow the frequency check, then the
 * two core polls power_on_big() does. Everything bounded, everything restored.
 */
static void atf_tail_rehearsal(int cpu)
{
	/*
	 * Order matters and so does the restore point. In ATF the core polls
	 * run with MP2's bus protection ALREADY dropped, so atf_bus_rehearsal()
	 * leaves it dropped and hands back the saved value for the caller to
	 * restore after the core polls. Restoring it in between — which is what
	 * this did first — measures a machine ATF never presents, and the
	 * verdict it prints would be worth nothing.
	 */
	u32 prot0 = atf_bus_rehearsal();

	atf_core_poll_rehearsal(cpu);
	P("  restoring TOPAXI_PROTECTEN %08x", prot0);
	writel(prot0, infracfg + TOPAXI_PROTECTEN);
	udelay(100);
	bus_report("after the tail rehearsal");
}

static void prime_atf_meter_latch(int unused)
{
	unsigned int first, second;

	/*
	 * tee.img's meter reads CLK26CALI_1 once. This silicon exposes the
	 * previous run's count there; meter() normally hides that by taking two
	 * samples. Take two source-37 samples while the rehearsed clock is live
	 * and, critically, do not touch CLK26CALI again before the SMC.
	 */
	first = meter_once(37);
	second = meter_once(37);
	P("  primed ATF's lagged meter: source37 first=%u second=%u kHz; "
	  "leaving the 750 MHz count latched", first, second);
}

static void atf_clock_rehearsal(void (*with_clock_applied)(int), int arg)
{
	u32 pll0, muxsel0, ckdiv0, k1_0, mon0;
	bool held;

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

	held = cspm_sema_take();
	writel(readl(mcumixed + ARMPLLDIV_MUXSEL) | 1, mcumixed + ARMPLLDIV_MUXSEL);
	udelay(1);
	writel((readl(mcumixed + ARMPLLDIV_CKDIV) & ~0x1fu) | 8,
	       mcumixed + ARMPLLDIV_CKDIV);
	udelay(1);
	cspm_sema_give(held);

	held = cspm_sema_take();
	writel(0, mcumixed + ARMPLLDIV_ARM_K1);
	writel(0xffffffff, mcumixed + ARMPLLDIV_MON_EN);
	udelay(1);
	cspm_sema_give(held);

	P("  MUXSEL=%08x CKDIV=%08x", readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV));
	meter_report("at ATF's decision point");

	/*
	 * Anything that has to be measured with ATF's clock configuration in
	 * place goes HERE, not after the restore — power_on_big() runs with
	 * whatever power_on_cl3() left behind, so measuring the core polls
	 * against the pre-ATF mux and divider would be measuring the wrong
	 * machine.
	 */
	if (with_clock_applied)
		with_clock_applied(arg);

	P("  putting it back so ATF's entry assertions hold");
	held = cspm_sema_take();
	writel(muxsel0, mcumixed + ARMPLLDIV_MUXSEL);
	writel(ckdiv0, mcumixed + ARMPLLDIV_CKDIV);
	writel(k1_0, mcumixed + ARMPLLDIV_ARM_K1);
	writel(mon0, mcumixed + ARMPLLDIV_MON_EN);
	cspm_sema_give(held);
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

/* ---- polls 5 and 6: the bus release and the CCI snoop enable ---------- */

/*
 * The tail of power_on_cl3(), after the frequency check passes. These two are
 * the leading suspects for the spin seen on 2026-08-22, precisely because they
 * sit *after* the gate that was closed until ARMCAXPLL2 was moved — nothing had
 * ever reached them.
 *
 * The bus release is low risk: dropping MP2's TOPAXI protection for a domain
 * that is powered is what the SoC's own power-domain code does routinely, and
 * it is restored here.
 *
 * The CCI half is NOT low risk and is opt-in. Enabling snoop and DVM on a
 * slave interface whose master cannot answer is a way to stall the
 * interconnect for every other master on it — which is a whole-SoC hang, not a
 * lost CPU. Run it only with the dead-man reset armed.
 */
/*
 * Android's actual arrangement, and the one combination nothing has run: the
 * PCM running with cluster B enabled and paused (tools/cspm-probe stage 3),
 * then PSCI CPU_ON, with ATF doing the whole power-on itself.
 *
 * Our own cputop_power_on() must be skipped for that. B-40 FINAL+2 measured
 * that once the PCM runs it owns the SPMC and a manual PWR_ON is refused
 * outright — 0x102222a0 reads 0x04200120 from the first attempt and bit 17
 * never comes — so pre-satisfying ATF's polls is not available here, and ATF's
 * two unbounded power-good polls are live again. That is what the ring watcher
 * and the runner's dead-man reset are for.
 *
 * It also means ATF's five assertions cannot be checked first: the 0x102224xx
 * block is dark until the cluster is powered, and now nothing powers it before
 * the SMC. The ARMCAXPLL2 gate still applies and is still checked.
 */
static int skip_cputop;
module_param(skip_cputop, int, 0444);
MODULE_PARM_DESC(skip_cputop,
	"do not power the CPUTOP ourselves; let ATF do it inside CPU_ON. "
	"Required when the PCM is running, because it owns the SPMC.");

static int rehearse_cci;
module_param(rehearse_cci, int, 0444);
MODULE_PARM_DESC(rehearse_cci,
	"also rehearse ATF's CCI snoop enable for cluster 2 (can stall the "
	"interconnect — arm the dead-man reset first)");

static u32 atf_bus_rehearsal(void)
{
	u32 prot0 = readl(infracfg + TOPAXI_PROTECTEN);
	bool ok;
	int i;

	P("==== rehearsing power_on_cl3's bus release (poll 5) ====");
	bus_report("before");
	writel(prot0 & ~MP2_BUS_PROT_MASK, infracfg + TOPAXI_PROTECTEN);
	for (i = 0; i < 20000; i++) {
		if (!(readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK))
			break;
		udelay(1);
	}
	ok = !(readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK);
	P("  STA1 & 0x444 %s after %d us (=%08x)",
	  ok ? "cleared" : "*** TIMEOUT — ATF WOULD SPIN HERE ***", i,
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	if (!rehearse_cci) {
		P("==== poll 6 (CCI snoop enable) NOT rehearsed: pass "
		  "rehearse_cci=1 with the dead-man reset armed ====");
		return prot0;
	}

	P("==== rehearsing power_on_cl3's CCI snoop enable (poll 6) ====");
	{
		u32 snoop0 = readl(cci + CCI_MP2_SNOOP);
		u32 want = snoop0 | 3u | (3u << 4);	/* cluster 2 -> (2 + 1) << 4 */

		P("  MP2 slave interface %08x -> %08x", snoop0, want);
		writel(want, cci + CCI_MP2_SNOOP);
		for (i = 0; i < 20000; i++) {
			if (!(readl(cci + CCI_STATUS) & 1))
				break;
			udelay(1);
		}
		P("  CCI STATUS change-pending %s after %d us (=%08x)",
		  (readl(cci + CCI_STATUS) & 1) ?
			"*** TIMEOUT — ATF WOULD SPIN HERE ***" : "cleared",
		  i, readl(cci + CCI_STATUS));
		P("  restoring the slave interface to %08x", snoop0);
		writel(snoop0, cci + CCI_MP2_SNOOP);
		for (i = 0; i < 20000 && (readl(cci + CCI_STATUS) & 1); i++)
			udelay(1);
		bus_report("after");
	}
	return prot0;
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
/*
 * The watcher kthread on cpu0 and the SMC thread on smc_cpu both dump the
 * ring. Without this they interleave into one shared line buffer and both
 * advance atf_seen, so ATF's narration arrives garbled with whole ranges
 * dropped — at the one moment it is the only evidence there is.
 */
static DEFINE_SPINLOCK(atf_ring_lock);

static void atf_ring_dump(const char *when)
{
	u32 base, size, wptr, from, i, n;
	static char line[240];
	unsigned int col = 0;
	unsigned long flags;

	if (!atf_ring)
		return;
	spin_lock_irqsave(&atf_ring_lock, flags);
	base = readl(atf_ring + 0x00);
	size = readl(atf_ring + 0x04);
	wptr = readl(atf_ring + 0x08);
	if (base != ATF_RING_PHYS + 0x100 || size > ATF_RING_MAP - 0x100 ||
	    wptr < base || wptr > base + size) {
		P("  ATF ring header looks wrong: base=%08x size=%08x wptr=%08x",
		  base, size, wptr);
		spin_unlock_irqrestore(&atf_ring_lock, flags);
		return;
	}
	n = wptr - base;
	if (!atf_seen)
		atf_seen = n > 0x600 ? n - 0x600 : 0;	/* first dump: recent tail */
	if (n <= atf_seen) {
		P("  ATF ring (%s): nothing new (%u bytes total)", when, n);
		spin_unlock_irqrestore(&atf_ring_lock, flags);
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
	spin_unlock_irqrestore(&atf_ring_lock, flags);
}

static struct task_struct *ringwatch;
static DECLARE_COMPLETION(ringwatch_started);

/*
 * The PCM overwrites B's divider while ATF is in power_on_cl3's frequency
 * retry. ATF re-measures on every iteration, so create a wide causal window
 * in which its own mux/divider values win. This touches only non-secure
 * MCUMIXED registers; in particular it does NOT invoke an SMC or touch the
 * abist registers ATF is using concurrently.
 */
static void ringwatch_force_clock(void)
{
	u32 contested = 0;
	int i;

	if (!force_clock_from_watcher)
		return;

	P("==== watcher clock intervention: forcing B MUXSEL=1 CKDIV=8 for 2 s ====");
	P("  before force: MUXSEL=%08x CKDIV=%08x ARMCAXPLL2_CON1=%08x (%u kHz)",
	  readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV),
	  readl(mcumixed + ARMCAXPLL2_CON1),
	  pll_khz(readl(mcumixed + ARMCAXPLL2_CON1)));

	for (i = 0; i < 1000000 && !kthread_should_stop(); i++) {
		u32 mux = readl(mcumixed + ARMPLLDIV_MUXSEL);
		u32 div = readl(mcumixed + ARMPLLDIV_CKDIV);

		/*
		 * Do not rewrite an already-correct clock. The first version did
		 * two writels every 5 us and ATF measured a ramp of 0..17 MHz:
		 * the intervention itself was glitching the clock throughout the
		 * meter's integration window. Correct only an observed takeover.
		 */
		if ((mux & 0x3) != 1) {
			writel((mux & ~0x3u) | 1u, mcumixed + ARMPLLDIV_MUXSEL);
			contested++;
		}
		if ((div & 0x1f) != 8) {
			writel((div & ~0x1fu) | 8u, mcumixed + ARMPLLDIV_CKDIV);
			contested++;
		}
		udelay(2);
		if (!(i % 5000))
			cond_resched();
	}
	P("  after force: MUXSEL=%08x CKDIV=%08x; corrective writes=%u over %d polls",
	  readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV), contested, i);
}

static void ringwatch_release_mp2_sram(void)
{
	void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
	u32 sta1;
	int i;

	if (!release_mp2_sram_from_watcher)
		return;

	sta1 = readl(infracfg + TOPAXI_PROTECTEN_STA1);
	P("==== watcher MP2 SRAM release: MP2=%08x STATUS=%08x/%08x STA1=%08x ====",
	  readl(r), readl(spm + CPU_PWR_STATUS),
	  readl(spm + CPU_PWR_STATUS_2ND), sta1);
	if (!(readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) ||
	    !(sta1 & 0x444u) ||
	    (readl(mcumixed + ARMPLLDIV_MUXSEL) & 0x3u) != 1u) {
		P("  REFUSING intervention: not uniquely at post-frequency poll 5");
		return;
	}

	/* mt_spm_mtcmos.c's power-on order, using MP2's integrated SRAM bits. */
	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);
	writel(readl(r) | PWR_ON_2ND, r);
	for (i = 0; i < 2000; i++) {
		if ((readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) &&
		    (readl(spm + CPU_PWR_STATUS_2ND) & MP2_CPUTOP_BIT))
			break;
		udelay(1);
	}
	P("  PWR_ON_2ND: MP2=%08x STATUS=%08x/%08x wait=%d us",
	  readl(r), readl(spm + CPU_PWR_STATUS),
	  readl(spm + CPU_PWR_STATUS_2ND), i);

	writel(readl(r) & ~PWR_ISO, r);
	writel(readl(r) & ~SRAM_PDN, r);
	for (i = 0; i < 2000 && (readl(r) & SRAM_PDN_ACK); i++)
		udelay(1);

	/*
	 * A live MP0 is 0x0009004d: both SRAM_SLEEP_B and its bit-19 ACK are
	 * set. MP2 reaches only 0x0001004d unless the active-low sleep request
	 * receives a real wake edge. This is ATF's "memory power ready" step.
	 */
	writel(readl(r) & ~SRAM_SLEEP_B, r);
	for (i = 0; i < 2000 && (readl(r) & SRAM_SLEEP_B_ACK); i++)
		udelay(1);
	writel(readl(r) | SRAM_SLEEP_B, r);
	for (i = 0; i < 2000 && !(readl(r) & SRAM_SLEEP_B_ACK); i++)
		udelay(1);
	P("  SRAM_SLEEP_B wake: MP2=%08x ACK wait=%d us", readl(r), i);

	ndelay(900);
	writel(readl(r) | SRAM_ISOINT_B, r);
	ndelay(100);
	writel(readl(r) & ~SRAM_CKISO, r);
	writel(readl(r) & ~PWR_CLK_DIS, r);
	writel(readl(r) | PWR_RST_B, r);
	udelay(10);
	P("  release complete: MP2=%08x STA1=%08x SRAM_ACK wait=%d us",
	  readl(r), readl(infracfg + TOPAXI_PROTECTEN_STA1), i);

	/*
	 * ATF cleared PROTECTEN before the SRAM/isolation handshake completed.
	 * If STA1 retained bit 0x400, generate a fresh deassert edge now that
	 * the target is live; merely rewriting an already-clear PROTECTEN did
	 * not make the hardware reevaluate the stale acknowledgement.
	 */
	if (readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK) {
		u32 prot = readl(infracfg + TOPAXI_PROTECTEN);

		P("  retriggering all MP2 bus-protect bits: PROTECTEN=%08x STA1=%08x",
		  prot, readl(infracfg + TOPAXI_PROTECTEN_STA1));
		writel(prot | MP2_BUS_PROT_MASK, infracfg + TOPAXI_PROTECTEN);
		for (i = 0; i < 2000 &&
		     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK) !=
		     MP2_BUS_PROT_MASK; i++)
			udelay(1);
		P("  protect asserted: PROTECTEN=%08x STA1=%08x wait=%d us",
		  readl(infracfg + TOPAXI_PROTECTEN),
		  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);
		udelay(1000);
		writel(prot & ~MP2_BUS_PROT_MASK, infracfg + TOPAXI_PROTECTEN);
		for (i = 0; i < 2000 &&
		     (readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK); i++)
			udelay(1);
		P("  bus-protect retrigger: PROTECTEN=%08x STA1=%08x wait=%d us",
		  readl(infracfg + TOPAXI_PROTECTEN),
		  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);
	}
}

static void ringwatch_handoff_b_pcm(void)
{
	u32 b;
	int i;

	if (!handoff_b_pcm_from_watcher)
		return;

	b = readl(cspm + CSPM_SW_RSV2);
	P("==== watcher CPUHVFS handoff: B starts at SW_RSV2=%08x ====", b);
	if ((b & (CPUHVFS_CLUSTER_EN | CPUHVFS_SW_PAUSE)) != CPUHVFS_SW_PAUSE) {
		P("  REFUSING handoff: B is not disabled+paused");
		return;
	}
	P("  B disabled+paused; waiting boundedly for ATF clock stage");

	for (i = 0; i < 2000 && !kthread_should_stop(); i++) {
		if ((readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) &&
		    (readl(mcumixed + ARMPLLDIV_MUXSEL) & 0x3u) == 1u)
			break;
		usleep_range(900, 1100);
	}
	P("  handoff wait=%d ms MP2=%08x MUXSEL=%08x STA1=%08x", i,
	  readl(spm + MP2_CPUSYS_PWR_CON),
	  readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	if (i == 2000)
		return;

	{
		static const unsigned int pulse_us[] = { 450 };
		unsigned int p;

		/* Avoid two clock owners fighting while CPUHVFS processes cluster-on. */
		writel(readl(mcumixed + ARMPLLDIV_MUXSEL) & ~0x3u,
		       mcumixed + ARMPLLDIV_MUXSEL);
		udelay(10);
		P("  parked B on 26 MHz before CPUHVFS pulse: MUXSEL=%08x CKDIV=%08x",
		  readl(mcumixed + ARMPLLDIV_MUXSEL),
		  readl(mcumixed + ARMPLLDIV_CKDIV));

		for (p = 0; p < ARRAY_SIZE(pulse_us); p++) {
			writel(0, csram + CSRAM_PAUSE_SRC);
			b = (readl(cspm + CSPM_SW_RSV2) | CPUHVFS_CLUSTER_EN) &
			    ~CPUHVFS_SW_PAUSE;
			writel(b, cspm + CSPM_SW_RSV2);
			writel(b, csram + CSRAM_SW_RSV2);
			udelay(pulse_us[p]);
			if (stop_pcm_from_watcher && p == ARRAY_SIZE(pulse_us) - 1) {
				/* Capture the pre-reset edge: stop PCM before printk or re-pause. */
				writel(CSPM_KEY, cspm + CSPM_POWERON_CONFIG);
				writel(CSPM_CON0_KEY | CSPM_PCM_SW_RESET,
				       cspm + CSPM_PCM_CON0);
				writel(CSPM_CON0_KEY, cspm + CSPM_PCM_CON0);
				stop_pcm_from_watcher = 0;
				if (armpll2_khz)
					armpll2_set(armpll2_khz);
			}
			b |= CPUHVFS_SW_PAUSE;
			writel(b, cspm + CSPM_SW_RSV2);
			writel(b, csram + CSRAM_SW_RSV2);
			writel((readl(cspm + CSPM_SW_RSV0) & CPUHVFS_SW_PAUSE) ? BIT(0) : 0,
			       csram + CSRAM_PAUSE_SRC);
			udelay(10);
			P("  B pulse %u us: SW_RSV2=%08x MP2=%08x STA1=%08x HWSTA_B=%08x",
			  pulse_us[p], b, readl(spm + MP2_CPUSYS_PWR_CON),
			  readl(infracfg + TOPAXI_PROTECTEN_STA1),
			  readl(cspm + CSPM_HWSTA_B));
			if (!(readl(infracfg + TOPAXI_PROTECTEN_STA1) & MP2_BUS_PROT_MASK))
				break;
		}
	}
}

static void ringwatch_enable_cci(void)
{
	u32 snoop;
	int i;

	if (!enable_cci_from_watcher)
		return;
	P("==== watcher CCI intervention: MP2 snoop=%08x STATUS=%08x STA1=%08x ====",
	  readl(cci + CCI_MP2_SNOOP), readl(cci + CCI_STATUS),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	if (!(readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT) ||
	    (readl(mcumixed + ARMPLLDIV_MUXSEL) & 0x3u) != 1u) {
		P("  REFUSING CCI intervention: ATF is not at the post-frequency stage");
		return;
	}
	snoop = readl(cci + CCI_MP2_SNOOP);
	writel(snoop | 0x3u, cci + CCI_MP2_SNOOP);
	for (i = 0; i < 2000 && (readl(cci + CCI_STATUS) & 1u); i++)
		udelay(1);
	P("  CCI enabled: MP2 snoop=%08x STATUS=%08x STA1=%08x wait=%d us",
	  readl(cci + CCI_MP2_SNOOP), readl(cci + CCI_STATUS),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);
}

static void ringwatch_power_core(void)
{
	void __iomem *r;
	u32 bit;
	int i;

	if (!power_core_from_watcher)
		return;
	r = spm + MP2_CPU0_PWR_CON + (cpu_target - 8) * 4;
	bit = BIT(15 - cpu_target);
	P("==== watcher core intervention: cpu%d PWR_CON=%08x STATUS=%08x STA1=%08x ====",
	  cpu_target, readl(r), readl(spm + CPU_PWR_STATUS),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	writel(readl(r) | PWR_ON, r);
	for (i = 0; i < 2000 && !(readl(spm + CPU_PWR_STATUS) & bit); i++)
		udelay(1);
	P("  core PWR_ON: PWR_CON=%08x STATUS=%08x STA1=%08x wait=%d us",
	  readl(r), readl(spm + CPU_PWR_STATUS),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1), i);

	writel(readl(r) | PWR_ON_2ND, r);
	for (i = 0; i < 2000 && !(readl(spm + CPU_PWR_STATUS_2ND) & bit); i++)
		udelay(1);
	writel(readl(r) & ~BIT(9), r); /* PD_SLPB_CLAMP */
	writel(readl(r) & ~SRAM_PDN, r);
	for (i = 0; i < 2000 && (readl(r) & SRAM_PDN_ACK); i++)
		udelay(1);
	writel(readl(r) | SRAM_SLEEP_B, r);
	writel(readl(r) | SRAM_ISOINT_B, r);
	writel(readl(r) & ~PWR_ISO, r);
	udelay(1);
	writel(readl(r) & ~SRAM_CKISO, r);
	writel(readl(r) & ~PWR_CLK_DIS, r);
	writel(readl(r) | PWR_RST_B, r);
	udelay(10);
	P("  core full release: PWR_CON=%08x STATUS=%08x/%08x STA1=%08x",
	  readl(r), readl(spm + CPU_PWR_STATUS),
	  readl(spm + CPU_PWR_STATUS_2ND),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
}

static void ringwatch_probe_axi(void)
{
	u32 a20c, a22c;

	if (!probe_axi_from_watcher)
		return;
	/*
	 * MISCDBG (0x1022220c) and MP2_AXI_CONFIG (0x1022222c) live in MCUCFG,
	 * which is secure: non-secure reads return 0 (or all-ones) and
	 * non-secure writes are dropped. The first version of this probe used
	 * plain readl/writel, so its "1022220c=00000000" was the bus answering,
	 * not the register — and its own `!= 0xffffffff` guard then skipped the
	 * 0x22c write entirely. It has therefore never performed the operation
	 * it claims to. Go through ATF's guarded accessors instead.
	 *
	 * The bits come from ATF's own power_off_cl3 (freedomtan/atf-1.0-x20,
	 * plat/mt6797/power.c), which quiesces the interface with
	 *	MISCDBG |= (1<<0) | (1<<4);
	 * before engaging protection. Its power_on_cl3 only ever clears bit 0,
	 * and only *after* the protection release it is stuck on — so if bit 4
	 * latched at power-down, nothing in the shipping firmware clears it.
	 *
	 * The watcher may NOT do that itself: the secure accessors are SMCs,
	 * and issuing one while a core is parked at EL3 is how the watcher
	 * joins it (lab rule 1.1/2.7). So the real operation lives in
	 * clear_miscdbg_pre_smc() below, which runs before CPU_ON. All the
	 * watcher does here is report the non-secure view for continuity.
	 */
	a20c = readl(mp2cfg + 0x20c);
	a22c = readl(mp2cfg + 0x22c);
	P("==== watcher AXI probe (non-secure view only): 1022220c=%08x 1022222c=%08x STA1=%08x ====",
	  a20c, a22c, readl(infracfg + TOPAXI_PROTECTEN_STA1));
}

/*
 * Clear MISCDBG's AXI quiesce bits before handing the cluster to ATF.
 *
 * ATF's power_off_cl3 sets MISCDBG (MCUCFG+0x220c) bits 0 and 4 to quiesce
 * MP2's interface, then engages INFRA_TOPAXI_PROTECTEN1 bits 2/6/10 and waits
 * for PROTEXTSTA3 to report all three. Its power_on_cl3 releases the
 * protection FIRST and only then clears bit 0 — and never clears bit 4 at all.
 * Poll 5 is exactly that protection release, so if either quiesce bit is still
 * latched from whoever powered MP2 down at boot, ATF is asking the fabric to
 * bring up a port it has itself told to stay idle, and it will wait forever.
 *
 * Doing it here rather than in the watcher is mandatory: these are secure
 * accessors, i.e. SMCs, and the watcher must never issue one (lab rule 2.7).
 */
/*
 * A diagnostic that runs ON the A72, during its own hotplug bring-up.
 *
 * cpu8 comes online, prints from its own core, and then hard-locks with zero
 * hardirqs, zero softirqs and zero cputime — it goes idle and never wakes. The
 * two candidate explanations are (a) its architected counter is not running,
 * so the timer comparator never fires, or (b) it is not receiving interrupts
 * at all. Reading CNTVCT twice from the core itself separates them, and this
 * is the only place code of ours can run there before it idles.
 */
static u64 a72_diag[6];
static u64 a72_gic[6];
static int a72_diag_done;
static int a72_diag_state = -1;

static int a72_diag_online(unsigned int cpu)
{
	if (cpu != (unsigned int)cpu_target)
		return 0;

	a72_diag[0] = read_sysreg(midr_el1);
	a72_diag[1] = read_sysreg(cntfrq_el0);
	a72_diag[2] = read_sysreg(cntvct_el0);
	udelay(200);
	a72_diag[3] = read_sysreg(cntvct_el0);
	a72_diag[4] = read_sysreg(cntkctl_el1);
	a72_diag[5] = read_sysreg(cntv_ctl_el0);
	/*
	 * The counter is fine, so the core is not waking because it is not
	 * being interrupted. These are the GICv3 CPU-interface system
	 * registers: if SRE is clear the interface is not even in system-register
	 * mode; if IGRPEN1 is clear group-1 IRQs are off; if PMR is 0 everything
	 * is masked. gic_cpu_init() runs at CPUHP_AP_*_STARTING, i.e. before this
	 * callback, so all three should already be set the same as on an A53.
	 */
	a72_gic[0] = read_sysreg_s(SYS_ICC_SRE_EL1);
	a72_gic[1] = read_sysreg_s(SYS_ICC_IGRPEN1_EL1);
	a72_gic[2] = read_sysreg_s(SYS_ICC_PMR_EL1);
	a72_gic[3] = read_sysreg_s(SYS_ICC_CTLR_EL1);
	a72_gic[4] = read_sysreg(daif);
	a72_gic[5] = read_sysreg_s(SYS_ICC_BPR1_EL1);

	/*
	 * Does this core survive a broadcast TLB invalidate?
	 *
	 * The A72 does not merely stop taking interrupts — it stops executing.
	 * The kernel's cluster-2 idle-poll heartbeat freezes mid-count while the
	 * core is still powered (MP2_CPU0_PWR_CON keeps PWR_ON) and is NOT in
	 * WFI (SPM_CPU_IDLE_STA2 bit 20 clear). A core that halts in that state
	 * is stalled on something the interconnect never completes, and the
	 * commonest such thing is `tlbi ...is` + `dsb ish`: the DSB cannot
	 * retire until every DVM participant acknowledges. That also explains
	 * why a tight cpu_relax() spinner survived for minutes — it issues no
	 * TLBIs — while anything doing real work did not.
	 *
	 * Local first, then inner-shareable, with a print between them, so the
	 * netconsole line that is missing names the one that hung.
	 */
	P("  a72 self-test: local TLBI…");
	asm volatile("tlbi vmalle1\n dsb nsh\n isb" ::: "memory");
	P("  a72 self-test: local TLBI returned. broadcast TLBI (dsb ish)…");
	asm volatile("tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
	P("  a72 self-test: broadcast TLBI returned. bare dsb ish…");
	asm volatile("dsb ish" ::: "memory");
	P("  a72 self-test: ALL PASSED — DVM/coherency is not the stall");

	/*
	 * Can this core use memory beyond its L1?
	 *
	 * After ATF's own power-on, MP2_CPUSYS_PWR_CON reads 0x00010127: PWR_ISO,
	 * SRAM_CKISO and SRAM_PDN all still asserted, PWR_ON_2ND and SRAM_ISOINT_B
	 * absent, SRAM_PDN_ACK and SRAM_SLEEP_B_ACK never set — against a live A53
	 * cluster's 0x0009004d. That is the register picture of a cluster whose
	 * SRAM is powered down and isolated. A core in that state can still run
	 * indefinitely out of L1, which is exactly what the idle poll loop and the
	 * cpu_relax() spinner are, and would stall the moment it needs L2.
	 *
	 * 256 KB is four times this A72's 32 KB L1-D, so every pass must miss into
	 * L2. If the core never returns from here, the SRAM is the answer.
	 */
	{
		static u8 sweep[256 * 1024] __aligned(64);
		volatile u64 acc = 0;
		int pass, off;

		P("  a72 self-test: sweeping 256 KB (4x L1-D) to force L2 traffic…");
		for (pass = 0; pass < 8; pass++)
			for (off = 0; off < sizeof(sweep); off += 64)
				acc += READ_ONCE(sweep[off]);
		P("  a72 self-test: 256 KB sweep x8 RETURNED (acc=%llu) — L2 works",
		  (unsigned long long)acc);
	}

	a72_diag_done = 1;
	return 0;
}

/*
 * Compare the target A72's GIC redistributor frame against a working A53's.
 *
 * The CPU interface on cpu8 reads back exactly like an A53's (SRE=1,
 * IGRPEN1=1, PMR=0xf0, DAIF=0) and its architected counter runs, yet it takes
 * zero interrupts. That leaves the redistributor: GICR_WAKER.ProcessorSleep
 * still set, or the PPI never enabled in this frame. GICR stride is 0x20000
 * (RD frame + SGI frame), so cpu N is at 0x19200000 + N * 0x20000 — which is
 * where the kernel said it found cpu8: 0x19300000.
 */
#define GICR_BASE_PHYS		0x19200000u
#define GICR_STRIDE		0x20000u
#define GICR_CTLR		0x0000
#define GICR_TYPER		0x0008
#define GICR_WAKER		0x0014
#define GICR_SGI_OFF		0x10000
#define GICR_IGROUPR0		(GICR_SGI_OFF + 0x0080)
#define GICR_ISENABLER0		(GICR_SGI_OFF + 0x0100)

static void gicr_dump(unsigned int cpu)
{
	void __iomem *r = ioremap(GICR_BASE_PHYS + cpu * GICR_STRIDE, GICR_STRIDE);

	if (!r) {
		P("    cpu%u redistributor: ioremap failed", cpu);
		return;
	}
	P("    cpu%u GICR @%08x: CTLR=%08x TYPER=%08x_%08x WAKER=%08x "
	  "IGROUPR0=%08x ISENABLER0=%08x", cpu,
	  GICR_BASE_PHYS + cpu * GICR_STRIDE,
	  readl(r + GICR_CTLR), readl(r + GICR_TYPER + 4), readl(r + GICR_TYPER),
	  readl(r + GICR_WAKER), readl(r + GICR_IGROUPR0),
	  readl(r + GICR_ISENABLER0));
	if (readl(r + GICR_WAKER) & BIT(2))
		P("      *** ProcessorSleep still SET on cpu%u — the "
		  "redistributor was never woken, so nothing is delivered ***", cpu);
	iounmap(r);
}

/*
 * Does the A72 take an interrupt at all?
 *
 * Everything static checks out — counter running, timer PPI enabled in its
 * redistributor, redistributor awake, CPU interface in system-register mode
 * with IRQs unmasked — and yet the core records zero hardirqs and hard-locks
 * as soon as it idles. An IPI is the smallest possible test of delivery.
 *
 * It must be the non-waiting form: if the target never runs the callback, a
 * waiting smp_call would hang this CPU too, and that is precisely the cascade
 * lab rule 1.1 exists to prevent.
 */
static atomic_t a72_ipi_seen = ATOMIC_INIT(0);

static void a72_ipi_fn(void *unused)
{
	atomic_set(&a72_ipi_seen, 1);
}

static void a72_ipi_test(void)
{
	int i;

	atomic_set(&a72_ipi_seen, 0);
	P("  --- IPI delivery test: pinging cpu%d (non-waiting) ---", cpu_target);
	smp_call_function_single(cpu_target, a72_ipi_fn, NULL, 0);
	for (i = 0; i < 200; i++) {
		if (atomic_read(&a72_ipi_seen)) {
			P("    *** cpu%d ANSWERED the IPI after %d ms — interrupt "
			  "delivery works ***", cpu_target, i);
			return;
		}
		mdelay(1);
	}
	P("    cpu%d did NOT answer an IPI in 200 ms — it is online but "
	  "receives no interrupts", cpu_target);
}

/*
 * Keep the A72 out of WFI, and prove it does real work.
 *
 * cpu8 answers an IPI 1 ms after coming online and then dies within ~10 s,
 * recording zero hardirqs and zero cputime. It works until it idles. ATF's own
 * power_off_cl3 waits on `SPM_CPU_IDLE_STA2 bit 20` — "Wait CL3's WFI" — so the
 * SPM is watching MP2's WFI signal; if nothing has told the power controller
 * that cluster 2 is live, the first WFI completes a power-down it still
 * believes is pending. A bound spinner never executes WFI, so if the core
 * survives while it runs, that is the mechanism.
 */
/*
 * A SCHED_IDLE spinner is `idle=poll`, restricted to one CPU and done from a
 * module instead of the command line — which we cannot change without a flash.
 *
 * The A72 is fully functional right up to the moment it executes WFI, and it
 * cannot be woken from there even though it stays powered and clocked. At the
 * lowest scheduling class the spinner is preempted by any real work, so the
 * core behaves as a normal CPU; it only runs when the core would otherwise
 * have idled, and stops it reaching WFI.
 */
static struct task_struct *a72_keepalive;
static int stay_loaded;

static void a72_watch_die(void);

static int a72_keepalive_fn(void *arg)
{
	sched_set_normal(current, 19);
	while (!kthread_should_stop()) {
		cpu_relax();
		if (need_resched())
			schedule();
	}
	return 0;
}

static int a72_spin_fn(void *arg)
{
	unsigned long end = jiffies + msecs_to_jiffies(spin_ms);
	u64 iters = 0;
	u64 t0 = read_sysreg(cntvct_el0), t1;

	while (time_before(jiffies, end)) {
		iters++;
		cpu_relax();
		if (!(iters & 0xffff)) {
			touch_nmi_watchdog();
			touch_softlockup_watchdog();
		}
	}
	t1 = read_sysreg(cntvct_el0);
	P("  spinner on cpu%d finished: %llu iterations in %llu counter ticks "
	  "(%llu ms at 13 MHz)", smp_processor_id(), iters, t1 - t0,
	  (t1 - t0) / 13000);
	return 0;
}

static void a72_spin_test(void)
{
	struct task_struct *t;

	if (spin_ms < 0) {
		a72_keepalive = kthread_create(a72_keepalive_fn, NULL,
					       "a72-keepalive/%d", cpu_target);
		if (IS_ERR(a72_keepalive)) {
			P("    could not create the keepalive thread (%ld)",
			  PTR_ERR(a72_keepalive));
			a72_keepalive = NULL;
			return;
		}
		kthread_bind(a72_keepalive, cpu_target);
		wake_up_process(a72_keepalive);
		P("  --- permanent SCHED_IDLE keepalive pinned to cpu%d ---",
		  cpu_target);
		P("    the module stays loaded; rmmod gemini_a72_psci stops it "
		  "and cpu%d will wedge on its next idle", cpu_target);
		msleep(2000);
		P("    cpu%d online=%d after 2 s of keepalive", cpu_target,
		  cpu_online(cpu_target));
		return;
	}
	if (spin_ms == 0)
		return;
	P("  --- pinning a spinner to cpu%d for %d ms (no WFI while it runs) ---",
	  cpu_target, spin_ms);
	t = kthread_create(a72_spin_fn, NULL, "a72-spin");
	if (IS_ERR(t)) {
		P("    could not create the spinner (%ld)", PTR_ERR(t));
		return;
	}
	kthread_bind(t, cpu_target);
	wake_up_process(t);
	msleep(spin_ms + 500);
	P("    spinner window over; cpu%d online=%d", cpu_target,
	  cpu_online(cpu_target));

}

static void a72_watch_die(void)
{
	/*
	 * Watch it die, with enough resolution to say *when*.
	 *
	 * SPM_CPU_IDLE_STA2 bit 20 is MP2's WFI status — the bit ATF's own
	 * power_off_cl3 spins on ("Wait CL3's WFI"). That makes it a direct
	 * test of whether the cluster-2 poll-idle patch in arch_cpu_idle() is
	 * doing anything: a polling core never executes WFI, so bit 20 must
	 * stay CLEAR. If it is set, the patch is not on the path being taken
	 * and the builds that carried it tested nothing.
	 *
	 * The IPI probe is non-waiting, so a core that has stopped answering
	 * cannot hang the sampler; the first sample that fails timestamps the
	 * death to within 500 ms.
	 */
	{
		int k;
		u64 prev = 0;
		int frozen = 0;

		P("  --- watching cpu%d: poll heartbeat every 100 ms ---",
		  cpu_target);
		for (k = 0; k < 200; k++) {
			u32 s2;
			int answered;

			/*
			 * No IPI in the loop. smp_call_function_single() takes a
			 * per-CPU csd even in its non-waiting form, and a second call
			 * to a CPU that never dequeued the first one blocks the
			 * *caller* — which is why every previous run of this probe
			 * produced exactly one sample. polls and irqs are plain
			 * memory reads and cannot hang anything.
			 */
			answered = -1;
			msleep(50);
			s2 = readl(spm + CPU_IDLE_STA2);
			/*
			 * polls  = the kernel's cluster-2 idle-poll heartbeat. If it
			 *          climbs, the core is executing and merely deaf; if it
			 *          stops, the core itself has stopped.
			 * irqs   = every interrupt the core has taken.
			 */
			{
				/*
				 * The kernel's idle-poll heartbeat is gone now
				 * that cluster 2 idles normally; track the
				 * interrupt count instead, which is the thing
				 * that actually stopped when the core died.
				 */
				u64 now = kstat_cpu_irqs_sum(cpu_target);

				/*
				 * Only print while it is still moving, then the
				 * moment it stops, then stop printing. A wall
				 * of identical lines is what buried the last
				 * three runs.
				 */
				if (now != prev) {
					P("   +%3d ms irqs=%llu (+%llu) "
					  "wfi=%u PWR_CON=%08x", k * 100, now,
					  now - prev,
					  !!(s2 & BIT(20)),
					  readl(spm + MP2_CPU0_PWR_CON +
						((cpu_target - 8) * 4)));
					prev = now;
					frozen = 0;
				} else if (++frozen == 3) {
					P("   *** FROZEN at +%d ms: irqs stuck "
					  "at %llu, wfi=%u, PWR_CON=%08x ***",
					  k * 100, now,
					  !!(s2 & BIT(20)),
					  readl(spm + MP2_CPU0_PWR_CON +
						((cpu_target - 8) * 4)));
					vproc2_report("at the freeze");
				}
			}
			msleep(50);
		}
	}

	/*
	 * Now let it idle, and look at what the hardware did to it.
	 *
	 * If MP2_CPU0_PWR_CON loses PWR_ON, or its SPMC leaves FSM_ON, then WFI
	 * caused a power-down and the core is simply gone. If the registers are
	 * unchanged but it still will not answer an IPI, the core is parked in a
	 * state it cannot be woken from. If it *does* answer, then WFI is fine
	 * and only the timer never fires.
	 */
	P("  --- letting cpu%d idle for 3 s, then probing it ---", cpu_target);
	msleep(3000);
	P("    MP2_CPU%d_PWR_CON=%08x CPU_PWR_STATUS=%08x/%08x coreSPMC=%08x MP2=%08x",
	  cpu_target - 8,
	  readl(spm + MP2_CPU0_PWR_CON + ((cpu_target - 8) * 4)),
	  readl(spm + CPU_PWR_STATUS), readl(spm + CPU_PWR_STATUS_2ND),
	  sread(0x10222430u + ((cpu_target - 8) * 4)),
	  readl(spm + MP2_CPUSYS_PWR_CON));
	/*
	 * And what is its clock doing while it sleeps? ATF is long finished, so
	 * CLK26CALI is ours again. If source 37 has fallen back to 26 MHz or
	 * zero, something took the cluster clock away the moment the core
	 * stopped executing, and no interrupt can wake a core with no clock.
	 */
	armpll2_report("while cpu is idle");
	meter_report("while cpu is idle");
	P("  --- IPI test again, this time while cpu%d is IDLE ---", cpu_target);
	a72_ipi_test();
}

/*
 * cpuhvfs_notify_cluster_on(CPU_CLUSTER_B), transcribed from the vendor's
 * mt_cpufreq_hybrid.c: require SW_PAUSE set at entry, then clear it and set
 * CLUSTER_EN, mirroring into CSRAM. Android does this immediately after the
 * kernel's CPU_ON returns; we have never done it, and it is the only step of
 * the vendor's big-core online path still missing.
 */
static void a72_cspm_cluster_on(void)
{
	u32 b;

	if (!cspm_cluster_on_after)
		return;

	b = readl(cspm + CSPM_SW_RSV2);
	P("  --- cpuhvfs_notify_cluster_on(B): SW_RSV2 = %08x ---", b);
	if (!(b & CPUHVFS_SW_PAUSE)) {
		P("    REFUSING: vendor requires SW_PAUSE set at entry "
		  "(is the PCM running with B parked?)");
		return;
	}
	writel(0, csram + CSRAM_PAUSE_SRC);
	b = (b | CPUHVFS_CLUSTER_EN) & ~CPUHVFS_SW_PAUSE;
	writel(b, cspm + CSPM_SW_RSV2);
	writel(b, csram + CSRAM_SW_RSV2);
	udelay(200);
	P("    -> SW_RSV2 = %08x  MP2=%08x  (Android's online value is 0x56f0)",
	  readl(cspm + CSPM_SW_RSV2), readl(spm + MP2_CPUSYS_PWR_CON));
}

/*
 * Undo ATF's per-core SPARK retention enable.
 *
 * After powering the core, ATF calls big_spark2_setldo() and then
 * big_spark2_core(core, 1), which is just bit 0 of that core's SPMC register
 * (ATF's own log: "Write 10222430 = bb0101"). SPARK is the per-core retention
 * LDO: with it armed, a WFI can drop the core into retention at a reduced
 * voltage, and the exit from that state is managed by CPUHVFS on Android. The
 * A53s use the same mechanism and are fine, but nothing on our side manages it
 * for cluster 2 — and cluster 2's core dies exactly when it first idles.
 */
static void a72_disable_spark(void)
{
	u32 reg = 0x10222430u + ((cpu_target - 8) * 4);
	u32 v;

	if (!disable_big_spark)
		return;
	v = sread(reg);
	P("  --- disabling SPARK retention: %08x = %08x -> %08x ---",
	  reg, v, v & ~1u);
	swrite(reg, v & ~1u);
	udelay(10);
	P("    readback %08x", sread(reg));
}

static void a72_diag_report(void)
{
	if (!a72_diag_done) {
		P("  A72 self-diagnostic did NOT run (callback never fired)");
		return;
	}
	P("  --- read ON cpu%d, by itself, during bring-up ---", cpu_target);
	P("    MIDR_EL1   = %016llx", a72_diag[0]);
	P("    CNTFRQ_EL0 = %llu Hz", a72_diag[1]);
	P("    CNTVCT_EL0 = %llu then %llu after 200 us  -> delta %lld",
	  a72_diag[2], a72_diag[3], (long long)(a72_diag[3] - a72_diag[2]));
	P("    %s", a72_diag[3] == a72_diag[2] ?
	  "*** THE ARCHITECTED COUNTER IS STOPPED ON THIS CORE ***" :
	  "counter is running — so the timer comparator can fire; suspect the GIC");
	P("    CNTKCTL_EL1 = %016llx  CNTV_CTL_EL0 = %016llx",
	  a72_diag[4], a72_diag[5]);
	P("    ICC_SRE_EL1=%llx IGRPEN1=%llx PMR=%llx CTLR=%llx BPR1=%llx DAIF=%llx",
	  a72_gic[0], a72_gic[1], a72_gic[2], a72_gic[3], a72_gic[5], a72_gic[4]);
	if (!(a72_gic[0] & 1))
		P("    *** ICC_SRE_EL1.SRE is CLEAR — the CPU interface is not in "
		  "system-register mode; EL3 never enabled it for this core ***");
	else if (!(a72_gic[1] & 1))
		P("    *** ICC_IGRPEN1_EL1 is CLEAR — group-1 IRQs disabled ***");
	else if (a72_gic[2] == 0)
		P("    *** ICC_PMR_EL1 is 0 — every priority is masked ***");
	else
		P("    CPU interface looks correctly configured; the redistributor "
		  "or the distributor routing is the remaining suspect");

	P("  --- GIC redistributor frames (A53 reference vs the A72) ---");
	gicr_dump(0);
	gicr_dump(4);
	gicr_dump(cpu_target);
	a72_ipi_test();
	a72_disable_spark();
	a72_cspm_cluster_on();
	a72_spin_test();
	a72_watch_die();
}

static void clear_miscdbg_pre_smc(void)
{
	u32 a20c, a22c;

	if (!clear_miscdbg)
		return;

	a20c = sread(0x1022220cu);
	a22c = sread(0x1022222cu);
	P("  pre-SMC MISCDBG (secure): 1022220c=%08x 1022222c=%08x STA1=%08x",
	  a20c, a22c, readl(infracfg + TOPAXI_PROTECTEN_STA1));

	if (a20c & 0x11u) {
		swrite(0x1022220cu, a20c & ~0x11u);
		udelay(10);
		P("    MISCDBG &= ~0x11 -> %08x  STA1=%08x", sread(0x1022220cu),
		  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	} else {
		P("    MISCDBG quiesce bits already clear — nothing to undo");
	}

	if (clear_miscdbg > 1 && (a22c & 0x11u) && a22c != 0xffffffffu) {
		swrite(0x1022222cu, a22c & ~0x11u);
		udelay(10);
		P("    MP2_AXI_CONFIG &= ~0x11 -> %08x  STA1=%08x",
		  sread(0x1022222cu), readl(infracfg + TOPAXI_PROTECTEN_STA1));
	}
}

/*
 * MP2's sync DCM, from the vendor's mt_dcm.c:dcm_mcusys_mp2_sync_dcm().
 *
 * `0x10222274` bit 0 enables clock gating on MP2's sync (bus) clock; bit 1 is
 * a toggle the vendor pulses to make the new setting take, and bits [6:2] are
 * a divider select. The vendor kernel disables this DCM from cpu_psci_cpu_die()
 * when the last big core leaves, and only re-enables it *after* CPU_ON returns
 * (psci.c:597 and :808). So throughout ATF's power_on_cl3 — including poll 5 —
 * Android is running with MP2's sync DCM OFF, i.e. the bus clock ungated.
 *
 * We have never touched it, so ours is in whatever state boot left it. Poll 5
 * waits for the fabric to power MP2's port up, and that handshake is clocked;
 * a gated clock is a sufficient explanation for an ack that never arrives.
 */
#define MCUCFG_SYNC_DCM_MP2	0x10222274u
#define SYNC_DCM_MP2_MASK	0x7fu		/* bit0 en, bit1 tog, [6:2] div */

static void set_mp2_sync_dcm_pre_smc(void)
{
	u32 v;

	if (mp2_sync_dcm < 0)
		return;

	v = sread(MCUCFG_SYNC_DCM_MP2);
	P("  pre-SMC MP2 sync DCM: 0x10222274 = %08x (bit0 gating=%u)",
	  v, v & 1u);

	/* Vendor writes the value twice, with TOG set then clear. */
	if (mp2_sync_dcm == 0) {
		swrite(MCUCFG_SYNC_DCM_MP2, (v & ~SYNC_DCM_MP2_MASK) | 0x2u);
		swrite(MCUCFG_SYNC_DCM_MP2, (v & ~SYNC_DCM_MP2_MASK) | 0x0u);
	} else {
		swrite(MCUCFG_SYNC_DCM_MP2, (v & ~SYNC_DCM_MP2_MASK) | 0x0fu);
		swrite(MCUCFG_SYNC_DCM_MP2, (v & ~SYNC_DCM_MP2_MASK) | 0x0du);
	}
	udelay(10);
	P("    -> %08x  STA1=%08x", sread(MCUCFG_SYNC_DCM_MP2),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
}

static void ringwatch_stop_pcm(void)
{
	if (!stop_pcm_from_watcher)
		return;

	P("==== watcher PCM intervention: stopping paused CPUHVFS ====");
	P("  before stop: FSM=%08x MUXSEL=%08x CKDIV=%08x STA1=%08x",
	  readl(cspm + CSPM_PCM_FSM_STA),
	  readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	writel(CSPM_KEY, cspm + CSPM_POWERON_CONFIG);
	writel(CSPM_CON0_KEY | CSPM_PCM_SW_RESET, cspm + CSPM_PCM_CON0);
	writel(CSPM_CON0_KEY, cspm + CSPM_PCM_CON0);
	udelay(10);
	P("  after stop: FSM=%08x MUXSEL=%08x CKDIV=%08x STA1=%08x",
	  readl(cspm + CSPM_PCM_FSM_STA),
	  readl(mcumixed + ARMPLLDIV_MUXSEL),
	  readl(mcumixed + ARMPLLDIV_CKDIV),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	if (armpll2_khz)
		armpll2_set(armpll2_khz);
}

static int ringwatch_fn(void *unused)
{
	int n;

	/* The SMC must not be issued until this independently scheduled. */
	complete(&ringwatch_started);
	for (n = 0; n < 120 && !kthread_should_stop(); n++) {
		if (n == 0 && (force_clock_from_watcher || stop_pcm_from_watcher ||
			       release_mp2_sram_from_watcher || handoff_b_pcm_from_watcher ||
			       enable_cci_from_watcher || power_core_from_watcher ||
			       probe_axi_from_watcher))
			msleep(1000);
		else
			ssleep(5);
		P("==== ring watch tick %d (cpu%d) ====", n, smp_processor_id());
		/*
		 * Intervene before dumping the ring. A frequency retry emits about
		 * 1400 lines/s; printing that backlog can delay this kthread by tens
		 * of seconds and miss the useful window entirely.
		 */
		if (n == 0) {
			ringwatch_handoff_b_pcm();
			ringwatch_enable_cci();
			ringwatch_stop_pcm();
			ringwatch_force_clock();
			ringwatch_release_mp2_sram();
			ringwatch_power_core();
			ringwatch_probe_axi();
		}
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
		/*
		 * NO abist measurement here. meter() WRITES CLK_DBG_CFG,
		 * CLK_MISC_CFG_0 and CLK26CALI_0 — the same registers ATF is
		 * using for its own frequency check inside the SMC we are
		 * watching. A tick landing inside ATF's measurement corrupts
		 * its reading, which costs it its single retry and then its
		 * PCW assertion: the watcher would be causing the panic it is
		 * there to observe. Everything below is a plain read.
		 */
		P("  watch: MUXSEL=%08x CKDIV=%08x CSPM_SEMA=%08x "
		  "(no abist here — it would race ATF's own meter)",
		  readl(mcumixed + ARMPLLDIV_MUXSEL),
		  readl(mcumixed + ARMPLLDIV_CKDIV),
		  readl(cspm + CSPM_SEMA));
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

	reinit_completion(&ringwatch_started);
	ringwatch = kthread_create(ringwatch_fn, NULL, "a72-ringwatch");
	if (IS_ERR(ringwatch)) {
		P("  could not start the ring watcher");
		ringwatch = NULL;
		return;
	}
	kthread_bind(ringwatch, 0);
	wake_up_process(ringwatch);
	if (!wait_for_completion_timeout(&ringwatch_started, HZ)) {
		P("  ring watcher did not schedule within 1 s — REFUSING CPU_ON");
		kthread_stop(ringwatch);
		ringwatch = NULL;
		return;
	}
	P("  ring watcher scheduled on cpu0; it survives a spin on cpu%d", smc_cpu);
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
	if (handoff_b_pcm_from_watcher) {
		u32 b = readl(cspm + CSPM_SW_RSV2);
		bool already_paused = !!(b & CPUHVFS_SW_PAUSE);

		P("  pre-SMC CPUHVFS handoff: disabling+pausing B, SW_RSV2=%08x", b);
		if (!(b & CPUHVFS_CLUSTER_EN)) {
			P("  REFUSING CPU_ON: handoff requires B CLUSTER_EN");
			return;
		}
		b = (b | CPUHVFS_SW_PAUSE) & ~CPUHVFS_CLUSTER_EN;
		writel(b, cspm + CSPM_SW_RSV2);
		writel(b, csram + CSRAM_SW_RSV2);
		if (!already_paused) {
			udelay(10); /* vendor: skip FW_DONE 1->0 transition */
			for (b = 0; b < 2000; b++) {
				if ((readl(cspm + CSPM_HWSTA_LL) & CPUHVFS_FW_DONE) &&
				    (readl(cspm + CSPM_HWSTA_L) & CPUHVFS_FW_DONE) &&
				    (readl(cspm + CSPM_HWSTA_B) & CPUHVFS_FW_DONE))
					break;
				udelay(10);
			}
			P("  pre-SMC pause FW_DONE wait=%u us HWSTA=%08x/%08x/%08x",
			  b * 10, readl(cspm + CSPM_HWSTA_LL),
			  readl(cspm + CSPM_HWSTA_L), readl(cspm + CSPM_HWSTA_B));
			if (b == 2000) {
				P("  REFUSING CPU_ON: CPUHVFS did not complete its pause");
				return;
			}
		} else {
			P("  B was already paused; no in-flight transition to drain");
		}
		writel((readl(cspm + CSPM_SW_RSV0) & CPUHVFS_SW_PAUSE) ? BIT(0) : 0,
		       csram + CSRAM_PAUSE_SRC);
		udelay(1000);
		P("  pre-SMC B paused: SW_RSV2=%08x PAUSE_SRC=%08x",
		  readl(cspm + CSPM_SW_RSV2), readl(csram + CSRAM_PAUSE_SRC));
	}
	clear_miscdbg_pre_smc();
	set_mp2_sync_dcm_pre_smc();
	ringwatch_start();
	if (!ringwatch) {
		P("  REFUSING CPU_ON: the in-kernel watcher is not running");
		return;
	}
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
	bool assertions_ok = false;

	P("==== gemini-a72-psci stage=%d cpu=%d vproc2=%d mV settle=%d ms ====",
	  stage, cpu_target, vproc2_mv, settle_ms);

	/*
	 * Validate before anything touches a register. Every offset in this
	 * module is computed from cpu_target as `idx = cpu - 8`, so cpu=0 would
	 * make MP2_CPU0_PWR_CON + idx*4 land on SPM+0x220 — MP0_CPU0_PWR_CON,
	 * a CPU this kernel is running on — and the core rehearsal would then
	 * clear its PWR_ON. The secure window check does not catch the MCUCFG
	 * side of that either: 0x10222430 + idx*4 slides to 0x10222410, still
	 * inside ATF's guard.
	 */
	if (cpu_target != 8 && cpu_target != 9) {
		P("cpu_target=%d is not an A72 — refusing (8 or 9 only)", cpu_target);
		return -EINVAL;
	}
	if (smc_cpu <= 0 || smc_cpu >= nr_cpu_ids || !cpu_online(smc_cpu)) {
		P("smc_cpu=%d is not a usable CPU — refusing. It must be online, "
		  "and not cpu0: the ring watcher is bound there and would be "
		  "lost with it.", smc_cpu);
		return -EINVAL;
	}
	/*
	 * The DA9214 codes VBUCKB_A as (mV - 300) / 10 in seven bits, so an
	 * out-of-range value does not fail, it WRAPS: 200 mV asks for 1480 and
	 * 1600 asks for 320. This is the A72 core rail; over-volting it is the
	 * one thing in this repo that no watchdog undoes.
	 */
	if (vproc2_mv && (vproc2_mv < 600 || vproc2_mv > 1180)) {
		P("vproc2_mv=%d is outside 600..1180 — refusing", vproc2_mv);
		return -EINVAL;
	}

	spm = ioremap(SPM_PHYS, 0x1000);
	mcumixed = ioremap(MCUMIXED_PHYS, 0x1000);
	topckgen = ioremap(TOPCKGEN_PHYS, 0x1000);
	cspm = ioremap(CSPM_PHYS, 0x1000);
	csram = ioremap(0x0012a000, 0x1000);
	infracfg = ioremap(INFRACFG_PHYS, 0x1000);
	cci = ioremap(CCI_PHYS, 0x10000);
	mp2cfg = ioremap(0x10222000, 0x1000);
	if (!spm || !mcumixed || !topckgen || !cspm || !csram || !infracfg || !cci ||
	    !mp2cfg) {
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

	if (!spmc_software_top_on()) {
		P("  software SPMC TOP sequence failed — refusing further action");
		goto out;
	}

	if (skip_cputop) {
		P("==== skip_cputop: leaving the CPUTOP for ATF ====");
		P("  MP2=%08x CPU_PWR_STATUS=%08x — ATF's two power-good polls are"
		  " LIVE this run", readl(spm + MP2_CPUSYS_PWR_CON),
		  readl(spm + CPU_PWR_STATUS));
		P("  0x102224a0 = %08x (dark until powered; assertions unreadable)",
		  sread(0x102224a0));
	} else if (!cputop_power_on()) {
		goto out;
	}

	assertions_ok = skip_cputop ? true : atf_assertions_hold();
	if (!assertions_ok)
		P("  NOTE: ATF would panic with the block in this state");

	meter_report("cluster powered, before ATF's clock steps");

	if (stage == 2) {
		atf_clock_rehearsal(NULL, 0);
		goto out;
	}

	if (stage == 5) {
		atf_clock_rehearsal(atf_tail_rehearsal, cpu_target);
		cspm_sema_report_and_free();
		bus_report("stage 5");
		P("Every unbounded poll in ATF's path has now been measured "
		  "bounded. Stage 3 is the real call.");
		goto out;
	}

	if (stage == 3 || stage == 6) {
		/*
		 * Refuse to fire when the module has already proved ATF cannot
		 * succeed. Both of these are cheap and both are irreversible if
		 * ignored: a failed assertion is an EL3 panic, and a frequency
		 * outside ATF's windows costs it its one retry and then the
		 * same panic. armpll2_khz defaults to 0, so without this guard
		 * a bare `run-psci.sh 3` fires at ~630 MHz — outside both
		 * windows — which is exactly the run that cost 2026-08-22 its
		 * device.
		 *
		 * The quantity to gate on is ARMCAXPLL2's PROGRAMMED rate, not
		 * the live abist reading. At this point in the flow ATF's clock
		 * steps have not run: the cluster is powered but PWR_CLK_DIS is
		 * still set, so source 37 reads 26000 kHz. It becomes 749988
		 * only after ATF sets the mux and the divider itself — measured
		 * in stage 2. ARMCAXPLL2 is what source 37 then follows, and it
		 * is the thing this module actually controls.
		 */
		unsigned int f = pll_khz(readl(mcumixed + ARMCAXPLL2_CON1));

		P("gate check: ARMCAXPLL2 is programmed for %u kHz "
		  "(live abist(37) = %u kHz, still gated by PWR_CLK_DIS)",
		  f, meter(37));

		if (!assertions_ok) {
			P("REFUSING CPU_ON: ATF's entry assertions do not hold, "
			  "so it would panic at EL3. Fix the block state first.");
			goto out;
		}
		if (skip_cputop)
			P("  (assertions not checked: skip_cputop leaves the block "
			  "dark until ATF powers it)");
		if (!((f >= ATF_WIN1_LO && f <= ATF_WIN1_HI) ||
		      (f >= ATF_WIN2_LO && f <= ATF_WIN2_HI))) {
			P("REFUSING CPU_ON: ARMCAXPLL2 = %u kHz is outside both "
			  "of ATF's windows [%u..%u] and [%u..%u].", f,
			  ATF_WIN1_LO, ATF_WIN1_HI, ATF_WIN2_LO, ATF_WIN2_HI);
			P("Pass armpll2_khz=750000 — that is the whole point of "
			  "this tool.");
			goto out;
		}
		P("gate check passed: assertions hold and ARMCAXPLL2 = %u kHz", f);

		if (prime_atf_meter == 1) {
			if (!(readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT)) {
				P("REFUSING CPU_ON: prime_atf_meter=1 requires a pre-powered CPUTOP");
				goto out;
			}
			P("==== priming ATF's one-sample-lag abist meter from source37 ====");
			atf_clock_rehearsal(prime_atf_meter_latch, 0);
			P("  no further CLK26CALI access is permitted before CPU_ON");
		} else if (prime_atf_meter == 2) {
			unsigned int first, second, src, div;
			unsigned int chosen_src = UINT_MAX, chosen_div = 0;

			P("==== finding a 750 MHz raw count to prime ATF's lagged meter ====");
			for (div = 1; div <= 16 && chosen_src == UINT_MAX; div++) {
				for (src = 0; src < 64; src++) {
					unsigned int measured;

					meter_once_div(src, div);
					measured = meter_once_div(src, div);
					if (measured >= ATF_WIN1_LO && measured <= ATF_WIN1_HI) {
						P("  source%u divider-code%u=%u kHz is in ATF's first window",
						  src, div, measured);
						chosen_src = src;
						chosen_div = div;
						break;
					}
				}
			}
			if (chosen_src == UINT_MAX) {
				P("REFUSING CPU_ON: no source 0..63/divider-code 1..16 yields ATF's 750 MHz count");
				goto out;
			}
			first = meter_once_div(chosen_src, chosen_div);
			second = meter_once_div(chosen_src, chosen_div);
			P("  source%u/div%u first=%u second=%u kHz; leaving that count latched",
			  chosen_src, chosen_div, first, second);
			P("  no further CLK26CALI access is permitted before CPU_ON");
		} else if (prime_atf_meter) {
			P("REFUSING CPU_ON: prime_atf_meter must be 0, 1, or 2");
			goto out;
		}

		cspm_sema_report_and_free();
		bus_report("before CPU_ON");

		if (stage == 6) {
			/*
			 * Stage 3 proves the cluster can be powered and that an A72
			 * executes, but it fires a bare SMC at a park stub, so the
			 * core never becomes a Linux CPU. Stage 6 runs the identical
			 * gate and then hands straight to the kernel's own hotplug,
			 * which issues CPU_ON with __pa(secondary_entry) and does all
			 * the bring-up bookkeeping.
			 *
			 * It has to be add_cpu() from in here rather than an `echo`
			 * afterwards, because the frequency gate is passed by priming
			 * ATF's one-sample-lagged abist meter: the latched count is
			 * consumed by the very next CLK26CALI read, so nothing may
			 * come between the priming and the SMC.
			 *
			 * cpu_up() takes the hotplug locks and IPIs everybody, so if
			 * ATF spins at EL3 inside it the whole machine goes rather
			 * than one core. The dead-man switch is the only net.
			 */
			int rc;

			/*
			 * arm64 refuses a late CPU that carries a capability the
			 * system does not have: verify_local_cpu_caps() takes the
			 * `system_has_cap == 0 && cpu_has_cap == 1` branch and, for
			 * ARM64_CPUCAP_LOCAL_CPU_ERRATUM (which is OPTIONAL but not
			 * PERMITTED for late CPUs), calls cpu_die_early():
			 *
			 *   CPU8: Detected conflict for capability 58 (Spectre-v2),
			 *         System: 0, CPU: 1
			 *   CPU8: will not boot
			 *
			 * The A53 boot CPUs are unaffected by Spectre-v2 and by the
			 * A57/A72 errata (1319367 speculative-AT, 1742098 AArch32
			 * AES), so with maxcpus=8 those caps never enter the system
			 * set and every A72 is a conflict. Marking them present makes
			 * the check take the other branch, which also runs the cap's
			 * cpu_enable() on the A72 — i.e. the mitigation actually gets
			 * installed on the core that needs it.
			 *
			 * This is a bring-up shim, not the fix. The fix is the vendor
			 * arrangement: a platform hook that runs these prerequisites
			 * from cpu_psci_cpu_boot() so cpu8/9 come up during boot with
			 * maxcpus removed, and their caps fold into the system set
			 * before it is finalised. That needs a kernel flash.
			 */
			if (late_caps && *late_caps) {
				char *p = late_caps, *tok;

				while ((tok = strsep(&p, ",")) != NULL) {
					int cap;

					if (!*tok || kstrtoint(tok, 0, &cap))
						continue;
					if (cap < 0 || cap >= ARM64_NCAPS) {
						P("  REFUSING cap %d: out of range", cap);
						continue;
					}
					if (test_bit(cap, system_cpucaps)) {
						P("  cap %d already present system-wide", cap);
						continue;
					}
					set_bit(cap, system_cpucaps);
					P("  marked cap %d present system-wide", cap);
				}
			}

			a72_diag_state = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
								   "gemini/a72:online",
								   a72_diag_online, NULL);
			if (a72_diag_state < 0)
				P("  could not register the A72 self-diagnostic (%d)",
				  a72_diag_state);

			P("==== stage 6: handing cpu%d to the kernel's hotplug ====",
			  cpu_target);
			rc = add_cpu(cpu_target);
			P("  *** add_cpu(%d) RETURNED %d ***", cpu_target, rc);
			P("  cpu%d online = %d", cpu_target, cpu_online(cpu_target));
			/*
			 * The frequency gate was passed by priming ATF's lagged meter,
			 * which is a deliberately false premise (§7): it only makes ATF
			 * *believe* the clock is right. Now that ATF is finished and no
			 * longer using CLK26CALI, measure source 37 for real. If the A72
			 * is online but parked on 26 MHz, that — not the bring-up — is
			 * why it stops answering IPIs and why a workqueue pool on it
			 * reports "stuck for 88s".
			 */
			a72_diag_report();
			if (a72_keepalive)
				stay_loaded = 1;

			/*
			 * The second core of the cluster. ATF's power_on_big() skips
			 * power_on_cl3() once big_on is non-zero and goes straight to
			 * this core's two unbounded polls — CPU_PWR_STATUS bit (7-idx)
			 * and its own SPMC bit 17. One of them never acked when cpu9
			 * was tried after cpu8, so read the SPMC at rest first: if it
			 * is not the FSM_OFF/all-off-acked family (0x004x0100) the
			 * attempt is already lost and hanging EL3 to find out is a
			 * waste of a boot.
			 */
			if (rc == 0 && also_cpu >= 8 && also_cpu <= 9 &&
			    also_cpu != cpu_target) {
				u32 sp = sread(0x10222430u + ((also_cpu - 8) * 4));
				int rc2;

				P("==== second core: cpu%d ====", also_cpu);
				P("  its SPMC at rest = %08x (FSM_STATE_OUT=%u), "
				  "MP2_CPU%d_PWR_CON=%08x", sp, (sp >> 23) & 0x1f,
				  also_cpu - 8,
				  readl(spm + MP2_CPU0_PWR_CON + ((also_cpu - 8) * 4)));
				if (((sp >> 23) & 0x1f) != 0) {
					P("  REFUSING: its SPMC is not at rest — ATF "
					  "would spin at EL3 on the power-good poll");
				} else {
					cpu_target = also_cpu;   /* for the diagnostics */
					a72_keepalive = NULL;
					rc2 = add_cpu(also_cpu);
					P("  *** add_cpu(%d) RETURNED %d, online=%d ***",
					  also_cpu, rc2, cpu_online(also_cpu));
					if (rc2 == 0)
						a72_spin_test();
				}
			}
			if (a72_diag_state >= 0)
				cpuhp_remove_state_nocalls(a72_diag_state);
			if (rc == 0) {
				armpll2_report("after add_cpu");
				meter_report("after add_cpu — the REAL A72 clock");
			}
			spm_report("after add_cpu");
			atf_ring_dump("after add_cpu");
			bus_report("after add_cpu");
			goto out;
		}

		if (make_mailbox_and_park()) {
			P("could not build the instrument — refusing to fire");
			goto out;
		}
		raw_cpu_on(cpu_target);
		goto out;
	}

	if (stage == 4) {
		atf_clock_rehearsal(NULL, 0);
		P("prerequisites are in place, VPROC2 is UP and the CPUTOP domain "
		  "is powered past both of ATF's unbounded polls.");
		P("Now: echo 1 > /sys/devices/system/cpu/cpu%d/online", cpu_target);
		/*
		 * cpu_up() takes the hotplug locks and IPIs everybody. If ATF
		 * spins inside it, the whole machine goes, not one CPU — which
		 * is why stage 3 exists and why it fires a bare SMC instead.
		 */
		P("DO NOT do that until stage 3 has shown CPU_ON RETURNING. "
		  "cpu_up() holds the hotplug locks and IPIs every CPU, so a "
		  "spin inside it costs the machine, not one core.");
	}

out:
	/*
	 * Reached only when nothing hung — a spinning SMC never returns here at
	 * all, and the watcher is stopped before we get this far. The MMIO
	 * mappings are safe to drop; the coherent park page deliberately is NOT
	 * freed, because an A72 that fetched the stub may still be executing it
	 * (it CPU_OFFs itself, but nothing here can prove when), and handing
	 * that memory back to the allocator would be worse than leaking it.
	 */
	if (stay_loaded) {
		/*
		 * The convention here is that this module always fails to load
		 * (§4: "insmod reporting an error is success, read dmesg"), so
		 * that nothing of ours is left running. A keepalive thread is
		 * the one case that must outlive init: it is the only thing
		 * stopping the A72 from reaching the WFI it cannot wake from.
		 * Keep the mappings too — the thread's teardown needs none of
		 * them, but unmapping while loaded would be gratuitous.
		 */
		P("==== staying loaded to keep the A72 out of WFI ====");
		return 0;
	}

	if (spm)
		iounmap(spm);
	if (mcumixed)
		iounmap(mcumixed);
	if (topckgen)
		iounmap(topckgen);
	if (cspm)
		iounmap(cspm);
	if (csram)
		iounmap(csram);
	if (infracfg)
		iounmap(infracfg);
	if (mp2cfg)
		iounmap(mp2cfg);
	if (cci)
		iounmap(cci);
	if (atf_ring)
		iounmap(atf_ring);
	P("==== end ====");
	return -EAGAIN;	/* never actually stay loaded */
}

static void __exit a72psci_exit(void)
{
	if (a72_keepalive) {
		P("stopping the cpu%d keepalive — it will wedge on its next idle",
		  cpu_target);
		kthread_stop(a72_keepalive);
		a72_keepalive = NULL;
	}
}

module_init(a72psci_init);
module_exit(a72psci_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mt6797 A72 bring-up through PSCI, with ATF's own frequency gate satisfied");
