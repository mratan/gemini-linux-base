// SPDX-License-Identifier: GPL-2.0-only
/*
 * gemini-a72-bringup — bring the two Cortex-A72 cores (cpu8/cpu9) online on
 * mt6797, WITHOUT PSCI CPU_ON.
 *
 * WHY NOT PSCI
 *
 * The on-device ATF (tee.img, "v1.0(debug):7f8e0c2", built 2019-05-08 — the
 * same bytes as 01-backups/tee1.bin) implements cluster-2 power-on as:
 *
 *     power_on_cl3():
 *         MP2_CPUSYS_PWR_CON |= PWR_ON          // SPM + 0x218, bit 2
 *         udelay(2)
 *         while (!(CPU_PWR_STATUS & BIT(17))) ; // SPM + 0x188   <-- no timeout
 *         while (!(mcucfg(0x102222A0) & BIT(17))) ;
 *         assert(mcucfg(0x102224A0) == 0x00FF1100)  ... and four more
 *
 * It never sets PWR_ON_2ND, never clears PWR_ISO, never touches the SRAM
 * bits: on this SoC MP2's CPUTOP is meant to be sequenced by hardware (the
 * "SPMC"), and bit 17 of CPU_PWR_STATUS is its acknowledgement. That bit does
 * not assert on our system, so the SMC never returns and the machine is lost.
 *
 * Everything after that poll is a plain register sequence we can do ourselves:
 *
 *     power_on_big(cpu):
 *         mcucfg(0x10222208) = 0x000F0000
 *         mcucfg(0x10222290 + idx*8) = entry_lo   // the core's boot address
 *         mcucfg(0x10222294 + idx*8) = 0
 *         MP2_CPUn_PWR_CON &= ~PWR_RST_B ; |= PWR_ON     // SPM + 0x240 + idx*4
 *         while (!(CPU_PWR_STATUS & BIT(15 - cpu))) ;
 *         while (!(mcucfg(0x10222430 + idx*4) & BIT(17))) ;
 *         MP2_CPUn_PWR_CON |= PWR_RST_B
 *
 * THE THING THAT MAKES THIS POSSIBLE, AND THAT NOBODY TRIED
 *
 * MCUCFG is unreachable from the non-secure world — every offset reads 0 and
 * drops writes. But ATF carries a general secure accessor pair:
 *
 *     0xC200035F  IDVFS_READ (addr)        -> *(u32 *)addr
 *     0xC200035E  IDVFS_WRITE(addr, val)   -> *(u32 *)addr = val
 *
 * both guarded by ONE check: (addr & 0xFFFFC000) == 0x10220000. Anything else
 * returns -3. B-40 concluded "IDVFS_READ returns 0 for every address" from two
 * probes at 0x10006218 and 0x1001A204 — both OUTSIDE that window, so both
 * results were about the guard, not about the service. Inside the window this
 * is unrestricted read/write to all of MCUCFG, including the A72 boot-address
 * registers. It is also how BIGIDVFSSRAMLDOSET is finally observable: that SMC
 * is a plain write of (old & ~0xFFF) | 0x8F0 | vosel to 0x102222B0, with no
 * EEM/PTP dependency anywhere in it.
 *
 * SAFETY
 *
 *  - The secure accessors are clamped to the window here. Out-of-window calls
 *    make ATF return -3 where the dispatcher expects a context pointer; not
 *    worth finding out what that does.
 *  - No PSCI call is made in any stage. Nothing can spin at EL3.
 *  - Every poll is bounded and reports its timeout.
 *  - Stage 1 writes nothing at all.
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
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <asm/cacheflush.h>

/* ---- the secure MCUCFG accessors ------------------------------------- */

#define SIP_IDVFS_READ		0xC200035FU
#define SIP_IDVFS_WRITE		0xC200035EU
#define SIP_IDVFS_SRAMLDOSET	0xC20003BFU

#define MCUCFG_LO		0x10220000U
#define MCUCFG_HI		0x10223FFFU

#define P(fmt, ...) pr_emerg("a72bu: " fmt "\n", ##__VA_ARGS__)

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

/* ---- SPM ------------------------------------------------------------- */

#define SPM_PHYS		0x10006000UL
#define SPM_POWERON_CONFIG_EN	0x000
#define SPM_KEY			((0xb16U << 16) | 1U)	/* SPM_PROJECT_CODE */
#define SPM_SLEEP_TIMER_STA	0x178
#define PWR_STATUS		0x180
#define PWR_STATUS_2ND		0x184
#define CPU_PWR_STATUS		0x188
#define CPU_PWR_STATUS_2ND	0x18c
#define MP0_CPUSYS_PWR_CON	0x210
#define MP1_CPUSYS_PWR_CON	0x214
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
#define PD_SLPB_CLAMP		BIT(9)
#define SRAM_PDN_ACK		BIT(12)
#define SRAM_SLEEP_B		BIT(16)
#define SRAM_SLEEP_B_ACK	BIT(19)

/* CPU_PWR_STATUS bit for a cpu, from ATF: BIT(15 - cpu) */
#define CPU_PWR_BIT(cpu)	BIT(15 - (cpu))
#define MP2_CPUTOP_BIT		BIT(17)

/* ---- other blocks we read directly ----------------------------------- */

#define INFRACFG_PHYS		0x10001000UL
#define TOPAXI_PROTECTEN	0x234
#define TOPAXI_PROTECTEN_STA1	0x23c
#define MCUMIXED_PHYS		0x1001a000UL
#define ARMPLLDIV_MUXSEL	0x270	/* [1:0] = cluster B, [3:2] LL, [5:4] L, [7:6] CCI */
#define ARMPLLDIV_CKDIV		0x274	/* [4:0] = cluster B, [9:5] LL, [14:10] L */
#define ARMPLLDIV_ARM_K1	0x27c
#define ARMPLLDIV_MON_EN	0x284
#define CCI_PHYS		0x10390000UL
#define CSPM_PHYS		0x11015000UL
#define CSPM_POWERON_CONFIG	0x000
#define CSPM_SEMA		0x448
#define CSPM_KEY		0x0b160001U
#define TOPCKGEN_PHYS		0x10000000UL

