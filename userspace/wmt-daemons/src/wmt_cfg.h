/* SPDX-License-Identifier: GPL-2.0
 *
 * WMT_SOC.cfg parser for the Gemini WMT daemon port (Slice 7).
 *
 * New file (not from the BPI-R2 BSP). The kernel WMT core parses its own
 * copy of WMT_SOC.cfg (fetched by name via request_firmware(), i.e. from
 * /lib/firmware) in modules/connectivity/common_main/core/wmt_conf.c. This
 * userspace parser mirrors that parser's line/key/value semantics exactly
 * (split on CR/LF, require '=', trim spaces/tabs, values decimal or
 * 0x-prefixed hex) so the launcher can validate at startup that the staged
 * payload config parses, and log the values that the kernel will see.
 * The kernel remains the authoritative consumer.
 */
#ifndef _WMT_CFG_H_
#define _WMT_CFG_H_

#include <stddef.h>

#define WMT_CFG_UNSET (-1)

struct wmt_soc_cfg {
	/* The keys present in the Gemini's real 80-byte WMT_SOC.cfg
	 * (04-docs/PAYLOAD-CATALOG.md). WMT_CFG_UNSET when absent. */
	int coex_wmt_ant_mode;
	int wmt_gps_lna_pin;
	int wmt_gps_lna_enable;
	int co_clock_flag;

	int pairs_parsed;   /* key=value pairs recognised */
	int pairs_unknown;  /* well-formed pairs with keys we don't track
			     * (the kernel knows more keys; not an error) */
	int lines_malformed; /* non-empty lines without '=' (kernel warns too) */
};

void wmt_soc_cfg_init(struct wmt_soc_cfg *cfg);

/* Parse a config image of len bytes (need not be NUL-terminated).
 * Returns 0 on success (even if no known keys found), -1 on OOM. */
int wmt_soc_cfg_parse_buf(const char *buf, size_t len, struct wmt_soc_cfg *cfg);

/* Read and parse a config file. Returns 0 on success, -1 if the file
 * cannot be read, -2 on parse (OOM) failure. */
int wmt_soc_cfg_parse_file(const char *path, struct wmt_soc_cfg *cfg);

#endif /* _WMT_CFG_H_ */
