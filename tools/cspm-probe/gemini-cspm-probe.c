// SPDX-License-Identifier: GPL-2.0-only
/*
 * gemini-cspm-probe — bring up MT6797's CPU DVFS co-processor, in stages, to
 * answer one question: does the A72 cluster's power switch acknowledge once
 * CSPM is running?
 *
 * WHY THIS EXISTS
 *
 * B-40: PSCI CPU_ON for cpu8 hangs inside ATF, which sets MP2_CPUTOP_PWR_ON
 * and then spins forever on a power-good that never asserts. Driving the whole
 * MTCMOS sequence by hand does not make it acknowledge either. Android powers
 * that cluster through CSPM — its log says so directly:
 *
 *     [CPUHVFS] (0) [0018f295] cluster2 on,  pause = 0x0, swctrl = 0x25f0
 *
 * and the vendor driver shows the mechanism: cluster power is requested by
 * setting CLUSTER_EN (bit 14) in CSPM_SW_RSV2, which the PCM image running on
 * the co-processor polls and acts on. No PCM, nobody acting.
 *
 * A cheaper route was eliminated first: the Big cluster's *frequency* control
 * (mt_idvfs.c) goes through a secure SIP family 0xC20003B0.., but our ATF
 * returns SMC_UNK for every one of them — identically to a deliberately bogus
 * id, which is the control that makes that reading mean something. There is no
 * SMC shortcut. See tools/psci-probe.
 *
 * WHY THIS IS SAFE TO RUN, WHICH IS THE PART THAT TOOK THE LONGEST TO ESTABLISH
 *
 * The obvious fear is that a DVFS co-processor, once started, fights
 * mediatek-cpufreq for the CPU rails over i2c6 — the bus the CPU regulator has
 * sat behind since #51, in the path of every frequency transition.
 *
 * It cannot, and the vendor's own code is why. CSPM only ever touches i2c when
 * the PCM is UNPAUSED: __cspm_unpause_pcm_to_run() calls clk_enable(i2c_clk)
 * and the pause path calls clk_disable(i2c_clk). The kick sequence itself sets
 * SW_PAUSE on every cluster before starting the PCM, and PSF_PAUSE_INIT keeps
 * it that way until something explicitly unpauses.
 *
 * This module never unpauses, and never asks for the i2c clock at all. The
 * co-processor runs with its bus master gated off. That is a property of the
 * hardware path, not a mitigation layered on top of it.
 *
 * STAGES — each is a separate insmod, so a step that hangs can be stepped over
 * and the one before it is known to have completed.
 *
 *   stage=1  (default)  map, reset the PCM, load the image into IM, and READ
 *                       IT BACK through the IM host port to verify it. Does
 *                       NOT start the PCM. Nothing executes.
 *   stage=2             everything in 1, then kick the PCM to run with every
 *                       cluster paused.
 *   stage=3             everything in 2, then set CLUSTER_EN on cluster B and
 *                       watch MP2_CPUSYS_PWR_CON.
 *   stage=4             everything in 3, then UNPAUSE cluster B.
 *   stage=5             attach to an already-running stage-2 PCM, then enable
 *                       and unpause B without resetting/reloading the PCM.
 *
 * Stage 3 turned out not to be enough, and the vendor code says why: setting
 * CLUSTER_EN while paused only records the request. cspm_cluster_notify_on()
 * keeps SW_PAUSE when pause_src_map is non-zero, and it is
 * __cspm_unpause_pcm_to_run() that walks the clusters, skips any without
 * CLUSTER_EN, and clears SW_PAUSE on the rest. Measured: CLUSTER_EN latched
 * (SW_RSV2 = 0x70f0) and MP2_CPUSYS_PWR_CON did not move for 200 ms.
 *
 * WHAT STAGE 4 COSTS, AND WHY IT IS STILL BOUNDED. Unpausing needs the i2c
 * clock, so the argument that CSPM cannot reach the bus stops applying. Two
 * things keep it bounded anyway. Only cluster B carries CLUSTER_EN, and the
 * vendor's unpause loop skips clusters without it -- so LL and L stay paused
 * and mediatek-cpufreq keeps VPROC1 to itself. And the caller is expected to
 * pin both A53 policies to userspace first, so Linux issues no i2c traffic of
 * its own during the window.
 *
 * Stage 1 exists because of the one genuinely dangerous unknown: the IM
 * fetches the image from DRAM by PHYSICAL address, and on a 4GB part the
 * vendor adds 0x40000000 to it (MAPPING_DRAM_ACCESS_ADDR, "for SPM and MD32
 * only"). Get that wrong and the co-processor executes whatever else is in
 * memory. Reading the image back out of IM before starting it turns that from
 * a hope into a check.
 *
 * WHAT SUCCESS LOOKS LIKE. MP2_CPUSYS_PWR_CON (0x10006218) today goes
 * 0x00010132 -> 0x00010136 under ATF and stops: PWR_ON set, PWR_ON_2ND never
 * reached. If it moves past 0x136 with the PCM running, CSPM is the missing
 * piece.
 *
 * Copyright (c) 2026 Gemini PDA Linux port
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>

#include "gemini-cspm-fw.h"

#define CSPM_PHYS		0x11015000UL
#define CSPM_SIZE		0x1000
#define CSRAM_PHYS		0x0012a000UL
#define CSRAM_SIZE		0x3000
#define MP2_CPUSYS_PWR_CON	0x10006218UL

/* the 4GB-mode DRAM alias the vendor applies for SPM masters */
#define SPM_DRAM_MAP_OFFSET	0x40000000ULL
#define SPM_DRAM_MAP_LIMIT	(SPM_DRAM_MAP_OFFSET + 0x80000000ULL)