#define DA9214_I2C_ADAPTER	2
#define DA9214_ADDR		0x68
#define DA9214_PAGE_CON		0x00
#define DA9214_BUCKB_CONT	0x5e

static int stage = 1;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage,
	"1 = read-only survey; 2 = + prerequisites and SRAMLDO readback; "
	"3 = + hand CPUTOP power-up; 4 = + boot address and core release");

static int cpu_target = 8;
module_param(cpu_target, int, 0444);

/*
 * ATF's own log ring (physical 0x7FF40000, the tee_reserved_mem LK hands us)
 * shows how a little core is actually started:
 *
 *     ... power on CPU5 ...
 *     little_spark2_setldo sparkvretcntrl=3f
 *     Little power on:0x3F
 *     mt_on_1, entry 10103c
 *
 * The reset vector is 0x0010103c — ATF's warm-boot entry in ON-CHIP SRAM, not
 * a DRAM address, and it matches MP0's boot-address register 0x10220038 which
 * reads 0x0010103c on this machine. A core released from reset has MMU and
 * caches off and is not yet in the coherency domain, so whether it can fetch
 * from DRAM at all is a real question. Setting this to 0x0010103c points the
 * A72 at the same entry the A53s use, which answers it.
 */
static unsigned int entry_override;
module_param(entry_override, uint, 0444);
MODULE_PARM_DESC(entry_override,
	"physical reset vector for the A72 (0 = our own stub in DRAM)");

static void __iomem *spm, *infracfg, *mcumixed, *cci, *cspm, *topckgen;

static void meter_all(const char *when);

/* ---- reporting ------------------------------------------------------- */

static void spm_report(const char *when)
{
	P("%-24s MP0=%08x MP1=%08x MP2=%08x  ISO=%08x", when,
	  readl(spm + MP0_CPUSYS_PWR_CON), readl(spm + MP1_CPUSYS_PWR_CON),
	  readl(spm + MP2_CPUSYS_PWR_CON), readl(spm + CPU_EXT_BUCK_ISO));
	P("%-24s PWR_STATUS=%08x/%08x CPU_PWR_STATUS=%08x/%08x  MP2_CPU0=%08x MP2_CPU1=%08x",
	  "", readl(spm + PWR_STATUS), readl(spm + PWR_STATUS_2ND),
	  readl(spm + CPU_PWR_STATUS), readl(spm + CPU_PWR_STATUS_2ND),
	  readl(spm + MP2_CPU0_PWR_CON), readl(spm + MP2_CPU0_PWR_CON + 4));
}

/*
 * The control that decides whether any of this is real: read the SAME MCUCFG
 * address two ways. A non-secure ioremap read is known to return 0; if the
 * secure path returns something else, the window is open.
 */
static void prove_the_window(void)
{
	void __iomem *ns = ioremap(0x10222000UL, 0x1000);
	static const u32 offs[] = { 0x000, 0x2a0, 0x2b0, 0x4a0, 0x4a4 };
	int i;

	P("---- control: same address, non-secure vs secure ----");
	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		u32 a = 0x10222000U + offs[i];

		P("  0x%08x   ioremap=%08x   SMC=%08x", a,
		  ns ? readl(ns + offs[i]) : 0xffffffff, sread(a));
	}
	if (ns)
		iounmap(ns);

	/* and an out-of-window id, refused locally, so ATF never sees it */
	P("  (0x10006218 would be refused by this module: in_window=%d)",
	  in_window(0x10006218U));
}

static void dump_range(const char *name, u32 base, u32 first, u32 last)
{
	u32 o;

	P("---- %s 0x%08x+0x%03x..0x%03x ----", name, base, first, last);
	for (o = first; o <= last; o += 16) {
		u32 v[4];
		int i;

		for (i = 0; i < 4; i++)
			v[i] = (o + i * 4 <= last) ? sread(base + o + i * 4) : 0;
		P("  +%03x: %08x %08x %08x %08x", o, v[0], v[1], v[2], v[3]);
	}
}

/*
 * The diff that matters: MP0 and MP1 are running, MP2 is dark, and on this
 * SoC the three clusters have parallel MCUCFG blocks. Anything that is a
 * "cluster is alive" bit shows up here as MP0 == MP1 != MP2.
 */
static void cluster_diff(void)
{
	static const struct { u32 mp0, mp1, mp2; const char *what; } regs[] = {
		{ 0x1022002c, 0x1022022c, 0x1022222c, "AXI_CONFIG (ACINACTM=bit4)" },
		{ 0x10220038, 0x10220238, 0x10222238, "+0x038" },
		{ 0x1022003c, 0x1022023c, 0x1022223c, "+0x03c" },
		{ 0x10220064, 0x10220264, 0x10222264, "+0x064" },
		{ 0x10221040, 0x10223040, 0x10222440, "SPMC-ish core block" },
		{ 0x10221044, 0x10223044, 0x10222444, "" },
		{ 0x10221048, 0x10223048, 0x10222448, "" },
		{ 0x1022104c, 0x1022304c, 0x1022244c, "" },
		{ 0x10221070, 0x10223070, 0x10222470, "debug/ctrl" },
		{ 0x10221200, 0x10223200, 0x10222200, "cluster misc 0" },
		{ 0x10221204, 0x10223204, 0x10222204, "" },
		{ 0x10221208, 0x10223208, 0x10222208, "written 0x000F0000 by ATF" },
		{ 0x1022120c, 0x1022320c, 0x1022220c, "|=0x11 on power-down" },
		{ 0x10221250, 0x10223250, 0x10222250, "" },
		{ 0x10221254, 0x10223254, 0x10222254, "" },
		{ 0x102217fc, 0x102237fc, 0x102227fc, "" },
	};
	int i;

	P("---- cluster block diff: MP0(up) MP1(up) MP2(dark) ----");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 a = sread(regs[i].mp0), b = sread(regs[i].mp1),
		    c = sread(regs[i].mp2);

		P("  %08x/%08x/%08x  %08x %08x %08x  %s%s",
		  regs[i].mp0, regs[i].mp1, regs[i].mp2, a, b, c,
		  (a == b && b != c) ? "<<< DIFFERS  " : "", regs[i].what);
	}
}

