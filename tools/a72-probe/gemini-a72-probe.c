// SPDX-License-Identifier: GPL-2.0-only
/*
 * gemini-a72-probe — the A72 cluster power-on prerequisite that mainline has
 * never had, and that four sessions of looking in the power drivers could not
 * find because it is not in them.
 *
 * WHERE IT WAS
 *
 * MediaTek patched cpu_psci_cpu_boot() in arch/arm64/kernel/psci.c. Before
 * PSCI CPU_ON for cpu8/cpu9, the vendor kernel runs cpu_power_on_buck():
 *
 *     SPM + 0x218 |= (1 << 0)              MP2_CPUSYS_PWR_CON bit 0
 *     dummy read of 0x102224a0
 *     latch RESET (PWRAP_SPI_CTL via the watchdog)
 *     da9214_config_interface(0x00, 0x0, 0xF, 0)   PAGE_CON -> page 0
 *     da9214_config_interface(0x5E, 0x1, 0x1, 0)   BUCKB_CONT bit0 -> VPROC2 ON
 *     udelay(1000)
 *     SPM + 0x290 &= ~0x3                  <-- EXT_BUCK_ISO
 *     unlatch RESET
 *     udelay(240); BigiDVFSSRAMLDOSet(110000); udelay(240)
 *
 * THE ONE THAT MATTERS, MEASURED
 *
 * EXT_BUCK_ISO at 0x10006290 reads 0x00000002 on our system — bit 1 SET, i.e.
 * the A72 cluster's external supply is ISOLATED from the SoC. B-40 established
 * that ATF sets MP2_CPUTOP_PWR_ON and then spins forever on a power-good that
 * never asserts, and that it does so whether VPROC2 is up or down. An asserted
 * isolation cell explains exactly that: the rail can be on and the domain still
 * never sees a good supply.
 *
 * B-40 tested "VPROC2 up". It never tested "VPROC2 up AND not isolated",
 * because nothing in base/power/ mentions this register — the write lives in
 * the PSCI core, which is the one place nobody thought to diff.
 *
 * WHAT THIS DOES NOT DO, and why that is a deliberate risk being taken:
 *
 *  - No watchdog RESET latch. mtk_wdt_swsysret_config() has no mainline
 *    equivalent; it exists to stop a watchdog reset landing in the middle of
 *    the PMIC-wrapper transaction. The window is ~1 ms.
 *  - No BigiDVFSSRAMLDOSet(110000). That is an SMC into an iDVFS service this
 *    ATF does not implement (measured: SMC_UNK, see tools/psci-probe). If the
 *    A72s power up but do not execute, the big cluster's SRAM rail is the
 *    first thing to suspect.
 *
 * STAGES
 *   stage=1 (default)  report the registers, change nothing.
 *   stage=2            run the power-on sequence, then report. No CPU_ON.
 *   stage=3            everything in 2, then PSCI CPU_ON(cpu8) from a kthread
 *                      while this CPU polls MP2_CPUSYS_PWR_CON and reports.
 *
 * Stage 3 can hang the machine: CPU_ON with a valid entry is the call B-40
 * showed spins inside ATF. Every step announces itself to /dev/kmsg first, so
 * netconsole names whatever we die in. The watchdog has recovered this box
 * repeatedly today.
 *
 * Copyright (c) 2026 Gemini PDA Linux port
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/arm-smccc.h>
#include <uapi/linux/psci.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <asm/cacheflush.h>

#define CLK_INFRA_DEVICE_APC	47

/*
 * The iDVFS secure calls the vendor actually uses on the A72 path. Android's
 * /proc/idvfs/dvt_test reports "idvfs Enable/SWREQ cnt = 345/0", so
 * BIGIDVFSENABLE has run 345 times there, and Android's dmesg carries no
 * idvfs_error() at all -- so these succeed under the SAME ATF we are running.
 * The getters returned SMC_UNK when probed cold; calling the one the vendor
 * calls, with the arguments Android reports, is the test that matters.
 */
