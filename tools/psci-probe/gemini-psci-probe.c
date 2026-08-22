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
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <uapi/linux/psci.h>
#include <linux/delay.h>

static unsigned int tests = 0x03;	/* the two safe controls by default */
module_param(tests, uint, 0444);
MODULE_PARM_DESC(tests, "bitmask of probes to run; see the file header");

/*
 * Neither in DRAM nor aligned, so psci_validate_entry_point() must reject it.
 * If some firmware were to branch there anyway, that core faults immediately
 * on its own and cannot touch anything of ours.
 */
#define BOGUS_ENTRY	0xdead000000000001ULL

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

	P("==== end, survived ====");
	return -EINVAL;	/* one-shot report; never stay loaded */
}

module_init(probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Non-destructive PSCI interrogation of the MT6797 A72 cluster");