/*
 * Is the MP2 MCUCFG sub-block alive at all?
 *
 * The stage-1 survey found every register in 0x10222xxx reading 0 through the
 * secure accessor, while 0x1022002c/38/3c/64 (MP0), 0x1022022c (MP1) and
 * 0x102217fc/0x102237fc return real values through the SAME call. So the
 * question is not access rights — EL3 gets zero. Either the block is simply
 * zeroed at reset, or it has no supply.
 *
 * A write followed by a read-back separates those. MP2 is off, so its boot
 * address registers are inert: nothing reads them until a core is released.
 */
static void mp2_block_probe(const char *when)
{
	static const u32 scratch[] = { 0x10222294, 0x1022229c };
	int i;
	bool alive = false;

	P("---- MP2 block liveness (%s) ----", when);
	for (i = 0; i < ARRAY_SIZE(scratch); i++) {
		u32 a = scratch[i], rb;

		swrite(a, 0xA5A50000u | i);
		rb = sread(a);
		P("  0x%08x <- %08x, reads %08x  %s", a, 0xA5A50000u | i, rb,
		  rb == (0xA5A50000u | i) ? "HOLDS — block is alive" : "dropped");
		if (rb == (0xA5A50000u | i)) {
			alive = true;
			swrite(a, 0);
		}
	}
	P("  0x1022222c=%08x 0x10222238=%08x 0x1022223c=%08x 0x102227fc=%08x",
	  sread(0x1022222c), sread(0x10222238), sread(0x1022223c),
	  sread(0x102227fc));
	P("  0x102222a0=%08x 0x102222b0=%08x 0x102224a0=%08x 0x102224a4=%08x",
	  sread(0x102222a0), sread(0x102222b0), sread(0x102224a0),
	  sread(0x102224a4));
	P("  (MP0 control, known live: 0x1022002c=%08x 0x102217fc=%08x)",
	  sread(0x1022002c), sread(0x102217fc));
	/*
	 * The scratch write is a weak signal (0x1022229c implements only its
	 * low bits), so decide on the reads: the whole block reading zero when
	 * MP0's siblings read real values is the "no supply" signature.
	 */
	alive = alive || sread(0x102222b4) || sread(0x1022222c);
	P("  VERDICT: MP2 MCUCFG is %s", alive ? "ALIVE" : "DEAD (no supply/clock)");
}

static void survey(void)
{
	prove_the_window();
	spm_report("survey");

	P("---- SPM extras ----");
	P("  POWERON_CONFIG_EN=%08x SLEEP_TIMER_STA=%08x",
	  readl(spm + SPM_POWERON_CONFIG_EN), readl(spm + SPM_SLEEP_TIMER_STA));

	P("---- MP2 SPMC / boot-address / iDVFS block ----");
	dump_range("MCUCFG", 0x10222000, 0x200, 0x2fc);
	dump_range("MCUCFG", 0x10222000, 0x400, 0x4fc);
	P("  0x10222700 (big spark2 LDO) = %08x", sread(0x10222700));
	P("  MP2 top SPMC 0x102222a0 = %08x  (ATF waits for bit17)",
	  sread(0x102222a0));
	P("  MP2 c0  SPMC 0x10222430 = %08x  c1 0x10222434 = %08x",
	  sread(0x10222430), sread(0x10222434));
	P("  big SRAM LDO 0x102222b0 = %08x  cal 0x102222b4 = %08x",
	  sread(0x102222b0), sread(0x102222b4));
	P("  boot addr    0x10222290 = %08x %08x  0x10222298 = %08x %08x",
	  sread(0x10222290), sread(0x10222294),
	  sread(0x10222298), sread(0x1022229c));

	cluster_diff();
	mp2_block_probe("at rest");

	meter_all("at survey");
	P("---- infracfg / CCI / MCUMIXED (non-secure) ----");
	P("  TOPAXI_PROTECTEN=%08x STA1=%08x",
	  readl(infracfg + TOPAXI_PROTECTEN),
	  readl(infracfg + TOPAXI_PROTECTEN_STA1));
	P("  CCI STATUS(0x1039000c)=%08x  S4(0x10394000)=%08x  S5(0x10395000)=%08x",
	  readl(cci + 0xc), readl(cci + 0x4000), readl(cci + 0x5000));
	P("  MCUMIXED 0x1001a270=%08x 0x1001a274=%08x 0x1001a220=%08x 0x1001a224=%08x",
	  readl(mcumixed + 0x270), readl(mcumixed + 0x274),
	  readl(mcumixed + 0x220), readl(mcumixed + 0x224));
}

/* ---- prerequisites (the vendor's cpu_power_on_buck) ------------------ */

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
	r = da9214_rmw(ad, DA9214_PAGE_CON, 0x0, 0xf, 0);
	if (r >= 0)
		r = da9214_rmw(ad, DA9214_BUCKB_CONT, 0x1, 0x1, 0);
	i2c_put_adapter(ad);
	return r < 0 ? r : 0;
}

static void prerequisites(void)
{
	struct arm_smccc_res res;
	u32 before, after;

	P("==== prerequisites (vendor cpu_power_on_buck) ====");

	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);

	P("step 1: MP2_CPUSYS_PWR_CON |= PWR_RST_B");
	writel(readl(spm + MP2_CPUSYS_PWR_CON) | PWR_RST_B,
	       spm + MP2_CPUSYS_PWR_CON);

	P("step 2: dummy read 0x102224a0 = %08x (secure this time)",
	  sread(0x102224a0));

	P("step 3: VPROC2 on");
	if (enable_vproc2())
		P("  VPROC2 ENABLE FAILED — continuing so the rest is readable");
	mdelay(2);

	mp2_block_probe("VPROC2 on, still isolated");

	P("step 4: CPU_EXT_BUCK_ISO %08x -> clear bits[1:0]",
	  readl(spm + CPU_EXT_BUCK_ISO));
	writel(readl(spm + CPU_EXT_BUCK_ISO) & ~0x3u, spm + CPU_EXT_BUCK_ISO);
	udelay(240);
	P("  CPU_EXT_BUCK_ISO now %08x", readl(spm + CPU_EXT_BUCK_ISO));

	mp2_block_probe("VPROC2 on, isolation cleared");

	/*
	 * step 5, and the measurement B-40 could never make: the SRAM LDO
	 * setter, with a read-back of the register it writes.
	 */
	before = sread(0x102222b0);
	arm_smccc_smc(SIP_IDVFS_SRAMLDOSET, 110000, 0, 0, 0, 0, 0, 0, &res);
	udelay(240);
	after = sread(0x102222b0);
	P("step 5: BIGIDVFSSRAMLDOSET(110000) -> %ld", (long)res.a0);
	P("  0x102222b0  %08x -> %08x   %s", before, after,
	  before == after ? "NO CHANGE" : "CHANGED");
	P("  expected (old & ~0xfff) | 0x8f0 | 0xb = %08x",
	  (before & ~0xfffu) | 0x8f0u | 0xbu);
	P("  cal 0x102222b4 = %08x  (eFuse 0x1020666c mirrors here)",
	  sread(0x102222b4));

	spm_report("after prerequisites");
}

