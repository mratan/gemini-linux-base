// SPDX-License-Identifier: GPL-2.0
/*
 * SoC-only stand-ins for the external-combo-chip detect paths that
 * wmt_detect.c references unconditionally. The Gemini's CONSYS is
 * on-SoC (chip id 0x0279 -> reported as 0x6797); there is no external
 * MT66xx combo chip and no SDIO bus to probe, and the vendor
 * sdio_detect.c / wmt_detect_pwr.c pull in 3.18-only headers
 * (<mt_boot.h>, <mtk_rtc.h>) that do not exist on 6.6. The real
 * wmt_loader takes the SoC branch as soon as EXT_CHIP_PWR_ON fails
 * (MTK_WCN_COMBO_CHIP_SUPPORT is not defined), so these only have to
 * report "no external chip" coherently:
 *   - wmt_detect_read_ext_cmb_status() == 0  -> "not detected"
 *   - sdio_detect_query_chipid()      == -1  -> no combo chip id
 *   - hif_sdio_is_chipid_valid()      <  0   -> id invalid
 */

#include "wmt_detect.h"
#include "sdio_detect.h"
#include "wmt_detect_pwr.h"

int sdio_detect_init(void)
{
	return 0;
}

int sdio_detect_exit(void)
{
	return 0;
}

int sdio_detect_do_autok(int chipId)
{
	return 0;
}

int sdio_detect_query_chipid(int waitFlag)
{
	return -1;
}

int hif_sdio_is_chipid_valid(int chipId)
{
	return -1;
}

int wmt_detect_read_ext_cmb_status(void)
{
	return 0;
}
