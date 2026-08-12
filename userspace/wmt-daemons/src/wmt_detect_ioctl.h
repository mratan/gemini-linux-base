/* SPDX-License-Identifier: GPL-2.0
 *
 * /dev/wmtdetect ioctl interface.
 *
 * Values lifted verbatim from the defines embedded in the BPI-R2 BSP
 * wmt_loader.c (upstream/src_wmt_loader.c) and cross-checked against the
 * kernel authority for this port:
 * modules/connectivity/common_detect/wmt_detect.h in the gemini-linux-base
 * tree (WMT_DETECT_IOC_MAGIC 'w', commands 0..8) — identical.
 */
#ifndef _WMT_DETECT_IOCTL_H_
#define _WMT_DETECT_IOCTL_H_

#include <sys/ioctl.h>

#define WCN_COMBO_LOADER_DEV          "/dev/wmtdetect"

#define WMT_DETECT_IOC_MAGIC            'w'
#define COMBO_IOCTL_GET_CHIP_ID       _IOR(WMT_DETECT_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_SET_CHIP_ID       _IOW(WMT_DETECT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_EXT_CHIP_DETECT   _IOR(WMT_DETECT_IOC_MAGIC, 2, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID   _IOR(WMT_DETECT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT    _IOR(WMT_DETECT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP    _IOR(WMT_DETECT_IOC_MAGIC, 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON   _IOR(WMT_DETECT_IOC_MAGIC, 6, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_OFF  _IOR(WMT_DETECT_IOC_MAGIC, 7, int)
#define COMBO_IOCTL_DO_SDIO_AUDOK     _IOR(WMT_DETECT_IOC_MAGIC, 8, int)

#endif /* _WMT_DETECT_IOCTL_H_ */
