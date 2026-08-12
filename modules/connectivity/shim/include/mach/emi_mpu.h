/* Shim: MT6797 EMI MPU interface (no-op on the 6.6 Base tree).
 *
 * The vendor 3.18 tree protects the CONSYS EMI shared-memory window through
 * the MediaTek EMI MPU driver (drivers/misc/mediatek/emi_mpu). That driver
 * does not exist on the Base tree, so region protection is a no-op here:
 * functionally the window is simply left unprotected, which matches how the
 * Base tree already ran all its CONSYS experiments (bsg100 tested the
 * EMI-MPU theory explicitly and eliminated it, blockers.md B-21 #247).
 * Macro/enum values mirror the vendor mt6797 emi_mpu.h so the call site
 * compiles unchanged. See API-CHURN-LOG.md N1 (semantic, novel).
 */
#ifndef __SHIM_EMI_MPU_H__
#define __SHIM_EMI_MPU_H__

#define NO_PROTECTION	0
#define SEC_RW		1
#define SEC_RW_NSEC_R	2
#define SEC_RW_NSEC_W	3
#define SEC_R_NSEC_R	4
#define FORBIDDEN	5
#define SEC_R_NSEC_RW	6

#define SET_ACCESS_PERMISSON(d7, d6, d5, d4, d3, d2, d1, d0) \
	((((unsigned int)(d7)) << 21) | (((unsigned int)(d6)) << 18) | \
	 (((unsigned int)(d5)) << 15) | (((unsigned int)(d4)) << 12) | \
	 (((unsigned int)(d3)) << 9)  | (((unsigned int)(d2)) << 6)  | \
	 (((unsigned int)(d1)) << 3)  | ((unsigned int)(d0)))

static inline int emi_mpu_set_region_protection(unsigned long long start,
						unsigned long long end,
						int region,
						unsigned int access_permission)
{
	return 0;
}

#endif /* __SHIM_EMI_MPU_H__ */