#define MTK_SIP_IDVFS_BIGIDVFSENABLE	0xC20003B0U
#define MTK_SIP_IDVFS_SRAMLDOSET	0xC20003BFU
#define IDVFS_CTRL_REG_ANDROID		0x0010a203U	/* from /proc/idvfs/dvt_test */
#define IS_SMC_UNK(v)			(((u32)(v)) == 0xFFFFFFFFU)

static long do_smc(const char *what, u32 fn, u64 a1, u64 a2, u64 a3)
{
	struct arm_smccc_res res;

	pr_emerg("a72-probe: SMC %s fn=0x%08x a1=0x%llx a2=0x%llx a3=0x%llx\n",
		 what, fn, a1, a2, a3);
	mdelay(20);
	arm_smccc_smc(fn, a1, a2, a3, 0, 0, 0, 0, &res);
	pr_emerg("a72-probe: SMC %s -> 0x%lx (%ld) %s\n", what,
		 (unsigned long)res.a0, (long)res.a0,
		 IS_SMC_UNK(res.a0) ? "<- SMC_UNK, not implemented" : "<- ANSWERED");
	return (long)res.a0;
}

/*
 * A park page of "wfi; b .-4", the same trick tools/psci-probe uses. A core
 * that actually starts lands here and stops, which proves the power-up without
 * needing the whole SMP bring-up path to be correct as well.
 */
#define AARCH64_WFI		0xd503207fu
#define AARCH64_B_BACK_ONE	0x17ffffffu

/*
 * A better park stub: announce, then park. Assembled with the cross toolchain
 * and checked against objdump, not hand-encoded.
 *
 *     movz x0, #0x5644 ; movk x0, #0x1101, lsl #16   -> CSPM_SW_RSV15
 *     movz w1, #0x72a7 ; movk w1, #0xa72a, lsl #16   -> 0xa72a72a7
 *     str  w1, [x0] ; dsb sy ; 1: wfi ; b 1b
 *
 * The A72 comes out of CPU_ON with MMU and caches off, so a plain store to a
 * known physical address is the one thing it can reliably do.
 *
 * THE MAILBOX HAS TO BE SOMETHING BOTH SIDES CAN ACTUALLY USE, and the first
 * attempt was not: it targeted CSPM_SW_RSV15, which turns out to be
 * unwritable — a kernel store of 0 left it reading 0xbabebabe. So the A72
 * could not have written it either, and the resulting "A72 never executed"
 * verdict was unsupported. A test whose two answers look identical is not a
 * test.
 *
 * The mailbox is now a dma_alloc_coherent() buffer: uncached on this side, a
 * plain physical address on the A72's side, and its address is patched into
 * the MOVZ/MOVK pair at runtime. Before trusting any result, the probe writes
 * and reads back the mailbox itself, so "the A72 did not write" can be
 * distinguished from "nothing could have".
 */
static const u32 a72_announce_stub[] = {
	0xd28ac880, 0xf2a22020, 0x528e54e1, 0x72b4e541,
	0xb9000001, 0xd5033f9f, 0xd503207f, 0x17ffffff,
};
#define A72_MAGIC		0xa72a72a7u

/* MOVZ Xd,#imm16 and MOVK Xd,#imm16,lsl#16 with Rd=0, as objdump confirmed */
#define MOVZ_X0(imm)	(0xd2800000u | (((imm) & 0xffffu) << 5))
#define MOVK_X0_16(imm)	(0xf2a00000u | (((imm) & 0xffffu) << 5))

#define SPM_PHYS		0x10006000UL
#define SPM_SIZE		0x1000
#define MP2_PWR_CON		0x218
#define EXT_BUCK_ISO		0x290
#define MP0_PWR_CON		0x210
#define PWR_STATUS		0x180
#define PWR_STATUS_2ND		0x184
#define CPU_PWR_STATUS		0x188
#define CPU_PWR_STATUS_2ND	0x18c

