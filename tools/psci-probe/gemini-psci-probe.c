// SPDX-License-Identifier: GPL-2.0
/*
 * gemini-psci-probe — ask the firmware about the Cortex-A72 cluster without
 * trying to start it.
 *
 * WHY THIS EXISTS
 *
 * The only way anyone has ever asked "can cpu8 come up?" on this machine is
 * `echo 1 > /sys/devices/system/cpu/cpu8/online`, which hard-locks the writing
 * CPU, takes every other CPU down behind cpus_read_lock, and costs about four
 * minutes of watchdog reset and reboot per attempt. Its only output is "the
 * machine went away".
 *
 * THE ONE PROBE THAT MATTERS
 *
 * TF-A's psci_cpu_on() does its work in this order:
 *
 *      psci_validate_mpidr(target)        -> INVALID_PARAMS if unknown
 *      psci_validate_entry_point(...)     -> INVALID_ADDRESS if bad address
 *      psci_cpu_on_start(...)             -> the platform power sequence
 *
 * So CPU_ON with a deliberately invalid entry point exercises the firmware's
 * recognition of a core and returns *before* any power domain is touched. It
 * starts nothing. Comparing a core the firmware certainly knows (cpu1, which
 * it booted for us) against cpu8 is the discriminator:
 *
 *   cpu1 -> INVALID_ADDRESS, cpu8 -> INVALID_ADDRESS
 *        firmware knows cluster 2; the hang is later, in the power sequence
 *   cpu1 -> INVALID_ADDRESS, cpu8 -> INVALID_PARAMS
 *        firmware does not know cluster 2 at all
 *   cpu1 returns, cpu8 hangs
 *        firmware's cluster-2 path hangs BEFORE it validates anything
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 * AFFINITY_INFO. It looked like the ideal read-only probe and it is useless
 * on this firmware: measured 2026-08-22, `AFFINITY_INFO(0x000, 0)` hangs the
 * whole machine, twice, on a core that is demonstrably ON. This is PSCI 0.2
 * (PSCI_VERSION returns 2, and PSCI_FEATURES correctly reports NOT_SUPPORTED
 * because FEATURES is a 1.0 call), and MediaTek's implementation of the
 * optional parts of 0.2 evidently is not one. Do not reach for it again.
 *
 * Every SMC is announced BEFORE it is issued. One that never returns takes
 * the calling CPU with it, so the only way to learn which call hung is to
 * have already said so; netconsole emits one UDP packet per line
 * synchronously, so the announcement is on the wire before the SMC runs.
 *
 * Usage — one bit per probe, so a hang can be stepped over on the next run:
 *   insmod gemini-psci-probe.ko tests=0x01   # PSCI_VERSION x3, control
 *   insmod gemini-psci-probe.ko tests=0x02   # CPU_ON(cpu1, bogus), control
 *   insmod gemini-psci-probe.ko tests=0x04   # CPU_ON(cpu8, bogus)  <- the ask
 *   insmod gemini-psci-probe.ko tests=0x08   # CPU_ON(cpu9, bogus)
 *   insmod gemini-psci-probe.ko tests=0x10   # AFFINITY_INFO — HANGS, see above
 *
 * THE iDVFS SIP PROBES (0x100 and up), added 2026-08-22.
 *
 * The A72 cluster's power-up is not a register sequence the kernel writes. The
 * vendor's mt_idvfs.c reaches it through a SECURE MONITOR CALL to ATF —
 * BigiDVFSEnable_hp(), commented in the vendor source "for cpu hot plug call",
 * ends in SEC_BIGIDVFSENABLE() which is smc(0xC20003B0, ...). There is a whole
 * SIP family, 0xC20003B0..0xC20003C1, plus a register read/write pair.
 *
 * If OUR ATF implements that service, the A72 path may be a handful of SMCs
 * rather than a 2500-line CSPM port. If it returns SMC_UNK, it does not, and
 * that is worth knowing before writing any driver at all. Either way it is one
 * insmod against a 2500-line question.
 *
 * These four are READ-ONLY by construction — they read a register or query a
 * value and change nothing:
 *   insmod gemini-psci-probe.ko tests=0x100  # IDVFS_READ of the vendor's own addr
 *   insmod gemini-psci-probe.ko tests=0x200  # BIGIDVFSPLLGETFREQ
 *   insmod gemini-psci-probe.ko tests=0x400  # BIGIDVFSPLLGETPOSDIV + GETPCW
 *   insmod gemini-psci-probe.ko tests=0x800  # BIGIDVFSSRAMLDOGET
 *
 * A SIP this firmware does not implement should return SMC_UNK (0xFFFFFFFF).
 * It might instead hang, exactly as AFFINITY_INFO does — which is why each one
 * has its own bit and announces itself first.
 *
 * NOT included, deliberately: BIGIDVFSENABLE (0xC20003B0) and everything else
 * that writes. Establish that the service answers before asking it to power up
 * a cluster.
 *
 * ANSWERED 2026-08-22, and the answer is NO. All four getters return SMC_UNK,
 * identically to the deliberately-bogus control id, so this ATF does not carry
 * the iDVFS service at all. IDVFS_READ (0xC200035F) is the one id that does not
 * return SMC_UNK -- but it returns 0 for every address tried, including
 * 0x10006218 where devmem reads 0x00010132, so whatever answers there is not
 * reading registers either. There is no SMC shortcut to the A72 cluster; the
 * route is the CSPM port after all.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <uapi/linux/psci.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>

/* iDVFS secure service, from the vendor's mt_idvfs.h (arm64 ids) */
#define MTK_SIP_IDVFS_READ		0xC200035FU
#define MTK_SIP_IDVFS_PLLGETFREQ	0xC20003BAU
#define MTK_SIP_IDVFS_PLLGETPOSDIV	0xC20003BCU
#define MTK_SIP_IDVFS_PLLGETPCW		0xC20003BEU
#define MTK_SIP_IDVFS_SRAMLDOGET	0xC20003C0U