#define PROJECT_CODE		0xb16

#define POWERON_CONFIG_EN	0x000
#define POWER_ON_VAL1		0x008
#define PCM_CON0		0x018
#define PCM_CON1		0x01c
#define PCM_IM_PTR		0x020
#define PCM_IM_LEN		0x024
#define PCM_REG_DATA_INI	0x028
#define PCM_PWR_IO_EN		0x02c
#define PCM_TIMER_VAL		0x030
#define PCM_IM_HOST_RW_PTR	0x038
#define PCM_IM_HOST_RW_DAT	0x03c
#define PCM_EVENT_VECTOR(n)	(0x040 + (n) * 4)
#define PCM_EVENT_VECTOR_EN	0x080
#define SW_INT_CLEAR		0x094
#define IRQ_MASK		0x0b4
#define WAKEUP_EVENT_MASK	0x0c4
#define PCM_FSM_STA		0x178
#define SEMA1_M1		0x424
#define M0_REC(n)		(0x300 + (n) * 4)
#define M1_REC(n)		(0x350 + (n) * 4)
#define M2_REC(n)		(0x3a0 + (n) * 4)
#define SW_RSV(n)		(0x608 + (n) * 4)
#define RSV_CON			0x648

#define REGWR_EN		BIT(0)
#define REGWR_CFG_KEY		(PROJECT_CODE << 16)
#define CON0_PCM_KICK		BIT(0)
#define CON0_IM_KICK		BIT(1)
#define CON0_PCM_CK_EN		BIT(2)
#define CON0_PCM_SW_RESET	BIT(15)
#define CON0_CFG_KEY		(PROJECT_CODE << 16)
#define CON1_IM_SLAVE		BIT(0)
#define CON1_MIF_APBEN		BIT(3)
#define CON1_PCM_TIMER_EN	BIT(5)
#define CON1_IM_NONRP_EN	BIT(6)
#define CON1_SPM_SRAM_SLP_B	BIT(10)
#define CON1_SPM_SRAM_ISO_B	BIT(11)
#define CON1_EVENT_LOCK_EN	BIT(12)
#define CON1_CFG_KEY		(PROJECT_CODE << 16)
#define PCM_PWRIO_EN_R7		BIT(7)
#define PCM_RF_SYNC_R7		BIT(23)
#define R7_PCM_TIMER_SET	BIT(9)
#define POWER_ON_VAL1_DEF	0x20
#define PCM_FSM_STA_DEF		0x48490
#define FSM_IM_REDY		BIT(9)

#define IM_HOST_EN		BIT(31)
#define IM_HOST_W_EN		BIT(30)

/* swctrl / hwsta bit layout, from the vendor driver */
#define SW_F_MIN(v)		(((v) & 0xf) << 0)
#define SW_F_MAX(v)		(((v) & 0xf) << 4)
#define SW_F_DES(v)		(((v) & 0xf) << 8)
#define SW_F_ASSIGN		BIT(12)
#define SW_PAUSE		BIT(13)
#define CLUSTER_EN		BIT(14)
#define F_CURR(v)		(((v) & 0xf) << 0)
#define V_CURR(v)		(((v) & 0x7f) << 8)
#define VS_CURR(v)		(((v) & 0x7f) << 16)

#define NUM_CPU_OPP		16
#define NUM_PHY_CLUSTER		3	/* LL, L, B -- CCI is virtual */
#define NUM_CPU_CLUSTER		4

/* CSRAM offsets the firmware and the vendor driver agree on */
#define OFFS_INIT_OPP		0x02e0
#define OFFS_INIT_FREQ		0x02f0
#define OFFS_INIT_VOLT		0x0300
#define OFFS_INIT_VSRAM		0x0310
#define OFFS_SW_RSV0		0x0320
#define OFFS_PAUSE_SRC		0x0330
#define OFFS_FW_RSV3		0x02b0

