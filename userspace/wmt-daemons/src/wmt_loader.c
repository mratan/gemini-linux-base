/* SPDX-License-Identifier: GPL-2.0
 *
 * wmt_loader — MediaTek CONSYS chip-id handshake, native Debian port.
 *
 * Lineage: BPI-SINOVOIP/BPI-R2-bsp-4.14 linux-mt/utils/wmt/src/wmt_loader.c
 * (verbatim copy in upstream/src_wmt_loader.c; see PROVENANCE.md).
 * Adaptations for the Gemini PDA port are marked GEMINI-PORT and logged in
 * README.md. The ioctl sequence is unchanged:
 *   1. COMBO_IOCTL_GET_SOC_CHIP_ID  — read SoC chip id from the detect driver
 *   2. COMBO_IOCTL_SET_CHIP_ID      — publish it (the WMT stub stores it;
 *                                     wmt_launcher's WMT_QUERY_CHIPID reads it)
 *   3. COMBO_IOCTL_DO_MODULE_INIT   — no-op on this kernel tree
 *                                     (MTK_WCN_REMOVE_KO=0: connectivity is
 *                                     loaded as .ko via modprobe), kept to
 *                                     mirror the vendor bring-up order.
 */
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#include "wmt_detect_ioctl.h"
#include "wmt_dev_wait.h"

/* GEMINI-PORT (A1): upstream retried open() forever; default wait budget. */
#define WMT_LOADER_DEFAULT_WAIT_SEC 10

int main(void)
{
	int iRet = -1;
	int chipId = -1;
	int gLoaderFd = -1;
	int wait_sec = wmt_dev_wait_sec(WMT_LOADER_DEFAULT_WAIT_SEC);

	setvbuf(stdout, NULL, _IOLBF, 0); /* GEMINI-PORT: line-buffer for journald */

	printf("init combo device\n");
	/* GEMINI-PORT (A1): bounded wait instead of infinite retry loop */
	gLoaderFd = wmt_open_dev_bounded(WCN_COMBO_LOADER_DEV,
					 O_RDWR | O_NOCTTY, wait_sec);
	if (gLoaderFd < 0)
		return EXIT_FAILURE;

	printf("Opened combo device\n");

	/* Get Device ID */
	chipId = ioctl(gLoaderFd, COMBO_IOCTL_GET_SOC_CHIP_ID, NULL);
	printf("get device id : 0x%x\n", chipId);
	if (chipId == -1) {
		fprintf(stderr, "ERROR: invalid SoC chip id from %s: %s\n",
			WCN_COMBO_LOADER_DEV, strerror(errno));
		close(gLoaderFd);
		return EXIT_FAILURE;
	}

	/* Set Device ID */
	iRet = ioctl(gLoaderFd, COMBO_IOCTL_SET_CHIP_ID, chipId);
	printf("set device id : 0x%x\n", chipId);
	if (iRet < 0) {
		fprintf(stderr, "ERROR: failed to set device id: %s\n",
			strerror(errno));
		close(gLoaderFd);
		return EXIT_FAILURE;
	}

	/* do module init (no-op with MTK_WCN_REMOVE_KO=0, see header) */
	iRet = ioctl(gLoaderFd, COMBO_IOCTL_DO_MODULE_INIT, chipId);
	printf("do module init: 0x%x\n", chipId);
	if (iRet < 0) {
		fprintf(stderr, "ERROR: failed to init module: %s\n",
			strerror(errno));
		close(gLoaderFd);
		return EXIT_FAILURE;
	}

	close(gLoaderFd); /* GEMINI-PORT: upstream leaked the fd (exit closed it) */
	printf("wmt_loader done (chip id 0x%x)\n", chipId);
	return EXIT_SUCCESS;
}