/* MP2_CPUSYS_PWR_CON bits, from the vendor's mt_spm_reg_mt6797.h */
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
#define MP1_PWR_CON		0x214

#define MCUCFG2_PHYS		0x10222000UL
#define MCUCFG2_SIZE		0x1000
#define IDVFS_DUMMY_OFF		0x4a0

#define DA9214_I2C_ADAPTER	2	/* i2c6 enumerates as adapter 2 */
#define DA9214_ADDR		0x68
#define DA9214_PAGE_CON		0x00
#define DA9214_BUCKB_CONT	0x5e

static int stage = 1;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "1 = report only, 2 = run the power-on sequence, 3 = also CPU_ON cpu8");

static void __iomem *spm;
static void __iomem *mcucfg2;

#define P(fmt, ...) pr_emerg("a72-probe: " fmt "\n", ##__VA_ARGS__)

static void report(const char *when)
{
	P("%-22s MP2=0x%08x  ISO=0x%08x  MP0=0x%08x  MP1=0x%08x", when,
	  readl(spm + MP2_PWR_CON), readl(spm + EXT_BUCK_ISO),
	  readl(spm + MP0_PWR_CON), readl(spm + MP1_PWR_CON));
	P("%-22s PWR_STATUS=0x%08x/0x%08x  CPU_PWR_STATUS=0x%08x/0x%08x", "",
	  readl(spm + PWR_STATUS), readl(spm + PWR_STATUS_2ND),
	  readl(spm + CPU_PWR_STATUS), readl(spm + CPU_PWR_STATUS_2ND));
}

/* mirrors da9214_config_interface(reg, val, mask, shift) */
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

static int enable_vproc2(void)
{
	struct i2c_adapter *ad = i2c_get_adapter(DA9214_I2C_ADAPTER);
	int r;

	if (!ad) {
		P("no i2c adapter %d", DA9214_I2C_ADAPTER);
		return -ENODEV;
	}

	/*
	 * Written straight at the chip rather than through the regulator, so
	 * the sequence matches the vendor's byte for byte. That does go behind
	 * da9211's regmap cache for BUCKB_CONT; harmless for an experiment,
	 * and the reason this is a probe module and not a driver.
	 */
	r = da9214_rmw(ad, DA9214_PAGE_CON, 0x0, 0xf, 0);
	if (r >= 0)
		r = da9214_rmw(ad, DA9214_BUCKB_CONT, 0x1, 0x1, 0);

	i2c_put_adapter(ad);
	return r < 0 ? r : 0;
}

static void power_on_buck(void)
{
	u32 v;

	P("step 1: MP2_CPUSYS_PWR_CON |= bit0");
	writel(readl(spm + MP2_PWR_CON) | BIT(0), spm + MP2_PWR_CON);

	P("step 2: dummy read of 0x102224a0 = 0x%08x",
	  readl(mcucfg2 + IDVFS_DUMMY_OFF));

	P("step 3: enable VPROC2 (DA9214 BUCKB_CONT bit0) — no watchdog latch, see header");
	if (enable_vproc2())
		P("  VPROC2 ENABLE FAILED — continuing anyway so the ISO result is still readable");
	mdelay(2);

	v = readl(spm + EXT_BUCK_ISO);
	P("step 4: EXT_BUCK_ISO is 0x%08x, clearing bits [1:0]", v);
	writel(v & ~0x3u, spm + EXT_BUCK_ISO);
	udelay(240);

	P("       (BigiDVFSSRAMLDOSet is issued below for stage >= 4)");
	report("after power-on seq");
}

static phys_addr_t park_pa;
static u32 *mbox;
static dma_addr_t mbox_dma;
static struct platform_device *mbox_pdev;