/*
 * The live rail state, in the firmware's own encodings. Defaults are the
 * values measured on this board; override per run if the rails have moved.
 *   v_*  : DA9214 vosel, (mV*100 - 30000 + 999)/1000   -- 0x4d = 1070 mV
 *   vs_* : SRAM LDO vosel, (mV*100 - 90000 + 2499)/2500 + 3 -- 0xf = 1200 mV
 *   f_*  : SOFTWARE opp index, 0 = highest of the 16
 */
static int v_ll = 0x4d, v_l = 0x4d, v_cci = 0x4d, v_b = 0x58;
static int vs_ll = 0xf, vs_l = 0xf, vs_cci = 0xf, vs_b = 0xb;
static int f_ll = 15, f_l = 15, f_cci = 15, f_b = 15;
static int freq_ll_khz, freq_l_khz, freq_b_khz, freq_cci_khz;
module_param(v_ll, int, 0444);   module_param(v_l, int, 0444);
module_param(v_cci, int, 0444);  module_param(v_b, int, 0444);
module_param(vs_ll, int, 0444);  module_param(vs_l, int, 0444);
module_param(vs_cci, int, 0444); module_param(vs_b, int, 0444);
module_param(f_ll, int, 0444);   module_param(f_l, int, 0444);
module_param(f_cci, int, 0444);  module_param(f_b, int, 0444);
module_param(freq_ll_khz, int, 0444);  module_param(freq_l_khz, int, 0444);
module_param(freq_b_khz, int, 0444);   module_param(freq_cci_khz, int, 0444);

static int clear_b_assign_after_kick;
module_param(clear_b_assign_after_kick, int, 0444);
MODULE_PARM_DESC(clear_b_assign_after_kick,
	"clear B's pending SW_F_ASSIGN while it is still paused, matching Android's offline 0x26f0 state");

static int stage = 1;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage,
	"1 load+verify; 2 kick paused; 3 enable B paused; 4 unpause B; "
	"5 attach to existing stage 2 and unpause B without resetting PCM");

static void __iomem *cspm;
static void __iomem *csram;
static void __iomem *mp2_pwr;
static struct clk *i2c_clk;
static void *fw_buf;
static dma_addr_t fw_dma;
static struct platform_device *fw_pdev;
static size_t fw_bytes;

#define P(fmt, ...) pr_emerg("cspm-probe: " fmt "\n", ##__VA_ARGS__)

static inline u32 cr(u32 off)		{ return readl(cspm + off); }
static inline void cw(u32 off, u32 v)	{ writel(v, cspm + off); }

/* opp index 0 is the highest frequency in software, 15 in firmware */
static inline u32 opp_sw_to_fw(u32 i)	{ return i < NUM_CPU_OPP ? (NUM_CPU_OPP - 1) - i : 0; }

static bool pcm_sw_reset(void)
{
	u32 sta;

	cw(PCM_CON0, CON0_CFG_KEY | CON0_PCM_SW_RESET);
	cw(PCM_CON0, CON0_CFG_KEY);
	udelay(10);

	sta = cr(PCM_FSM_STA);
	if (sta != PCM_FSM_STA_DEF) {
		P("PCM RESET FAILED: FSM_STA = 0x%x, expected 0x%x", sta, PCM_FSM_STA_DEF);
		return false;
	}
	P("PCM reset OK, FSM_STA = 0x%x", sta);
	return true;
}

static void cspm_register_init(void)
{
	cw(POWERON_CONFIG_EN, REGWR_CFG_KEY | REGWR_EN);
	cw(POWER_ON_VAL1, POWER_ON_VAL1_DEF | R7_PCM_TIMER_SET);
	cw(PCM_PWR_IO_EN, 0);

	cw(PCM_CON0, CON0_CFG_KEY | CON0_PCM_SW_RESET);
	cw(PCM_CON0, CON0_CFG_KEY);
	udelay(10);

	cw(PCM_CON0, CON0_CFG_KEY | CON0_PCM_CK_EN);
	cw(PCM_CON1, CON1_CFG_KEY | CON1_EVENT_LOCK_EN | CON1_SPM_SRAM_ISO_B |
		     CON1_SPM_SRAM_SLP_B | CON1_MIF_APBEN);
	cw(PCM_IM_PTR, 0);
	cw(PCM_IM_LEN, 0);

	cw(WAKEUP_EVENT_MASK, ~0u);
	cw(IRQ_MASK, 0x30f);		/* IRQM_ALL */
	cw(SW_INT_CLEAR, 0xf);
}

/*
 * Read the image back out of instruction memory through the host port and
 * compare it word for word against what we asked the IM to fetch. This is the
 * check that the physical address handed to the IM was the one the IM can
 * actually reach.
 */