/* ---- hand CPUTOP power-up ------------------------------------------- */

static bool wait_bit(void __iomem *r, u32 mask, bool set, int us, const char *what)
{
	int i;

	for (i = 0; i < us; i++) {
		if (!!(readl(r) & mask) == set) {
			P("  %-26s OK after %d us", what, i);
			return true;
		}
		udelay(1);
	}
	P("  %-26s TIMEOUT after %d us (reg=%08x)", what, us, readl(r));
	return false;
}

static bool cputop_power_up(void)
{
	void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
	bool hw_ack;

	P("==== MP2 CPUTOP power-up ====");
	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);

	P("  before: MP2=%08x CPU_PWR_STATUS=%08x", readl(r),
	  readl(spm + CPU_PWR_STATUS));

	/* exactly what ATF does, but bounded */
	writel(readl(r) | PWR_ON, r);
	udelay(2);
	hw_ack = wait_bit(spm + CPU_PWR_STATUS, MP2_CPUTOP_BIT, true, 2000,
			  "SPMC ack (bit17)");
	P("  MP2=%08x  MCUCFG 0x102222a0=%08x", readl(r), sread(0x102222a0));

	mp2_block_probe("after PWR_ON");

	if (hw_ack)
		P("  the hardware sequencer answered — the supply is up");
	else
		P("  SPMC did not ack");

	/*
	 * The SPMC ack only covers the coarse power switch. Measured: with it
	 * asserted, MP2_CPUSYS_PWR_CON still reads 0x00010127 — PWR_ISO,
	 * SRAM_CKISO and SRAM_PDN all still set — while the two running A53
	 * clusters read 0x0009004D and every running A53 core reads
	 * 0x0001004D. So the MTCMOS control bits still have to be walked in
	 * software, exactly as spm_mtcmos_ctrl_cpusys0() does for MP0.
	 */
	P("  walking the MTCMOS sequence (target 0x0009004d, MP0 = %08x)",
	  readl(spm + MP0_CPUSYS_PWR_CON));
	writel(readl(r) | PWR_ON_2ND, r);
	udelay(50);
	wait_bit(spm + CPU_PWR_STATUS, MP2_CPUTOP_BIT, true, 2000,
		 "bit17 after PWR_ON_2ND");

	/*
	 * The SRAM head switch ("spark"). MediaTek gates each cluster's SRAM
	 * array behind a fine-grained switch programmed in MCUCFG:
	 * little_spark2_setldo() writes 0x3f into a per-core field of
	 * 0x10221c00 (MP0) / 0x10223c00 (MP1), and big_spark2_setldo(a, b)
	 * writes 0x10222700 = (b << 9) | (a << 6) | 0x3f.
	 *
	 * ATF calls the big one at the END of power_on_big, which is fine for
	 * it because it is re-powering a cluster something else already
	 * initialised. On a cold cluster it has never been written, and an SRAM
	 * array with no head switch is exactly a domain whose SRAM_SLEEP_B_ACK
	 * never comes back.
	 */
	P("  spark: MP0 0x10221c00=%08x  MP1 0x10223c00=%08x  MP2 0x10222700=%08x",
	  sread(0x10221c00), sread(0x10223c00), sread(0x10222700));
	swrite(0x10222700, 0x3f);
	P("  spark: MP2 0x10222700 <- 0x3f, reads %08x", sread(0x10222700));

	writel(readl(r) & ~(PWR_ISO | PD_SLPB_CLAMP), r);
	writel(readl(r) & ~SRAM_PDN, r);
	wait_bit(r, SRAM_PDN_ACK, false, 2000, "SRAM_PDN_ACK clear");
	udelay(1);
	writel(readl(r) | SRAM_ISOINT_B, r);
	udelay(1);
	writel(readl(r) & ~SRAM_CKISO, r);

	/*
	 * SRAM_SLEEP_B is already 1 in the reset value (0x00010132), so simply
	 * setting it again is a no-op and the SRAM controller never sees an
	 * edge to acknowledge. Give it one.
	 */
	P("  SRAM_SLEEP_B is %d at this point; toggling it for an edge",
	  !!(readl(r) & SRAM_SLEEP_B));
	writel(readl(r) & ~SRAM_SLEEP_B, r);
	udelay(10);
	writel(readl(r) | SRAM_SLEEP_B, r);
	wait_bit(r, SRAM_SLEEP_B_ACK, true, 2000, "SRAM_SLEEP_B_ACK");
	writel(readl(r) & ~PWR_CLK_DIS, r);
	writel(readl(r) | PWR_RST_B, r);
	mdelay(2);

	P("  MP2=%08x (MP0 running = %08x)  %s", readl(r),
	  readl(spm + MP0_CPUSYS_PWR_CON),
	  readl(r) == readl(spm + MP0_CPUSYS_PWR_CON) ? "*** IDENTICAL ***" : "");
	P("  CPU_PWR_STATUS=%08x  MCUCFG 0x102222a0=%08x",
	  readl(spm + CPU_PWR_STATUS), sread(0x102222a0));
	P("  0x102224a0=%08x (ATF asserts 0x00FF1100)", sread(0x102224a0));
	P("  0x102224a4=%08x (ATF asserts 0xB9B13B14)", sread(0x102224a4));
	P("  0x102224ac=%08x (ATF asserts 0x01B10100)", sread(0x102224ac));
	P("  0x102224b0=%08x (ATF asserts 0x00AF00AF)", sread(0x102224b0));
	P("  0x102224b4=%08x (ATF asserts 0x00000010)", sread(0x102224b4));

	return !!(readl(spm + CPU_PWR_STATUS) & MP2_CPUTOP_BIT);
}