/*
 * The address the vendor itself reads through SEC_BIGIDVFS_READ, in its own
 * (disabled) posdiv sanity check. Using theirs rather than inventing one keeps
 * this inside whatever range ATF is willing to serve.
 */
#define IDVFS_VENDOR_READ_ADDR		0x102224a0UL

/*
 * A function id in the MediaTek SIP range that no vendor source defines. The
 * control for the whole iDVFS question: if an UNIMPLEMENTED SIP returns
 * SMC_UNK here, then a real answer from IDVFS_READ means something. If this
 * also returns 0, then "0" is just what this firmware says to everything and
 * the iDVFS probe proved nothing.
 */
#define MTK_SIP_BOGUS			0xC20003FEU

/*
 * SMC_UNK arrives in x0 as 0x00000000FFFFFFFF -- 32 bits, NOT sign-extended
 * into the 64-bit register. Comparing the result against -1 therefore never
 * matches, and every "is this implemented?" verdict prints inverted. That bug
 * was in the first version of these probes and briefly reversed the reading of
 * the control run; the raw hex in the log is what caught it.
 */
#define IS_SMC_UNK(v)			(((u32)(v)) == 0xFFFFFFFFU)

static unsigned int tests = 0x03;	/* the two safe controls by default */
module_param(tests, uint, 0444);
MODULE_PARM_DESC(tests, "bitmask of probes to run; see the file header");

static unsigned long readaddr = IDVFS_VENDOR_READ_ADDR;
module_param(readaddr, ulong, 0444);
MODULE_PARM_DESC(readaddr, "address for the tests=0x100 IDVFS_READ probe");

/*
 * Neither in DRAM nor aligned, so psci_validate_entry_point() must reject it.
 * If some firmware were to branch there anyway, that core faults immediately
 * on its own and cannot touch anything of ours.
 */
#define BOGUS_ENTRY	0xdead000000000001ULL

