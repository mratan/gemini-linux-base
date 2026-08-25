// SPDX-License-Identifier: GPL-2.0
/*
 * a72freq — change the running A72 cluster's frequency, the vendor's way.
 *
 * ARMCAXPLL2's MCUMIXED CON1/shadow interface is dead once the cluster is up:
 * writes land in the register and the PLL ignores them (measured 2026-08-24 —
 * CON1 read back 1001 MHz while the abist meter and a benchmark both said
 * 750). At runtime the big PLL listens to the MCUCFG-side iDVFS interface
 * (0x102224a0/a4), which is secure, and the firmware provides a SIP call that
 * performs the whole reprogram: MTK_SIP_KERNEL_IDVFS_BIGIDVFSPLLSETFREQ
 * (0xC20003B8, MHz in x1). Its implementation (freedomtan/atf-1.0-x20,
 * plat/mt6797/drivers/idvfs/mt_idvfs_api.c API_BIGPLLSETFREQ) disables the
 * PLL mid-change, so the caller must park the cluster on a backup clock
 * first. The vendor's own sequence (mt_cpufreq.c adjust_armpll_dds for
 * MT_CPU_DVFS_B) is:
 *
 *	CLK_MISC_CFG_0[5:4] = 3          TOPCKGEN: expose the backup mux input
 *	ARMPLLDIV_MUXSEL[1:0] = 2        cluster B onto MAINPLL-derived backup
 *	SMC 0xC20003B8 (MHz)             firmware reprograms the PLL
 *	ARMPLLDIV_MUXSEL[1:0] = 1        back onto the ARMPLL
 *	CLK_MISC_CFG_0[5:4] = 0
 *
 * with the CSPM hardware semaphore held around the MUXSEL writes, as ATF's
 * power_on_cl3 does. ATF chooses posdiv by range (<=500: /4, <=1000: /2,
 * else /1) so any 250..3000 MHz value is safe VCO-wise by its own table.
 *
 * Optionally raises the big SRAM LDO first (vsram_x100mv=): Android runs
 * Vsram at 1.20 V for the upper OPPs against the 1.10 V our bring-up sets.
 *
 * insmod gemini-a72freq.ko mhz=1001 [vsram_x100mv=120000]
 * Always fails to load (returns -EAGAIN) so nothing stays resident; the
 * result is in dmesg.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

#define P(fmt, ...) pr_emerg("a72freq: " fmt "\n", ##__VA_ARGS__)

#define SIP_PLLSETFREQ		0xC20003B8U
#define SIP_SRAMLDOSET		0xC20003BFU

#define TOPCK_PHYS		0x10000000
#define CLK_MISC_CFG_0		0x104

#define MCUMIXED_PHYS		0x1001a000
#define ARMPLLDIV_MUXSEL	0x270

#define CSPM_PHYS		0x11015000
#define CSPM_POWERON_CONFIG	0x000
#define CSPM_KEY		0x0b160001
#define CSPM_SEMA		0x448

static int mhz;
module_param(mhz, int, 0444);
MODULE_PARM_DESC(mhz, "target A72 cluster frequency in MHz (250..3000)");

static int vsram_x100mv;
module_param(vsram_x100mv, int, 0444);
MODULE_PARM_DESC(vsram_x100mv, "first raise the big SRAM LDO to this (e.g. 120000 = 1.20 V)");

static int __init a72freq_init(void)
{
	void __iomem *topck, *mix, *cspm;
	struct arm_smccc_res res;
	u32 mux, misc;
	bool sema = false;
	int i;

	if (mhz < 250 || mhz > 3000) {
		P("REFUSING mhz=%d (250..3000)", mhz);
		return -EINVAL;
	}
	if (!cpu_online(8) && !cpu_online(9)) {
		P("REFUSING: no A72 online — the firmware call needs the cluster up");
		return -EINVAL;
	}

	topck = ioremap(TOPCK_PHYS, 0x1000);
	mix = ioremap(MCUMIXED_PHYS, 0x1000);
	cspm = ioremap(CSPM_PHYS, 0x1000);
	if (!topck || !mix || !cspm)
		goto out;

	if (vsram_x100mv) {
		arm_smccc_smc(SIP_SRAMLDOSET, vsram_x100mv, 0, 0, 0, 0, 0, 0, &res);
		P("SRAM LDO -> %d (rc %ld)", vsram_x100mv, (long)res.a0);
		udelay(240);
	}

	writel(CSPM_KEY, cspm + CSPM_POWERON_CONFIG);
	for (i = 0; i < 200; i++) {
		writel(1, cspm + CSPM_SEMA);
		if (readl(cspm + CSPM_SEMA) & 1) {
			sema = true;
			break;
		}
		udelay(10);
	}
	if (!sema) {
		P("REFUSING: CSPM semaphore not granted");
		goto out;
	}

	misc = readl(topck + CLK_MISC_CFG_0);
	mux = readl(mix + ARMPLLDIV_MUXSEL);
	P("before: MUXSEL=%08x MISC=%08x", mux, misc);

	writel(misc | (3u << 4), topck + CLK_MISC_CFG_0);
	writel((mux & ~3u) | 2u, mix + ARMPLLDIV_MUXSEL);	/* B -> backup */
	udelay(2);

	arm_smccc_smc(SIP_PLLSETFREQ, mhz, 0, 0, 0, 0, 0, 0, &res);

	writel((readl(mix + ARMPLLDIV_MUXSEL) & ~3u) | 1u,
	       mix + ARMPLLDIV_MUXSEL);				/* B -> ARMPLL */
	writel(readl(topck + CLK_MISC_CFG_0) & ~(3u << 4),
	       topck + CLK_MISC_CFG_0);

	writel(1, cspm + CSPM_SEMA);				/* release */

	P("PLLSETFREQ(%d MHz) rc=%ld  MUXSEL=%08x", mhz, (long)res.a0,
	  readl(mix + ARMPLLDIV_MUXSEL));

out:
	if (topck)
		iounmap(topck);
	if (mix)
		iounmap(mix);
	if (cspm)
		iounmap(cspm);
	P("==== end ====");
	return -EAGAIN;
}

module_init(a72freq_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("runtime A72 cluster frequency change via the firmware's own SIP call");