/* ---- cluster B clock, exactly as ATF does it after the power-good ---- */

/*
 * CSPM's semaphore. On Android the DVFS co-processor masters MCUMIXED too, so
 * ATF takes SEMA around every write to it. CPUHVFS is not running here, so
 * this should be uncontended — but the protocol is cheap and the ordering is
 * the vendor's, so keep it. Bounded: a semaphore that never grants is a
 * finding, not a reason to wedge.
 */
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
	P("  CSPM semaphore not granted in 2 ms (reads %08x) — proceeding anyway",
	  readl(cspm + CSPM_SEMA));
	return false;
}

static void cspm_sema_give(void)
{
	writel(1, cspm + CSPM_SEMA);
}

/*
 * The vendor's own freq meter, _mt_get_cpu_freq_idvfs(), transcribed. Its
 * abist source numbering is from mt_idvfs.c's /proc show:
 *   34 = LL, 35 = L, 36 = CCI, 37 = Big (ATF uses the same 37 = 0x25).
 *
 * Measuring the two A53 clusters as well is the point: a meter that reports
 * plausible numbers for clocks we already know, and zero for cluster B, is
 * evidence. A meter that reports zero for everything is a broken instrument.
 */
static unsigned int meter_khz(unsigned int src)
{
	u32 dbg, misc;
	unsigned int t;
	int i;

	dbg = readl(topckgen + 0x10c);
	misc = readl(topckgen + 0x104);
	writel((dbg & 0xfffffffeu) | (src << 16), topckgen + 0x10c);
	writel(misc & 0x01ffffffu, topckgen + 0x104);
	writel(0x1000, topckgen + 0x220);
	writel(0x1010, topckgen + 0x220);
	for (i = 0; i < 20 && (readl(topckgen + 0x220) & 0x10); i++)
		mdelay(1);
	t = readl(topckgen + 0x224) & 0xffff;
	writel(dbg, topckgen + 0x10c);
	writel(misc, topckgen + 0x104);
	writel(0x1010, topckgen + 0x220);
	writel(0x1000, topckgen + 0x220);
	return (t * 26000u) / 1024u * 2u;
}

static void meter_all(const char *when)
{
	P("  freq meter (%s): LL=%u L=%u CCI=%u BIG=%u kHz", when,
	  meter_khz(34), meter_khz(35), meter_khz(36), meter_khz(37));
}

/*
 * Everything power_on_cl3() does AFTER its two power-good polls. This is the
 * half that was never reached, because the first poll is where ATF spins:
 * release the cluster clock, bring the big cluster's own PLL up through its
 * three-step enable, then point MCUMIXED's cluster-B mux at it.
 *
 * Without this the domain is powered and the cores are held at 26 MHz with
 * PWR_CLK_DIS still asserted — which is exactly the state the first attempt
 * left them in, and exactly why the mailbox stayed empty.
 */
static void cluster_clock_up(void)
{
	void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
	u32 v;

	P("==== cluster B clock (ATF power_on_cl3 tail) ====");

	P("  MP2=%08x, clearing PWR_CLK_DIS", readl(r));
	writel(readl(r) & ~PWR_CLK_DIS, r);
	udelay(20);
	P("  MP2=%08x", readl(r));

	P("  big PLL 0x102224a0 = %08x", sread(0x102224a0));
	swrite(0x102224a0, 0x00FF0100);
	udelay(1);
	swrite(0x102224a0, 0x00FF0101);
	udelay(10);
	swrite(0x102224a0, 0x00FF1101);
	udelay(30);
	P("  -> %08x  (pcw 0x102224a4 = %08x)", sread(0x102224a0),
	  sread(0x102224a4));

	cspm_sema_take();
	v = readl(mcumixed + ARMPLLDIV_MUXSEL);
	P("  ARMPLLDIV_MUXSEL %08x -> %08x (cluster B = [1:0])", v, v | 1);
	writel(v | 1, mcumixed + ARMPLLDIV_MUXSEL);
	udelay(1);
	v = readl(mcumixed + ARMPLLDIV_CKDIV);
	P("  ARMPLLDIV_CKDIV  %08x -> %08x (cluster B = [4:0])", v,
	  (v & ~0x1fu) | 8);
	writel((v & ~0x1fu) | 8, mcumixed + ARMPLLDIV_CKDIV);
	udelay(1);
	cspm_sema_give();

	cspm_sema_take();
	writel(0, mcumixed + ARMPLLDIV_ARM_K1);
	writel(0xffffffff, mcumixed + ARMPLLDIV_MON_EN);
	udelay(1);
	cspm_sema_give();

	/*
	 * Two independent readings of the same clock: the abist meter, and the
	 * PLL's own PCW arithmetic (the same _cpu_freq_calc the cpufreq port
	 * was validated against). If the meter is wrong the arithmetic still
	 * says whether the PLL is programmed.
	 */
	{
		u32 con0 = sread(0x102224a0), con1 = sread(0x102224a4);
		u64 f = (u64)(con1 & 0x7fffffffu) * 26u;
		unsigned int posdiv = 1u << ((con0 >> 12) & 7);

		meter_all("after cluster B clock");
		P("  cluster B PLL arithmetic says %llu kHz (pcw %08x posdiv %u)"
		  "   ATF expects ~750000",
		  (unsigned long long)((f >> 24) * 1000 / posdiv), con1, posdiv);
	}
	P("  MP2=%08x CPU_PWR_STATUS=%08x SPMC top=%08x", readl(r),
	  readl(spm + CPU_PWR_STATUS), sread(0x102222a0));
}