static int cpu_on_thread(void *unused)
{
	struct arm_smccc_res res;

	P("THREAD: issuing PSCI CPU_ON(mpidr 0x200, entry 0x%llx) — the call that spins",
	  (unsigned long long)park_pa);
	mdelay(50);
	arm_smccc_smc(PSCI_0_2_FN64_CPU_ON, 0x200, park_pa, 0, 0, 0, 0, 0, &res);
	P("THREAD: CPU_ON RETURNED %ld — it did not spin", (long)res.a0);
	return 0;
}

static int make_park_page(void)
{
	u32 *park = (u32 *)__get_free_page(GFP_KERNEL | GFP_DMA32);
	int w;

	if (!park)
		return -ENOMEM;
	for (w = 0; w < PAGE_SIZE / 4; w += 2) {
		park[w]     = AARCH64_WFI;
		park[w + 1] = AARCH64_B_BACK_ONE;
	}
	/* the announce stub goes at the entry point, with the mailbox patched in */
	memcpy(park, a72_announce_stub, sizeof(a72_announce_stub));
	park[0] = MOVZ_X0((u32)mbox_dma);
	park[1] = MOVK_X0_16((u32)mbox_dma >> 16);
	caches_clean_inval_pou((unsigned long)park, (unsigned long)park + PAGE_SIZE);
	park_pa = virt_to_phys(park);
	P("park page at VA %px PA 0x%llx (wfi; b .-4), deliberately leaked",
	  park, (unsigned long long)park_pa);
	return 0;
}