/*
 * A *valid* entry point, for the probe that matters.
 *
 * CPU_ON(cpu8, BOGUS_ENTRY) returns INVALID_PARAMS, which says the firmware
 * rejected the MPIDR. But Linux passes a real address and hard-locks instead,
 * so either the firmware validates the entry point first and only reaches the
 * MPIDR check on a good one, or the two paths differ some other way. This
 * gives the firmware an address it cannot object to and sees which happens.
 *
 * The page contains `wfi; b .-4` — if a core really does start there it parks
 * itself immediately, touches no memory, and cannot disturb anything. Cleaned
 * to the point of unification because that core starts with its MMU and caches
 * off and will fetch this straight from DRAM.
 */
#define AARCH64_WFI		0xd503207fu
#define AARCH64_B_BACK_ONE	0x17ffffffu

static const char *ret_str(long r)
{
	switch (r) {
	case PSCI_RET_SUCCESS:		return "SUCCESS";
	case PSCI_RET_NOT_SUPPORTED:	return "NOT_SUPPORTED";
	case PSCI_RET_INVALID_PARAMS:	return "INVALID_PARAMS  (firmware does not know this core)";
	case PSCI_RET_DENIED:		return "DENIED";
	case PSCI_RET_ALREADY_ON:	return "ALREADY_ON      (firmware knows it, and it is running)";
	case PSCI_RET_ON_PENDING:	return "ON_PENDING";
	case PSCI_RET_INTERNAL_FAILURE:	return "INTERNAL_FAILURE";
	case PSCI_RET_NOT_PRESENT:	return "NOT_PRESENT";
	case PSCI_RET_DISABLED:		return "DISABLED";
	case PSCI_RET_INVALID_ADDRESS:	return "INVALID_ADDRESS (firmware knows it; CPU_ON path runs)";
	default:			return "?";
	}
}

#define P(fmt, ...) pr_emerg("psci-probe: " fmt "\n", ##__VA_ARGS__)

static long smc(const char *desc, u32 fn, u64 a1, u64 a2, u64 a3)
{
	struct arm_smccc_res res;

	P("CALL  %s   fn=0x%08x a1=0x%llx a2=0x%llx",
	  desc, fn, (unsigned long long)a1, (unsigned long long)a2);
	/* Give netconsole's UDP packet time onto the wire before we may die. */
	mdelay(50);
	arm_smccc_smc(fn, a1, a2, a3, 0, 0, 0, 0, &res);
	P("RET   %s -> %ld (%s)", desc, (long)res.a0, ret_str((long)res.a0));
	return (long)res.a0;
}