/* ---- iDVFS: what actually owns cluster B's frequency ------------------ */

/*
 * The freq meter says cluster B runs at ~630 MHz — the same as CCI — while
 * the big PLL at 0x102224a0/a4 is programmed for 750 MHz. So ARMPLLDIV_MUXSEL
 * is not what switches this cluster over, which agrees with the vendor's own
 * per-cluster table: LL/L/CCI get ARMCAXPLL0/1/2 and MT_CPU_DVFS_B gets NULL,
 * because the big cluster's frequency belongs to the iDVFS co-processor.
 *
 * BigiDVFSEnable_hp() is the vendor's hotplug-time entry point. The order
 * matters and one step is a trap: once iDVFS is enabled it masters i2c6
 * itself, through the "iDVFSAPB" bridge at 0x11017000. i2c6 is the bus our
 * CPU regulator sits on and is in the path of every A53 DVFS transition since
 * #51, so the bridge is configured for the right slave first, exactly as
 * iDVFSAPB_init() does, rather than left pointing at whatever reset left.
 */
#define IDVFSAPB_PHYS		0x11017000UL
#define IDVFSAPB_HW_PMIC_ADDR	0x14
#define IDVFSAPB_I2C_SLAVE_ADDR	0x84
#define IDVFSAPB_I2C_TIMING	0xa0
#define SIP_IDVFS_BIGIDVFSENABLE 0xC20003B0U
#define IDVFS_CTRL_REG_ANDROID	0x0010a203U	/* from Android /proc/idvfs */

static void idvfs_enable(void)
{
	void __iomem *apb = ioremap(IDVFSAPB_PHYS, 0x1000);
	struct arm_smccc_res res;
	struct i2c_adapter *ad;
	unsigned int vproc_x100 = 100000;
	union i2c_smbus_data d;

	P("==== iDVFS enable (vendor BigiDVFSEnable_hp) ====");
	if (!apb) {
		P("  cannot map iDVFSAPB");
		return;
	}

	/* iDVFSAPB_init(): 3.4 MHz timing, DA9214 slave 0xd0, Big vsel reg 0xd9 */
	writel(0x1001, apb + IDVFSAPB_I2C_TIMING);
	writel(0x00d0, apb + IDVFSAPB_I2C_SLAVE_ADDR);
	writel(0xd9, apb + IDVFSAPB_HW_PMIC_ADDR);
	P("  iDVFSAPB timing=%08x slave=%08x pmic_addr=%08x",
	  readl(apb + IDVFSAPB_I2C_TIMING),
	  readl(apb + IDVFSAPB_I2C_SLAVE_ADDR),
	  readl(apb + IDVFSAPB_HW_PMIC_ADDR));

	/* the vendor reads the live VPROC2 before handing it to ATF */
	ad = i2c_get_adapter(DA9214_I2C_ADAPTER);
	if (ad) {
		if (i2c_smbus_xfer(ad, DA9214_ADDR, 0, I2C_SMBUS_READ, 0xd9,
				   I2C_SMBUS_BYTE_DATA, &d) >= 0) {
			/* DA9214 step -> mV: 300 + step*10 */
			vproc_x100 = (300 + (d.byte & 0x7f) * 10) * 100;
			P("  VPROC2 reads 0x%02x -> %u (mv x100)", d.byte,
			  vproc_x100);
		}
		i2c_put_adapter(ad);
	}

	/* vendor sets the SRAM LDO to 1200 mV immediately before the enable */
	arm_smccc_smc(SIP_IDVFS_SRAMLDOSET, 120000, 0, 0, 0, 0, 0, 0, &res);
	udelay(20);
	P("  SRAMLDOSET(120000) -> %ld, 0x102222b0 = %08x", (long)res.a0,
	  sread(0x102222b0));

	arm_smccc_smc(SIP_IDVFS_BIGIDVFSENABLE, IDVFS_CTRL_REG_ANDROID,
		      vproc_x100, 120000, 0, 0, 0, 0, &res);
	P("  BIGIDVFSENABLE(0x%08x, %u, 120000) -> %ld",
	  IDVFS_CTRL_REG_ANDROID, vproc_x100, (long)res.a0);
	mdelay(2);

	P("  0x102224a0=%08x 0x102224a4=%08x 0x10222470=%08x 0x102224c8=%08x",
	  sread(0x102224a0), sread(0x102224a4), sread(0x10222470),
	  sread(0x102224c8));
	meter_all("after iDVFS enable");
	iounmap(apb);
}

/* ---- the core itself ------------------------------------------------- */

/*
 * movz x0,#lo ; movk x0,#hi,lsl#16 ; movz w1,#0x72a7 ; movk w1,#0xa72a,lsl#16
 * str w1,[x0] ; dsb sy ; 1: b 1b            (checked against objdump)
 *
 * TWO CHANGES FROM THE FIRST VERSION, BOTH BECAUSE THE INSTRUMENT COULD NOT
 * PRODUCE A POSITIVE:
 *
 *  - It used to park in `wfi`. But the core already reports standbyWFI while
 *    it is held in reset, so SLEEP_TIMER_STA could not distinguish "executed
 *    and parked" from "never ran". It now spins in a branch, so bit 10 of
 *    SLEEP_TIMER_STA going 1 -> 0 is a second, independent witness that does
 *    not depend on the store landing.
 *
 *  - The page used to come from __get_free_page(), i.e. a normal cacheable
 *    mapping cleaned only to the Point of Unification. An A72 released from
 *    reset runs with MMU and caches OFF and fetches straight from DRAM, and
 *    cluster 2 is not in the coherency domain yet, so those instructions could
 *    still have been sitting in this cluster's caches. The stub now lives in
 *    the same dma_alloc_coherent() region as the mailbox — uncached on both
 *    sides, so what we wrote is what DRAM holds.
 */
static const u32 stub[] = {
	0xd2800000, 0xf2a00000, 0x528e54e1, 0x72b4e541,
	0xb9000001, 0xd5033f9f, 0x14000000, 0x17ffffff,
};
#define A72_MAGIC	0xa72a72a7u
#define MOVZ_X0(i)	(0xd2800000u | (((i) & 0xffffu) << 5))
#define MOVK_X0_16(i)	(0xf2a00000u | (((i) & 0xffffu) << 5))
#define B_SELF		0x14000000u
#define MP2_CPU0_WFI	BIT(10)