static int verify_im(void)
{
	int i, bad = 0;
	u32 got;

	for (i = 0; i < CSPM_FW_SIZE; i++) {
		cw(PCM_IM_HOST_RW_PTR, IM_HOST_EN | i);
		got = cr(PCM_IM_HOST_RW_DAT);
		if (got != cspm_dvfs_binary[i]) {
			if (bad < 4)
				P("  IM[%4d] = 0x%08x, expected 0x%08x", i, got,
				  cspm_dvfs_binary[i]);
			bad++;
		}
	}
	cw(PCM_IM_HOST_RW_PTR, 0);	/* release the host port */
	return bad;
}

static int kick_im_to_fetch(void)
{
	phys_addr_t pa = (phys_addr_t)fw_dma;
	u64 spm_pa = pa;
	u32 con0;
	int i;

	if (spm_pa >= SPM_DRAM_MAP_OFFSET && spm_pa < SPM_DRAM_MAP_LIMIT)
		spm_pa += SPM_DRAM_MAP_OFFSET;

	P("image at va=%p pa=0x%llx -> SPM sees 0x%llx, %d words",
	  fw_buf, (u64)pa, spm_pa, CSPM_FW_SIZE);

	cw(PCM_IM_PTR, (u32)spm_pa);
	cw(PCM_IM_LEN, CSPM_FW_SIZE - 1);

	/* the vendor takes the EMI semaphore around an IM fetch; so do we */
	for (i = 0; i < 10000; i++) {
		writel(0x1, cspm + SEMA1_M1);
		if (readl(cspm + SEMA1_M1) & 0x1)
			break;
		udelay(10);
	}
	if (i >= 10000) {
		P("EMI SEMA1 GET TIMEOUT — refusing to fetch");
		return -EBUSY;
	}

	con0 = cr(PCM_CON0) & ~(CON0_IM_KICK | CON0_PCM_KICK);
	cw(PCM_CON0, con0 | CON0_CFG_KEY | CON0_IM_KICK);
	cw(PCM_CON0, con0 | CON0_CFG_KEY);

	for (i = 0; i < 100000; i++) {
		if (cr(PCM_FSM_STA) & FSM_IM_REDY)
			break;
		udelay(1);
	}
	writel(0x1, cspm + SEMA1_M1);	/* release EMI SEMA */

	if (i >= 100000) {
		P("IM FETCH TIMEOUT, FSM_STA = 0x%x", cr(PCM_FSM_STA));
		return -ETIMEDOUT;
	}
	P("IM fetch done after %d us, FSM_STA = 0x%x", i, cr(PCM_FSM_STA));
	return 0;
}

/*
 * The register preamble the PCM image expects, transcribed from the vendor's
 * __cspm_kick_pcm_to_run(). These are addresses and masks the firmware reads
 * out of the M0/M1/M2 record registers; they are firmware ABI, not something
 * to derive.
 */
static void init_fw_abi_registers(void)
{
	static const u32 m0[] = { 0x1001af34, 0x1001af38, 0x1001af3c, 0x1001af40,
				  0x1001af44, 0x1001a204, 0x1 };
	static const u32 m1[] = { 0x200, 0xffffc1ff, 0x4, 0x8, 0xfffffff3,
				  0x1001af48, 0x1001af4c, 0x1001af50, 0x1001af54,
				  0x1001af58, 0x1001a214, 0x2, 0x20000, 0xffc1ffff,
				  0x10, 0x20, 0xffffffcf, 0x1001af5c, 0x1001af60,
				  0x1001af64 };
	static const u32 m2[] = { 0x1001af68, 0x1001af6c, 0x1001a224, 0x4, 0x2,
				  0xffffffc1, 0x40, 0x80, 0xffffff3f };
	int i;

	for (i = 0; i < ARRAY_SIZE(m0); i++)	/* M0_REC13..19 */
		cw(M0_REC(13 + i), m0[i]);
	for (i = 0; i < ARRAY_SIZE(m1); i++)	/* M1_REC0..19 */
		cw(M1_REC(i), m1[i]);
	for (i = 0; i < ARRAY_SIZE(m2); i++)	/* M2_REC0..8 */
		cw(M2_REC(i), m2[i]);

	/* log ring pointers in CSRAM; harmless but the FW expects them set */
	for (i = 0; i < 6; i++)
		cw(SW_RSV(7 + i), 0xa000 + 0x03d0 + i * 4);

	cw(RSV_CON, 0x0);

	cw(PCM_EVENT_VECTOR(1),  0x3e60000);
	cw(PCM_EVENT_VECTOR(6),  0x3e60000);
	cw(PCM_EVENT_VECTOR(11), 0x3e60000);
	cw(PCM_EVENT_VECTOR(4),  0x0);
	cw(PCM_EVENT_VECTOR(9),  0x0);
	cw(PCM_EVENT_VECTOR(14), 0x0);
}

