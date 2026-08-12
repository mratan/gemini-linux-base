/* SPDX-License-Identifier: GPL-2.0
 *
 * WMT ROM-patch directory scan for the Gemini WMT daemon port (Slice 7).
 *
 * This is the CUST_MULTI_PATCH "srh_patch" logic of the BPI-R2 BSP
 * stp_uart_launcher.c (cmd_hdr_sch_patch), extracted verbatim in behavior
 * into a standalone unit so it can be exercised by host unit tests
 * (tests/test_wmt_patch.c) without a kernel or device nodes. The launcher
 * supplies a callback that performs the WMT_IOCTL_SET_PATCH_NUM /
 * WMT_IOCTL_SET_PATCH_INFO ioctls; the tests supply a recording callback.
 *
 * Patch file format authority: 28-byte WMT_PATCH header
 * (04-docs/mirrors/bsg100/research.md, "WMT Firmware-Push Protocol"):
 *   [0..15]  ucDateTime      [16..19] ucPLat "ALPS"
 *   [20..21] u2HwVer         [22..23] u2SwVer
 *   [24..27] u4PatchVer ("patch info": [24]>>4 = total patch count,
 *            [24]&0xF = download sequence, bytes form addRess with [0]:=0)
 * Version gate (as upstream): ((hdr[22]<<8 | hdr[23]) ^ fwVersion) & 0xff == 0.
 */
#ifndef _WMT_PATCH_H_
#define _WMT_PATCH_H_

#include <stddef.h>

struct wmt_patch_entry {
	unsigned int download_seq;    /* hdr[24] & 0x0F */
	unsigned char address[4];     /* hdr[24..27] with [0] zeroed */
	char patch_name[256];         /* full path handed to the kernel */
};

/* Called once per matching patch file, in readdir order.
 * patch_num is hdr[24]>>4 of the FIRST matching file (upstream semantics);
 * is_first is 1 on the first match (when SET_PATCH_NUM must be sent).
 * Return 0 to continue scanning, nonzero to abort the scan. */
typedef int (*wmt_patch_cb)(const struct wmt_patch_entry *entry,
			    unsigned int patch_num, int is_first, void *ctx);

/* Map a WMT chip id to the patch filename prefix, e.g. 0x6797 or its icId
 * alias 0x0279 -> "ROMv3_patch". Includes the same chip-id aliases the
 * upstream launcher applies (denali/jade/everest). Returns 0, or -1 if the
 * chip is not in the upstream-supported list. */
int wmt_chip_patch_prefix(int chip_id, char *buf, size_t buflen);

#define WMT_PATCH_SCAN_EOPENDIR (-1) /* patch dir cannot be opened */
#define WMT_PATCH_SCAN_EUNSUP   (-2) /* unsupported chip id */

/* Scan dir for patches matching chip_id and fw_version.
 * Returns the number of matching patches (0 if none), or a negative
 * WMT_PATCH_SCAN_* error. */
int wmt_patch_scan(const char *dir, int chip_id, int fw_version,
		   wmt_patch_cb cb, void *ctx);

#endif /* _WMT_PATCH_H_ */