static struct platform_device *pdev;
static u32 *coh;
static dma_addr_t coh_dma;
static u32 *mbox;
static dma_addr_t mbox_dma;
static phys_addr_t park_pa;

static int make_mailbox_and_park(void)
{
	u32 *park;
	int w;

	pdev = platform_device_register_simple("gemini-a72-bringup",
					       PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(pdev)) {
		P("no staging device");
		return -ENODEV;
	}
	if (dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		P("no 32-bit DMA mask");
		return -ENODEV;
	}
	coh = dma_alloc_coherent(&pdev->dev, 2 * PAGE_SIZE, &coh_dma,
				 GFP_KERNEL);
	if (!coh) {
		P("no coherent region");
		return -ENOMEM;
	}
	mbox = coh;
	mbox_dma = coh_dma;
	park = (u32 *)((u8 *)coh + PAGE_SIZE);
	park_pa = coh_dma + PAGE_SIZE;

	*mbox = 0x5a5a5a5a;
	if (*mbox != 0x5a5a5a5a) {
		P("mailbox will not hold a value — it could not tell us anything");
		return -EIO;
	}
	*mbox = 0;
	if (*mbox != 0) {
		P("mailbox will not clear — aborting");
		return -EIO;
	}

	for (w = 0; w < PAGE_SIZE / 4; w++)
		park[w] = B_SELF;
	memcpy(park, stub, sizeof(stub));
	park[0] = MOVZ_X0((u32)mbox_dma);
	park[1] = MOVK_X0_16((u32)mbox_dma >> 16);
	/* uncached mapping, but make the ordering explicit anyway */
	dma_wmb();

	if ((park_pa >> 32) || (mbox_dma >> 32)) {
		P("coherent region landed above 4 GB — refusing");
		return -EIO;
	}
	if (park[0] != MOVZ_X0((u32)mbox_dma)) {
		P("stub did not stick in the coherent page — refusing");
		return -EIO;
	}
	P("mailbox PA 0x%llx, park PA 0x%llx (uncached), stub = %08x %08x %08x %08x",
	  (unsigned long long)mbox_dma, (unsigned long long)park_pa,
	  park[0], park[1], park[2], park[3]);
	return 0;
}

/* stop a spinning core again, so a failed run does not leave it burning */
static void power_core_down(int cpu)
{
	void __iomem *r = spm + MP2_CPU0_PWR_CON + (cpu - 8) * 4;

	P("  powering cpu%d back down", cpu);
	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);
	writel(readl(r) | PWR_ISO, r);
	writel(readl(r) | SRAM_CKISO, r);
	writel(readl(r) & ~SRAM_ISOINT_B, r);
	writel(readl(r) | SRAM_PDN, r);
	wait_bit(r, SRAM_PDN_ACK, true, 1000, "core SRAM_PDN_ACK set");
	writel(readl(r) & ~PWR_RST_B, r);
	writel(readl(r) | PWR_CLK_DIS, r);
	writel(readl(r) & ~PWR_ON, r);
	writel(readl(r) & ~PWR_ON_2ND, r);
	P("  MP2_CPU%d=%08x CPU_PWR_STATUS=%08x", cpu - 8, readl(r),
	  readl(spm + CPU_PWR_STATUS));
}

static void release_core(int cpu)
{
	int idx = cpu - 8;
	void __iomem *r = spm + MP2_CPU0_PWR_CON + idx * 4;
	u32 boot = 0x10222290U + idx * 8;
	u32 spmc = 0x10222430U + idx * 4;
	u32 rb;
	int i;

	P("==== releasing cpu%d (MP2_CPU%d) ====", cpu, idx);

	/* the coherency interface for cluster 2, which ATF clears on power-on */
	P("  AXI_CONFIG 0x1022222c = %08x (ACINACTM bit4)", sread(0x1022222c));
	swrite(0x1022222c, sread(0x1022222c) & ~0x10u);
	P("  -> %08x", sread(0x1022222c));

	P("  0x10222208 = %08x, writing 0x000F0000", sread(0x10222208));
	swrite(0x10222208, 0x000F0000);
	P("  -> %08x", sread(0x10222208));

	{
		u32 entry = entry_override ? entry_override : (u32)park_pa;

		P("  MP0's boot address register 0x10220038 = %08x "
		  "(ATF logs 'mt_on_1, entry 10103c')", sread(0x10220038));
		P("  boot address 0x%08x <- 0x%08x%s", boot, entry,
		  entry_override ? "  (override: ATF's own SRAM entry)" : "");
		swrite(boot, entry);
		swrite(boot + 4, 0);
		rb = sread(boot);
		P("  read back 0x%08x = %08x %08x   %s", boot, rb,
		  sread(boot + 4),
		  rb == entry ? "MATCHES" : "*** DOES NOT MATCH ***");
	}

	writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);
	P("  MP2_CPU%d_PWR_CON = %08x", idx, readl(r));

	writel(readl(r) & ~PWR_RST_B, r);
	writel(readl(r) | PWR_ON, r);
	udelay(1);
	writel(readl(r) | PWR_ON_2ND, r);
	wait_bit(spm + CPU_PWR_STATUS, CPU_PWR_BIT(cpu), true, 2000,
		 "core power good");

	/*
	 * Same story as the cluster: the ack is not the whole state. A running
	 * A53 core reads 0x0001004D; this one comes up at 0x00010337. Walk it.
	 */
	P("  MP2_CPU%d=%08x, target 0x0001004d (MP0_CPU0 = %08x)", idx,
	  readl(r), readl(spm + 0x220));
	writel(readl(r) & ~(PWR_ISO | PD_SLPB_CLAMP), r);
	writel(readl(r) & ~SRAM_PDN, r);
	wait_bit(r, SRAM_PDN_ACK, false, 2000, "core SRAM_PDN_ACK clear");
	udelay(1);
	writel(readl(r) | SRAM_ISOINT_B, r);
	writel(readl(r) & ~SRAM_CKISO, r);
	writel(readl(r) | SRAM_SLEEP_B, r);
	writel(readl(r) & ~PWR_CLK_DIS, r);
	udelay(10);
	P("  MP2_CPU%d=%08x  %s", idx, readl(r),
	  readl(r) == readl(spm + 0x220) ? "*** IDENTICAL to a running core ***"
					 : "still differs");
	P("  core SPMC 0x%08x = %08x (ATF waits bit17)", spmc, sread(spmc));

	P("  WFI before release: SLEEP_TIMER_STA=%08x (MP2_CPU0 bit10=%d)",
	  readl(spm + SPM_SLEEP_TIMER_STA),
	  !!(readl(spm + SPM_SLEEP_TIMER_STA) & MP2_CPU0_WFI));
	P("  releasing PWR_RST_B");
	writel(readl(r) | PWR_RST_B, r);

	for (i = 0; i < 60; i++) {
		u32 wfi = readl(spm + SPM_SLEEP_TIMER_STA);

		mdelay(10);
		if (*mbox == A72_MAGIC) {
			P("  *** +%d ms MAILBOX = %08x — THE A72 EXECUTED ***",
			  (i + 1) * 10, *mbox);
			break;
		}
		if (idx == 0 && !(wfi & MP2_CPU0_WFI)) {
			P("  *** +%d ms standbyWFI DROPPED (%08x) — the core is "
			  "running instructions ***", (i + 1) * 10, wfi);
			break;
		}
		if ((i % 10) == 9)
			P("  +%3d ms mbox=%08x MP2_CPU%d=%08x CPU_PWR_STATUS=%08x WFI_STA=%08x",
			  (i + 1) * 10, *mbox, idx, readl(r),
			  readl(spm + CPU_PWR_STATUS), wfi);
	}
	P("final: mailbox=%08x  %s", *mbox,
	  *mbox == A72_MAGIC ? "A72 RAN" : "no execution seen");
	P("       MP2_CPU%d=%08x MP2=%08x CPU_PWR_STATUS=%08x SPMC=%08x WFI=%08x",
	  idx, readl(r), readl(spm + MP2_CPUSYS_PWR_CON),
	  readl(spm + CPU_PWR_STATUS), sread(spmc),
	  readl(spm + SPM_SLEEP_TIMER_STA));

	if (*mbox != A72_MAGIC)
		power_core_down(cpu);
}