static int __init probe_init(void)
{
	long v;
	int i;

	P("==== begin, tests=0x%x ====", tests);
	P("running on MPIDR_EL1 = 0x%llx",
	  (unsigned long long)(read_cpuid_mpidr() & MPIDR_HWID_BITMASK));

	if (tests & 0x01) {
		for (i = 0; i < 3; i++) {
			v = smc("PSCI_VERSION", PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);
			P("  PSCI_VERSION = %ld.%ld", (v >> 16) & 0xffff, v & 0xffff);
		}
	}
	if (tests & 0x02)
		smc("CPU_ON cpu1 mpidr 0x001 CONTROL",
		    PSCI_0_2_FN64_CPU_ON, 0x001, BOGUS_ENTRY, 0);
	if (tests & 0x04)
		smc("CPU_ON cpu8 mpidr 0x200 A72",
		    PSCI_0_2_FN64_CPU_ON, 0x200, BOGUS_ENTRY, 0);
	if (tests & 0x08)
		smc("CPU_ON cpu9 mpidr 0x201 A72",
		    PSCI_0_2_FN64_CPU_ON, 0x201, BOGUS_ENTRY, 0);
	if (tests & 0x10)
		smc("AFFINITY_INFO cpu0 (KNOWN TO HANG)",
		    PSCI_0_2_FN64_AFFINITY_INFO, 0x000, 0, 0);

	if (tests & 0x20) {
		u32 *park = (u32 *)__get_free_page(GFP_KERNEL);
		phys_addr_t pa;
		int w;

		if (!park) {
			P("could not allocate the park page");
			goto done;
		}
		for (w = 0; w < PAGE_SIZE / 4; w += 2) {
			park[w]     = AARCH64_WFI;
			park[w + 1] = AARCH64_B_BACK_ONE;
		}
		caches_clean_inval_pou((unsigned long)park,
				       (unsigned long)park + PAGE_SIZE);
		pa = virt_to_phys(park);
		P("park page at VA %px PA 0x%llx (wfi; b .-4)",
		  park, (unsigned long long)pa);
		smc("CPU_ON cpu8 mpidr 0x200 VALID ENTRY",
		    PSCI_0_2_FN64_CPU_ON, 0x200, pa, 0);
		/* Deliberately leaked: if a core did start there, freeing the
		 * page would let the allocator hand out memory a running CPU
		 * is executing. One page is a cheap price for that. */
	}

	/*
	 * iDVFS secure service. res.a0 here is a VALUE, not a PSCI status, so
	 * ret_str()'s decoding is meaningless for these -- read the raw number.
	 * SMC_UNK is 0xFFFFFFFF, which arrives as -1.
	 */
	if (tests & 0x100) {
		v = smc("IDVFS_READ  (does ATF have the iDVFS SIP at all?)",
			MTK_SIP_IDVFS_READ, readaddr, 0, 0);
		P("      IDVFS_READ(0x%lx) raw = 0x%lx  %s", readaddr,
		  (unsigned long)v,
		  IS_SMC_UNK(v) ? "<- SMC_UNK: this ATF does NOT implement it" :
				  "<- answered; compare against devmem of the same address");
	}

	if (tests & 0x200) {
		v = smc("BIGIDVFSPLLGETFREQ", MTK_SIP_IDVFS_PLLGETFREQ, 0, 0, 0);
		P("      big PLL freq raw = 0x%lx (%ld)", (unsigned long)v, v);
	}

	if (tests & 0x400) {
		v = smc("BIGIDVFSPLLGETPOSDIV", MTK_SIP_IDVFS_PLLGETPOSDIV, 0, 0, 0);
		P("      big PLL posdiv raw = 0x%lx (%ld)", (unsigned long)v, v);
		v = smc("BIGIDVFSPLLGETPCW", MTK_SIP_IDVFS_PLLGETPCW, 0, 0, 0);
		P("      big PLL pcw raw = 0x%lx (%ld)", (unsigned long)v, v);
	}

	if (tests & 0x800) {
		v = smc("BIGIDVFSSRAMLDOGET", MTK_SIP_IDVFS_SRAMLDOGET, 0, 0, 0);
		P("      big VSRAM raw = 0x%lx (%ld, vendor units are mV*100)",
		  (unsigned long)v, v);
	}

	/*
	 * THE CONTROL for everything above. Without it, a return of 0 from
	 * IDVFS_READ is unreadable: it could be the register's value, or it
	 * could be what this firmware returns to any call it does not know.
	 */
	if (tests & 0x1000) {
		v = smc("BOGUS SIP id (control: what does an unimplemented SIP return?)",
			MTK_SIP_BOGUS, 0, 0, 0);
		P("      bogus SIP raw = 0x%lx (%ld)  %s", (unsigned long)v, v,
		  IS_SMC_UNK(v) ? "<- SMC_UNK, so this firmware DOES reject unknown ids"
				: "<- NOT SMC_UNK: this firmware answers anything, and the iDVFS probes prove nothing");
	}

	if (tests & 0x40)
		smc("MIGRATE_INFO_TYPE (optional PSCI 0.2)",
		    PSCI_0_2_FN_MIGRATE_INFO_TYPE, 0, 0, 0);

	if (tests & 0x80)
		smc("CPU_ON cpu1 mpidr 0x001 VALID-ish ENTRY (control)",
		    PSCI_0_2_FN64_CPU_ON, 0x001, 0x40000000ULL, 0);

done:

	P("==== end, survived ====");
	return -EINVAL;	/* one-shot report; never stay loaded */
}

module_init(probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Non-destructive PSCI interrogation of the MT6797 A72 cluster");
