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

#define SPM_PHYS		0x10006000UL
#define SPM_SIZE		0x1000
#define MP2_PWR_CON		0x218
#define EXT_BUCK_ISO		0x290
#define MP0_PWR_CON		0x210
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
	P("%-22s MP2=0x%08x  EXT_BUCK_ISO=0x%08x  MP0=0x%08x  MP1=0x%08x", when,
	  readl(spm + MP2_PWR_CON), readl(spm + EXT_BUCK_ISO),
	  readl(spm + MP0_PWR_CON), readl(spm + MP1_PWR_CON));
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

	P("       (skipping BigiDVFSSRAMLDOSet — this ATF has no iDVFS SIP)");
	report("after power-on seq");
}

static phys_addr_t park_pa;

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
	u32 *park = (u32 *)__get_free_page(GFP_KERNEL);
	int w;

	if (!park)
		return -ENOMEM;
	for (w = 0; w < PAGE_SIZE / 4; w += 2) {
		park[w]     = AARCH64_WFI;
		park[w + 1] = AARCH64_B_BACK_ONE;
	}
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

	if (stage == 4 || stage == 5) {
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