static void kick_pcm_to_run(void)
{
	u32 con0;
	int i;

	init_fw_abi_registers();

	/*
	 * Every cluster starts PAUSED, exactly as the vendor kicks it. This is
	 * what keeps the co-processor off i2c6 and out of DVFS.
	 *
	 * Ceiling 0 / floor 15 is the full range in software indices. DES must
	 * match the seeded current OPP. This used to hard-code DES=15 for every
	 * cluster, so changing f_b changed only HWSTA while the firmware still
	 * drove cluster B to the lowest OPP when unpaused.
	 */
	{
		u32 f[NUM_PHY_CLUSTER] = { f_ll, f_l, f_b };

		for (i = 0; i < NUM_PHY_CLUSTER; i++) {
			u32 swctrl = SW_F_MAX(opp_sw_to_fw(0)) |
				     SW_F_MIN(opp_sw_to_fw(NUM_CPU_OPP - 1)) |
				     SW_F_DES(opp_sw_to_fw(f[i])) |
				     SW_F_ASSIGN | SW_PAUSE;
			cw(SW_RSV(i), swctrl);
			writel(swctrl, csram + OFFS_SW_RSV0 + i * 4);
			P("  cluster%d swctrl = 0x%08x (opp %u -> fw %u, SW_PAUSE set)",
			  i, cr(SW_RSV(i)), f[i], opp_sw_to_fw(f[i]));
		}
	}

	/*
	 * THE CURRENT-STATE WORDS, AND THE BUG THAT KILLED THE MACHINE.
	 *
	 * This used to write V_CURR(0) | VS_CURR(0) — telling the firmware every
	 * cluster is sitting at 0 V. An unpaused firmware that believes that will
	 * drive the PMIC to "correct" it, and VPROC is BUCKA: LL, L and CCI, the
	 * clusters this kernel is running on. Clearing SW_PAUSE with those words
	 * in place killed the SoC instantly, three times out of three, with
	 * netconsole cut off mid-printk — a rail event, not a hang.
	 *
	 * The vendor seeds the truth (mt_cpufreq.c __set_cpuhvfs_init_sta):
	 *
	 *   F_CURR(opp_sw_to_fw(sta->opp[i])) | V_CURR(sta->volt[i])
	 *                                     | VS_CURR(sta->vsram[i])
	 *
	 * with volt = VOLT_TO_EXTBUCK_VAL()  = (mV*100 - 30000 + 999)/1000, the
	 *                                      DA9214 vosel code, and
	 *      vsram = VOLT_TO_PMIC_VAL()    = (mV*100 - 90000 + 2499)/2500 + 3,
	 *                                      the SRAM LDO vosel.
	 * Both were checked against live register reads: BUCKA at 1070 mV really
	 * does read 0x4d, and the on-die VSRAM LDO at 1200 mV really does read
	 * 0xf.
	 *
	 * These come in as module parameters rather than being read here, so the
	 * values can be computed from the live rails and eyeballed before the
	 * co-processor is ever unpaused.
	 */
	{
		static const char *nm[NUM_CPU_CLUSTER] = { "LL", "L", "B", "CCI" };
		u32 v[NUM_CPU_CLUSTER]  = { v_ll, v_l, v_b, v_cci };
		u32 vs[NUM_CPU_CLUSTER] = { vs_ll, vs_l, vs_b, vs_cci };
		u32 f[NUM_CPU_CLUSTER]  = { f_ll, f_l, f_b, f_cci };
		u32 khz[NUM_CPU_CLUSTER] = {
			freq_ll_khz, freq_l_khz, freq_b_khz, freq_cci_khz
		};

		for (i = 0; i < NUM_CPU_CLUSTER; i++) {
			/*
			 * Firmware ABI metadata written by vendor
			 * __cspm_check_and_update_sta(). Omitting these left reset
			 * garbage in CSRAM; cluster-off paths happened to survive, but
			 * the first powered cluster-on reset the SoC.
			 */
			writel(f[i], csram + OFFS_INIT_OPP + i * sizeof(u32));
			writel(khz[i], csram + OFFS_INIT_FREQ + i * sizeof(u32));
			writel(v[i], csram + OFFS_INIT_VOLT + i * sizeof(u32));
			writel(vs[i], csram + OFFS_INIT_VSRAM + i * sizeof(u32));

			cw(SW_RSV(3 + i), F_CURR(opp_sw_to_fw(f[i])) |
					  V_CURR(v[i]) | VS_CURR(vs[i]));
			writel(cr(SW_RSV(3 + i)),
			       csram + OFFS_FW_RSV3 + i * sizeof(u32));
			P("  %-3s hwsta = 0x%08x  (opp %u -> fw %u, %u kHz, vproc 0x%02x, vsram 0x%02x)",
			  nm[i], cr(SW_RSV(3 + i)), f[i], opp_sw_to_fw(f[i]),
			  khz[i], v[i], vs[i]);
		}
	}

	writel(BIT(0) /* PSF_PAUSE_INIT */, csram + OFFS_PAUSE_SRC);

	/* PCM timer as a free-run counter; wake sources masked off */
	cw(PCM_TIMER_VAL, 0xffffffff);
	cw(PCM_CON1, cr(PCM_CON1) | CON1_CFG_KEY | CON1_PCM_TIMER_EN);
	cw(WAKEUP_EVENT_MASK, ~0u);
	cw(IRQ_MASK, 0x30f);

	/* r7 controls power, as the firmware expects */
	cw(PCM_REG_DATA_INI, cr(POWER_ON_VAL1));
	cw(PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	cw(PCM_PWR_IO_EN, 0);
	cw(PCM_EVENT_VECTOR(0), 0);
	cw(PCM_EVENT_VECTOR_EN, 0);
	cw(PCM_PWR_IO_EN, PCM_PWRIO_EN_R7);

	P("about to KICK the PCM — everything after this line is the co-processor running");
	mdelay(50);

	con0 = cr(PCM_CON0) & ~(CON0_IM_KICK | CON0_PCM_KICK);
	cw(PCM_CON0, con0 | CON0_CFG_KEY | CON0_PCM_KICK);
	cw(PCM_CON0, con0 | CON0_CFG_KEY);
}

static void report_pcm_alive(const char *when)
{
	P("%s: FSM_STA=0x%08x PCM_TIMER=0x%08x SW_RSV0=0x%08x SW_RSV1=0x%08x SW_RSV2=0x%08x",
	  when, cr(PCM_FSM_STA), cr(0x150), cr(SW_RSV(0)), cr(SW_RSV(1)), cr(SW_RSV(2)));
}

static int __init cspm_probe_init(void)
{
	int bad, i;
	u32 before, v;

	P("==== begin, stage=%d, fw=%s (%d words) ====", stage, CSPM_FW_VERSION,
	  CSPM_FW_SIZE);

	/* Validate every firmware bitfield before resetting or starting CSPM. */
	if (f_ll < 0 || f_ll >= NUM_CPU_OPP ||
	    f_l < 0 || f_l >= NUM_CPU_OPP ||
	    f_b < 0 || f_b >= NUM_CPU_OPP ||
	    f_cci < 0 || f_cci >= NUM_CPU_OPP) {
		P("REFUSING invalid OPP seed: f_ll=%d f_l=%d f_b=%d f_cci=%d",
		  f_ll, f_l, f_b, f_cci);
		return -EINVAL;
	}
	if (v_ll < 0 || v_ll > 0x5a || v_l < 0 || v_l > 0x5a ||
	    v_cci < 0 || v_cci > 0x5a || v_b < 0 || v_b > 0x58 ||
	    vs_ll < 0 || vs_ll > 0x7f || vs_l < 0 || vs_l > 0x7f ||
	    vs_cci < 0 || vs_cci > 0x7f || vs_b < 0 || vs_b > 0x7f) {
		P("REFUSING invalid voltage seed");
		return -EINVAL;
	}
	if (stage >= 2 && stage <= 4 &&
	    (freq_ll_khz <= 0 || freq_ll_khz > 3000000 ||
	     freq_l_khz <= 0 || freq_l_khz > 3000000 ||
	     freq_b_khz <= 0 || freq_b_khz > 3000000 ||
	     freq_cci_khz <= 0 || freq_cci_khz > 3000000)) {
		P("REFUSING missing/invalid physical-frequency metadata: LL=%d L=%d B=%d CCI=%d",
		  freq_ll_khz, freq_l_khz, freq_b_khz, freq_cci_khz);
		return -EINVAL;
	}

	cspm = ioremap(CSPM_PHYS, CSPM_SIZE);
	csram = ioremap(CSRAM_PHYS, CSRAM_SIZE);
	mp2_pwr = ioremap(MP2_CPUSYS_PWR_CON, 4);
	if (!cspm || !csram || !mp2_pwr) {
		P("ioremap failed");
		goto out;
	}

	P("CSPM alive check: POWERON_CONFIG_EN=0x%08x PCM_CON0=0x%08x PCM_CON1=0x%08x FSM_STA=0x%08x",
	  cr(POWERON_CONFIG_EN), cr(PCM_CON0), cr(PCM_CON1), cr(PCM_FSM_STA));
	P("MP2_CPUSYS_PWR_CON = 0x%08x (before anything)", readl(mp2_pwr));

	/*
	 * Attach to the stage-2 PCM without resetting it. This is the missing
	 * experiment: power CPUTOP first while B is disabled (which acks), then
	 * let the already-running PCM see CLUSTER_EN. Re-running stage 4 here
	 * would reset/reload the PCM and lose the ordering being tested.
	 */
	if (stage == 5) {
		struct device_node *np;
		u32 ll = cr(SW_RSV(0)), l = cr(SW_RSV(1)), b = cr(SW_RSV(2));

		if ((ll & SW_PAUSE) == 0 || (l & SW_PAUSE) == 0 ||
		    (b & (SW_PAUSE | CLUSTER_EN)) != SW_PAUSE) {
			P("REFUSING stage 5 attach: expected paused stage-2 state, got LL=%08x L=%08x B=%08x",
			  ll, l, b);
			goto out;
		}
		P("stage 5 attach: PCM stays running; setting B CLUSTER_EN on powered MP2");
		cw(SW_RSV(2), b | CLUSTER_EN);
		writel(cr(SW_RSV(2)), csram + OFFS_SW_RSV0 + 2 * sizeof(u32));

		np = of_find_node_by_path("/i2c@1100e000");
		if (!np) {
			P("cannot find /i2c@1100e000 — not unpausing");
			goto out;
		}
		i2c_clk = of_clk_get_by_name(np, "main");
		of_node_put(np);
		if (IS_ERR(i2c_clk)) {
			P("cannot get the i2c main clock (%ld) — not unpausing",
			  PTR_ERR(i2c_clk));
			i2c_clk = NULL;
			goto out;
		}
		if (clk_prepare_enable(i2c_clk)) {
			P("cannot enable the i2c clock — not unpausing");
			goto out;
		}
		P("stage 5 attach: i2c clock enabled; about to clear PAUSE_SRC/SW_PAUSE");
		writel(0, csram + OFFS_PAUSE_SRC);
		cw(SW_RSV(2), cr(SW_RSV(2)) & ~SW_PAUSE);
		writel(cr(SW_RSV(2)), csram + OFFS_SW_RSV0 + 2 * sizeof(u32));
		P("stage 5 attach: unpause writes landed; sampling MP2");
		for (i = 0; i < 30; i++) {
			mdelay(10);
			P("  +%3d ms MP2=%08x B=%08x FSM=%08x",
			  (i + 1) * 10, readl(mp2_pwr), cr(SW_RSV(2)), cr(PCM_FSM_STA));
		}
		goto out;
	}

	/*
	 * The image must live where the IM can fetch it: physically contiguous,
	 * at an address we can name, and NOT sitting dirty in our caches -- the
	 * IM reads it over EMI, not through the CPU's caches. A module's own
	 * .rodata is in vmalloc space and fails the first two of those.
	 *
	 * dma_alloc_coherent() on a throwaway platform device gives all three:
	 * contiguous, a real bus address, and an uncached mapping so the memcpy
	 * is visible to EMI with no cache maintenance of our own. (The arm64
	 * flush helpers this originally used are not available to modules on
	 * 6.6, which is the better outcome -- this is the correct API anyway.)
	 */
	fw_bytes = CSPM_FW_SIZE * sizeof(u32);
	fw_pdev = platform_device_register_simple("gemini-cspm-probe", PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(fw_pdev)) {
		P("failed to create the staging device");
		fw_pdev = NULL;
		goto out;
	}
	if (dma_coerce_mask_and_coherent(&fw_pdev->dev, DMA_BIT_MASK(32))) {
		P("no 32-bit DMA mask");
		goto out_free;
	}
	fw_buf = dma_alloc_coherent(&fw_pdev->dev, fw_bytes, &fw_dma, GFP_KERNEL);
	if (!fw_buf) {
		P("failed to allocate %zu bytes for the image", fw_bytes);
		goto out_free;
	}
	memcpy(fw_buf, cspm_dvfs_binary, fw_bytes);

	cspm_register_init();
	if (!pcm_sw_reset())
		goto out_free;

	cw(PCM_CON0, CON0_CFG_KEY | CON0_PCM_CK_EN);
	cw(PCM_CON1, CON1_CFG_KEY | CON1_EVENT_LOCK_EN | CON1_SPM_SRAM_ISO_B |
		     CON1_SPM_SRAM_SLP_B | CON1_IM_NONRP_EN | CON1_MIF_APBEN);

	if (kick_im_to_fetch())
		goto out_free;

	bad = verify_im();
	if (bad) {
		P("IM VERIFY FAILED: %d of %d words differ — the co-processor would",
		  bad, CSPM_FW_SIZE);
		P("have executed something other than the image. NOT kicking.");
		goto out_free;
	}
	P("IM VERIFY OK: all %d words read back correctly", CSPM_FW_SIZE);

	if (stage < 2) {
		P("stage 1 only — PCM loaded but NOT started. Nothing is executing.");
		goto out_free;
	}

	kick_pcm_to_run();
	mdelay(20);
	report_pcm_alive("after kick");
	if (clear_b_assign_after_kick) {
		cw(SW_RSV(2), cr(SW_RSV(2)) & ~SW_F_ASSIGN);
		writel(cr(SW_RSV(2)), csram + OFFS_SW_RSV0 + 2 * sizeof(u32));
		P("cleared B SW_F_ASSIGN while paused: SW_RSV2=0x%08x", cr(SW_RSV(2)));
	}
	P("MP2_CPUSYS_PWR_CON = 0x%08x (after kick, before any cluster request)",
	  readl(mp2_pwr));

	if (stage < 3) {
		P("stage 2 only — PCM running, all clusters paused, no cluster requested.");
		P("the image stays allocated deliberately: the IM fetched it once, but");
		P("nothing here guarantees it will not be re-fetched while the PCM runs.");
		goto out;
	}

	before = readl(mp2_pwr);
	P("about to set CLUSTER_EN on cluster B (SW_RSV2). MP2 now 0x%08x", before);
	mdelay(50);

	cw(SW_RSV(2), cr(SW_RSV(2)) | CLUSTER_EN);
	writel(cr(SW_RSV(2)), csram + OFFS_SW_RSV0 + 2 * sizeof(u32));

	for (i = 0; i < 20; i++) {
		mdelay(10);
		v = readl(mp2_pwr);
		P("  +%2d ms  MP2_CPUSYS_PWR_CON = 0x%08x  SW_RSV2 = 0x%08x%s",
		  (i + 1) * 10, v, cr(SW_RSV(2)),
		  v != before ? "   <-- MOVED" : "");
	}

	P("after CLUSTER_EN: MP2_CPUSYS_PWR_CON = 0x%08x (was 0x%08x)", readl(mp2_pwr), before);

	if (stage < 4) {
		P("stage 3 only — cluster B requested but still paused.");
		goto out;
	}

	/*
	 * Unpause cluster B. The i2c clock is the same one i2c6 uses for its
	 * own transfers ("main" on /i2c@1100e000 is CLK_INFRA_I2C_APPM), so
	 * enabling it here just adds a reference; it does not take the bus
	 * away from anyone.
	 */
	{
		struct device_node *np = of_find_node_by_path("/i2c@1100e000");

		if (!np) {
			P("cannot find /i2c@1100e000 — not unpausing");
			goto out;
		}
		i2c_clk = of_clk_get_by_name(np, "main");
		of_node_put(np);
		if (IS_ERR(i2c_clk)) {
			P("cannot get the i2c main clock (%ld) — not unpausing",
			  PTR_ERR(i2c_clk));
			i2c_clk = NULL;
			goto out;
		}
		if (clk_prepare_enable(i2c_clk)) {
			P("cannot enable the i2c clock — not unpausing");
			goto out;
		}
		P("i2c clock enabled for CSPM");
	}

	before = readl(mp2_pwr);
	P("about to CLEAR SW_PAUSE on cluster B only. LL/L stay paused.");
	P("  LL=0x%08x L=0x%08x B=0x%08x", cr(SW_RSV(0)), cr(SW_RSV(1)), cr(SW_RSV(2)));
	mdelay(50);

	writel(0, csram + OFFS_PAUSE_SRC);
	cw(SW_RSV(2), cr(SW_RSV(2)) & ~SW_PAUSE);
	writel(cr(SW_RSV(2)), csram + OFFS_SW_RSV0 + 2 * sizeof(u32));

	for (i = 0; i < 30; i++) {
		mdelay(10);
		v = readl(mp2_pwr);
		P("  +%2d ms  MP2=0x%08x  B=0x%08x  LL=0x%08x  L=0x%08x  FSM=0x%08x%s",
		  (i + 1) * 10, v, cr(SW_RSV(2)), cr(SW_RSV(0)), cr(SW_RSV(1)),
		  cr(PCM_FSM_STA), v != before ? "   <-- MOVED" : "");
	}

	P("final MP2_CPUSYS_PWR_CON = 0x%08x (was 0x%08x)", readl(mp2_pwr), before);
	goto out;

out_free:
	if (fw_buf) {
		dma_free_coherent(&fw_pdev->dev, fw_bytes, fw_buf, fw_dma);
		fw_buf = NULL;
	}
	if (fw_pdev) {
		platform_device_unregister(fw_pdev);
		fw_pdev = NULL;
	}
out:
	P("==== end, survived ====");
	/*
	 * Never stay loaded. Returning an error unmaps nothing we still need:
	 * the CSPM state persists in hardware, which is the point.
	 */
	if (cspm)
		iounmap(cspm);
	if (csram)
		iounmap(csram);
	if (mp2_pwr)
		iounmap(mp2_pwr);
	return -EINVAL;
}

module_init(cspm_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Staged bring-up of the MT6797 CPUHVFS co-processor (B-40)");