static int __init a72_probe_init(void)
{
	u32 before, v;
	int i;

	P("==== begin, stage=%d ====", stage);

	spm = ioremap(SPM_PHYS, SPM_SIZE);
	mcucfg2 = ioremap(MCUCFG2_PHYS, MCUCFG2_SIZE);
	if (!spm || !mcucfg2) {
		P("ioremap failed");
		goto out;
	}

	report("before anything");

	/*
	 * stage 0: is MCUCFG reachable at all?
	 *
	 * Every offset tried in MCUCFG (0x10200000) and MCUCFG2 (0x10222000)
	 * reads 0x00000000 from the non-secure world, including from here --
	 * and that region cannot be unpowered, because it holds the config for
	 * the clusters currently running this code. So it is access-protected.
	 *
	 * That matters because the remaining unported step of the vendor's A72
	 * power-on -- BigiDVFSSRAMLDOSet(), the big cluster's SRAM rail at
	 * 0x102222b0 -- lives there, and the vendor reaches it through an
	 * iDVFS SMC this ATF does not implement.
	 *
	 * The one cheap hypothesis left is that the Device APC block gates the
	 * bus and its clock is off in our tree (infra_device_apc, disabled).
	 * Enable it and look again.
	 */
	if (stage == 0) {
		struct of_phandle_args spec = { };
		struct clk *apc;
		int i;
		static const u32 offs[] = { 0x000, 0x274, 0x2b0, 0x4a0 };

		P("MCUCFG2 before touching the APC clock:");
		for (i = 0; i < ARRAY_SIZE(offs); i++)
			P("  0x10222%03x = 0x%08x", offs[i],
			  readl(mcucfg2 + offs[i]));

		spec.np = of_find_compatible_node(NULL, NULL,
						  "mediatek,mt6797-infracfg");
		if (!spec.np) {
			P("no infracfg node");
			goto out;
		}
		spec.args_count = 1;
		spec.args[0] = CLK_INFRA_DEVICE_APC;
		apc = of_clk_get_from_provider(&spec);
		of_node_put(spec.np);
		if (IS_ERR(apc)) {
			P("could not get infra_device_apc (%ld)", PTR_ERR(apc));
			goto out;
		}
		if (clk_prepare_enable(apc)) {
			P("could not enable infra_device_apc");
			goto out;
		}
		P("infra_device_apc ENABLED");
		udelay(100);

		P("MCUCFG2 after:");
		for (i = 0; i < ARRAY_SIZE(offs); i++)
			P("  0x10222%03x = 0x%08x", offs[i],
			  readl(mcucfg2 + offs[i]));

		/* write/read-back test on the MP2 sync-DCM register (MP2 is off) */
		writel(0x0000000f, mcucfg2 + 0x274);
		P("wrote 0x0f to 0x10222274, reads back 0x%08x — %s",
		  readl(mcucfg2 + 0x274),
		  readl(mcucfg2 + 0x274) ? "WRITABLE" : "still dropped");
		writel(0x0, mcucfg2 + 0x274);

		clk_disable_unprepare(apc);
		P("infra_device_apc released");
		goto out;
	}

	if (stage < 2) {
		P("stage 1 — reported only, nothing written.");
		goto out;
	}

	power_on_buck();

	if (stage >= 4) {
		/*
		 * Ask ATF to enable the big cluster's iDVFS, exactly as the
		 * vendor does, with the control word Android reports. If this
		 * answers, the service exists and the cold getter probe was
		 * misleading; if it is SMC_UNK, this ATF really does lack it.
		 */
		do_smc("BIGIDVFSSRAMLDOSET(110000)", MTK_SIP_IDVFS_SRAMLDOSET,
		       110000, 0, 0);
		do_smc("BIGIDVFSENABLE(ctrl,vproc,vsram)",
		       MTK_SIP_IDVFS_BIGIDVFSENABLE, IDVFS_CTRL_REG_ANDROID,
		       100000, 110000);
		report("after iDVFS enable");
	}

	if (stage < 3) {
		P("stage 2 — sequence run, no CPU_ON. MP2 will not move on its own;");
		P("ATF is what drives it, and nothing has asked ATF for anything yet.");
		goto out;
	}

	if (stage == 4) {
		P("stage 4 — iDVFS asked, no CPU_ON.");
		goto out;
	}

	/*
	 * stage 6: assert PWR_ON OURSELVES and watch the ack, instead of asking
	 * ATF to do it and losing the machine when it spins.
	 *
	 * This is the measurement that separates the two remaining stories. The
	 * ack ATF polls lives in PWR_STATUS/CPU_PWR_STATUS (SPM+0x180/0x188),
	 * not in PWR_CON. If a bit for MP2 appears there once PWR_ON is set,
	 * the power switch works and the fault is later in the sequence. If
	 * nothing moves, the domain is not getting its supply and no amount of
	 * sequencing will help.
	 *
	 * Nothing here calls PSCI, so nothing can spin at EL3.
	 */
	/*
	 * stage 7: drive the whole cluster MTCMOS sequence by hand, WITH the
	 * buck prerequisites in place.
	 *
	 * B-40 already did this once and the domain never acknowledged -- but
	 * that was before CPU_EXT_BUCK_ISO was known about, so it ran with the
	 * A72 supply still isolated. Repeating it now that VPROC2 is on, the
	 * isolation is cleared and the iDVFS SMCs have run is a different
	 * experiment with the same shape.
	 *
	 * Every poll is bounded and nothing calls PSCI, so the worst case is a
	 * report saying which step did not complete.
	 */
	if (stage == 7) {
		u32 v;
		int j;

#define STEP(desc, expr) do {						\
		P("  %-28s MP2=0x%08x PWRS=0x%08x/0x%08x CPUS=0x%08x",	\
		  desc, readl(spm + MP2_PWR_CON), readl(spm + PWR_STATUS), \
		  readl(spm + PWR_STATUS_2ND), readl(spm + CPU_PWR_STATUS)); \
		expr;							\
		udelay(50);						\
	} while (0)

		P("stage 7: hand-driven MP2 CPUTOP power-up (no PSCI)");
		mdelay(20);

		STEP("start", );
		STEP("PWR_ON=1",     writel(readl(spm + MP2_PWR_CON) | PWR_ON, spm + MP2_PWR_CON));
		STEP("PWR_ON_2ND=1", writel(readl(spm + MP2_PWR_CON) | PWR_ON_2ND, spm + MP2_PWR_CON));
		mdelay(1);
		STEP("PWR_CLK_DIS=0", writel(readl(spm + MP2_PWR_CON) & ~PWR_CLK_DIS, spm + MP2_PWR_CON));
		STEP("PWR_ISO=0",     writel(readl(spm + MP2_PWR_CON) & ~PWR_ISO, spm + MP2_PWR_CON));
		STEP("SRAM_PDN=0",    writel(readl(spm + MP2_PWR_CON) & ~SRAM_PDN, spm + MP2_PWR_CON));

		for (j = 0; j < 1000; j++) {
			if (!(readl(spm + MP2_PWR_CON) & SRAM_PDN_ACK))
				break;
			udelay(10);
		}
		P("  SRAM_PDN_ACK cleared after %d us (%s)", j * 10,
		  j < 1000 ? "OK" : "TIMEOUT");

		STEP("SRAM_ISOINT_B=1", writel(readl(spm + MP2_PWR_CON) | SRAM_ISOINT_B, spm + MP2_PWR_CON));
		STEP("SRAM_CKISO=0",    writel(readl(spm + MP2_PWR_CON) & ~SRAM_CKISO, spm + MP2_PWR_CON));
		STEP("SRAM_SLEEP_B=1",  writel(readl(spm + MP2_PWR_CON) | SRAM_SLEEP_B, spm + MP2_PWR_CON));
		STEP("PWR_RST_B=1",     writel(readl(spm + MP2_PWR_CON) | PWR_RST_B, spm + MP2_PWR_CON));
		mdelay(2);
		STEP("settled", );

		v = readl(spm + MP2_PWR_CON);
		P("MP2 = 0x%08x   MP0 (a running cluster) = 0x%08x", v,
		  readl(spm + MP0_PWR_CON));
		P("verdict: SRAM_PDN_ACK=%d SRAM_SLEEP_B_ACK=%d ISO=%d RST_B=%d",
		  !!(v & SRAM_PDN_ACK), !!(v & SRAM_SLEEP_B_ACK),
		  !!(v & PWR_ISO), !!(v & PWR_RST_B));
		P("the domain is %s",
		  (v & SRAM_SLEEP_B_ACK) && !(v & SRAM_PDN_ACK) && !(v & PWR_ISO)
			? "UP — this is what MP0 looks like"
			: "NOT up");
		goto out;
#undef STEP
	}

	/*
	 * stage 9: does the MP2 SRAM control respond at all, and does a clock
	 * change it?
	 *
	 * Stage 7 got MP2 to 0x0001004d -- one bit off a running cluster, the
	 * missing bit being SRAM_SLEEP_B_ACK. And at REST, MP2 sits with
	 * SRAM_PDN=1 and SRAM_PDN_ACK=0: a genuinely powered-down SRAM should
	 * be acknowledging that. Neither ack ever moves, which is the
	 * signature of SRAM control logic with no clock rather than one that
	 * disagrees with us.
	 *
	 * Cluster B's clock source is ARMPLLDIV_MUXSEL[1:0] in MCUMIXED, which
	 * unlike MCUCFG we CAN write, and it currently selects 0 = CLKSQ.
	 * Walk it through all four sources and watch the acks.
	 */
	/*
	 * stage 10: power MP2 by hand, then ask ATF for cpu8, while polling a
	 * scratch register the A72's entry stub writes.
	 *
	 * This exists because pre-powering the domain changed CPU_ON's failure
	 * from "spins forever" to "resets the SoC in ~300 ms" -- a different
	 * and more interesting failure, consistent with the core actually
	 * starting. The magic in CSPM_SW_RSV15 settles whether it does.
	 */
	if (stage == 10) {
		u32 seen = 0;

		mbox_pdev = platform_device_register_simple("gemini-a72-probe",
							   PLATFORM_DEVID_AUTO,
							   NULL, 0);
		if (IS_ERR(mbox_pdev)) {
			P("no staging device for the mailbox");
			goto out;
		}
		if (dma_coerce_mask_and_coherent(&mbox_pdev->dev, DMA_BIT_MASK(32))) {
			P("no 32-bit DMA mask");
			goto out;
		}
		mbox = dma_alloc_coherent(&mbox_pdev->dev, PAGE_SIZE, &mbox_dma,
					  GFP_KERNEL);
		if (!mbox) {
			P("no mailbox");
			goto out;
		}

		/* prove the mailbox works before believing anything it says */
		*mbox = 0;
		if (*mbox != 0) {
			P("mailbox will not clear — aborting, it could not tell us anything");
			goto out;
		}
		*mbox = 0x5a5a5a5a;
		if (*mbox != 0x5a5a5a5a) {
			P("mailbox will not hold a value — aborting");
			goto out;
		}
		*mbox = 0;
		P("mailbox at PA 0x%llx verified writable and readable, cleared to 0x%08x",
		  (unsigned long long)mbox_dma, *mbox);

		if (make_park_page()) {
			P("no park page");
			goto out;
		}
		P("stub patched: %08x %08x (mailbox 0x%llx)", ((u32 *)phys_to_virt(park_pa))[0],
		  ((u32 *)phys_to_virt(park_pa))[1], (unsigned long long)mbox_dma);

		/* hand power-up, same as stage 7 */
		writel(readl(spm + MP2_PWR_CON) | PWR_ON, spm + MP2_PWR_CON);
		udelay(50);
		writel(readl(spm + MP2_PWR_CON) | PWR_ON_2ND, spm + MP2_PWR_CON);
		mdelay(1);
		writel(readl(spm + MP2_PWR_CON) & ~PWR_CLK_DIS, spm + MP2_PWR_CON);
		writel(readl(spm + MP2_PWR_CON) & ~PWR_ISO, spm + MP2_PWR_CON);
		writel(readl(spm + MP2_PWR_CON) & ~SRAM_PDN, spm + MP2_PWR_CON);
		udelay(100);
		writel(readl(spm + MP2_PWR_CON) | SRAM_ISOINT_B, spm + MP2_PWR_CON);
		writel(readl(spm + MP2_PWR_CON) & ~SRAM_CKISO, spm + MP2_PWR_CON);
		writel(readl(spm + MP2_PWR_CON) | SRAM_SLEEP_B, spm + MP2_PWR_CON);
		writel(readl(spm + MP2_PWR_CON) | PWR_RST_B, spm + MP2_PWR_CON);
		mdelay(2);
		P("MP2 hand-powered to 0x%08x", readl(spm + MP2_PWR_CON));

		P("asking ATF for cpu8, entry 0x%llx, polling the scratch reg",
		  (unsigned long long)park_pa);
		mdelay(50);
		kthread_run(cpu_on_thread, NULL, "a72-cpuon");

		for (i = 0; i < 60; i++) {
			mdelay(10);
			v = *mbox;
			if (v == A72_MAGIC && !seen) {
				seen = 1;
				P("  *** +%d ms  MAILBOX = 0x%08x — THE A72 EXECUTED CODE ***",
				  (i + 1) * 10, v);
			}
			if ((i % 10) == 9)
				P("  +%3d ms mailbox=0x%08x MP2=0x%08x", (i + 1) * 10,
				  v, readl(spm + MP2_PWR_CON));
		}
		P("final mailbox = 0x%08x  (%s)", *mbox,
		  *mbox == A72_MAGIC ? "A72 RAN — the core fetched and executed"
				     : "A72 did not write — and the mailbox was proven working");
		goto out;
	}

	if (stage == 9) {
		void __iomem *mcumixed = ioremap(0x1001a000, 0x1000);
		static const char * const src[] = { "CLKSQ 26M", "ARMPLL",
						    "MAINPLL", "UNIVPLL" };
		u32 mux, v;
		int k;

		if (!mcumixed) {
			P("cannot map MCUMIXED");
			goto out;
		}

		P("stage 9: walking cluster B's clock mux, watching the SRAM acks");
		P("  MP2 = 0x%08x, MUXSEL = 0x%08x", readl(spm + MP2_PWR_CON),
		  readl(mcumixed + 0x270));

		for (k = 0; k < 4; k++) {
			mux = readl(mcumixed + 0x270);
			writel((mux & ~0x3u) | k, mcumixed + 0x270);
			mdelay(5);
			v = readl(spm + MP2_PWR_CON);
			P("  B mux = %u (%-9s)  MUXSEL=0x%08x  MP2=0x%08x  PDN_ACK=%d SLEEP_B_ACK=%d",
			  k, src[k], readl(mcumixed + 0x270), v,
			  !!(v & SRAM_PDN_ACK), !!(v & SRAM_SLEEP_B_ACK));
		}

		/* put it back where the bootloader had it */
		mux = readl(mcumixed + 0x270);
		writel(mux & ~0x3u, mcumixed + 0x270);
		P("  restored B mux to 0 (CLKSQ), MUXSEL = 0x%08x",
		  readl(mcumixed + 0x270));
		iounmap(mcumixed);
		report("after mux walk");
		goto out;
	}

	if (stage == 6) {
		u32 s0 = readl(spm + PWR_STATUS), s1 = readl(spm + CPU_PWR_STATUS);

		P("stage 6: asserting MP2 PWR_ON by hand, no PSCI involved");
		P("  before: PWR_STATUS=0x%08x CPU_PWR_STATUS=0x%08x MP2=0x%08x",
		  s0, s1, readl(spm + MP2_PWR_CON));
		mdelay(20);

		writel(readl(spm + MP2_PWR_CON) | BIT(2), spm + MP2_PWR_CON);

		for (i = 0; i < 20; i++) {
			u32 n0, n1;

			mdelay(10);
			n0 = readl(spm + PWR_STATUS);
			n1 = readl(spm + CPU_PWR_STATUS);
			P("  +%3d ms MP2=0x%08x PWR_STATUS=0x%08x(%s) CPU_PWR_STATUS=0x%08x(%s)",
			  (i + 1) * 10, readl(spm + MP2_PWR_CON),
			  n0, n0 != s0 ? "CHANGED" : "same",
			  n1, n1 != s1 ? "CHANGED" : "same");
		}

		P("verdict: PWR_STATUS %s, CPU_PWR_STATUS %s after PWR_ON",
		  readl(spm + PWR_STATUS) != s0 ? "MOVED — the switch acknowledges"
					        : "did NOT move — no power-good",
		  readl(spm + CPU_PWR_STATUS) != s1 ? "MOVED" : "did NOT move");

		/* put it back so we leave nothing half-asserted */
		writel(readl(spm + MP2_PWR_CON) & ~BIT(2), spm + MP2_PWR_CON);
		report("after clearing PWR_ON");
		goto out;
	}

	if (make_park_page()) {
		P("could not allocate the park page");
		goto out;
	}

	before = readl(spm + MP2_PWR_CON);
	P("stage 3: asking ATF for cpu8 while polling MP2 from here. MP2 = 0x%08x",
	  before);
	mdelay(50);

	kthread_run(cpu_on_thread, NULL, "a72-cpuon");

	for (i = 0; i < 40; i++) {
		mdelay(25);
		v = readl(spm + MP2_PWR_CON);
		P("  +%3d ms  MP2 = 0x%08x  ISO = 0x%08x%s", (i + 1) * 25, v,
		  readl(spm + EXT_BUCK_ISO),
		  v != before ? "   <-- MOVED" : "");
		if (v != before && (v & 0x8) /* PWR_ON_2ND */) {
			P("  PWR_ON_2ND SET — the power switch acknowledged");
			break;
		}
	}
	P("final MP2 = 0x%08x (was 0x%08x)", readl(spm + MP2_PWR_CON), before);

out:
	P("==== end, survived ====");
	if (spm)
		iounmap(spm);
	if (mcucfg2)
		iounmap(mcucfg2);
	return -EINVAL;
}

module_init(a72_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 A72 cluster power-on prerequisite (B-40)");