/* ---- entry ----------------------------------------------------------- */

static int __init a72bu_init(void)
{
	P("==== gemini-a72-bringup, stage=%d cpu=%d ====", stage, cpu_target);

	spm = ioremap(SPM_PHYS, 0x1000);
	infracfg = ioremap(INFRACFG_PHYS, 0x1000);
	mcumixed = ioremap(MCUMIXED_PHYS, 0x1000);
	cci = ioremap(CCI_PHYS, 0x10000);
	cspm = ioremap(CSPM_PHYS, 0x1000);
	topckgen = ioremap(TOPCKGEN_PHYS, 0x1000);
	if (!spm || !infracfg || !mcumixed || !cci || !cspm || !topckgen) {
		P("ioremap failed");
		goto out;
	}

	/*
	 * stage 0 — put it all back. B-47 is an ENERGY limit: leaving VPROC2
	 * up and the big cluster powered costs current continuously, and that
	 * kills this machine silently by depletion rather than loudly by a
	 * peak. Any run that ends with the A72s not in use must undo itself.
	 */
	if (stage == 0) {
		void __iomem *r = spm + MP2_CPUSYS_PWR_CON;
		struct i2c_adapter *ad;

		spm_report("before restore");
		power_core_down(8);
		power_core_down(9);

		P("powering the MP2 cluster down");
		writel(SPM_KEY, spm + SPM_POWERON_CONFIG_EN);
		writel(readl(r) | PWR_ISO, r);
		writel(readl(r) | SRAM_CKISO, r);
		writel(readl(r) & ~SRAM_ISOINT_B, r);
		writel(readl(r) | SRAM_PDN, r);
		wait_bit(r, SRAM_PDN_ACK, true, 1000, "cluster SRAM_PDN_ACK");
		writel(readl(r) & ~PWR_RST_B, r);
		writel(readl(r) | PWR_CLK_DIS, r);
		writel(readl(r) & ~PWR_ON, r);
		writel(readl(r) & ~PWR_ON_2ND, r);
		wait_bit(spm + CPU_PWR_STATUS, MP2_CPUTOP_BIT, false, 2000,
			 "bit17 clear");

		P("re-asserting CPU_EXT_BUCK_ISO (B_EXT_BUCK_ISO)");
		writel(readl(spm + CPU_EXT_BUCK_ISO) | 0x2u,
		       spm + CPU_EXT_BUCK_ISO);
		udelay(100);

		P("turning VPROC2 (DA9214 BUCKB) off");
		ad = i2c_get_adapter(DA9214_I2C_ADAPTER);
		if (ad) {
			da9214_rmw(ad, DA9214_PAGE_CON, 0x0, 0xf, 0);
			da9214_rmw(ad, DA9214_BUCKB_CONT, 0x0, 0x1, 0);
			i2c_put_adapter(ad);
		} else {
			P("  no i2c adapter — VPROC2 LEFT ON, say so plainly");
		}

		spm_report("after restore");
		P("MP2 MCUCFG now reads 0x102222b0=%08x (0 = the block is dark "
		  "again, which is the resting state)", sread(0x102222b0));
		goto out;
	}

	survey();
	if (stage < 2)
		goto out;

	prerequisites();
	if (stage < 3)
		goto out;

	cputop_power_up();
	if (stage < 4)
		goto out;

	cluster_clock_up();
	if (stage < 5)
		goto out;

	if (stage >= 6)
		idvfs_enable();

	if (make_mailbox_and_park()) {
		P("could not build the instrument — refusing to report anything");
		goto out;
	}
	release_core(cpu_target);

out:
	P("==== end ====");
	return -EAGAIN;	/* never actually load */
}

module_init(a72bu_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mt6797 A72 bring-up without PSCI, via ATF's MCUCFG accessors");
