/*
* Copyright (C) 2016 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/threads.h>
#include <linux/printk.h>
#include "gl_typedef.h"

/* The vendor CPU-boost hint went through the MTK PPM driver
 * (mach/mt_ppm_api.h), which has no mainline equivalent on 6.6. This is a
 * throughput optimization only (bump core count / frequency during heavy
 * Wi-Fi traffic), not a correctness requirement, so it is a no-op here.
 * API-CHURN-LOG.md N9 (novel, semantic: no CPU boost — perf only). */
INT_32 kalBoostCpu(UINT_32 core_num)
{
	pr_warn_once("consys: kalBoostCpu is a no-op (no mainline MTK PPM)\n");
	return 0;
}
