/* SPDX-License-Identifier: GPL-2.0
 *
 * Bounded device-node wait helper for the Gemini WMT daemon port (Slice 7).
 * New file (not from the BPI-R2 BSP): replaces the upstream daemons'
 * unbounded open()/usleep() retry loops so that a missing kernel module or
 * device node produces a clear error and a clean nonzero exit instead of a
 * hang. See userspace/wmt-daemons/README.md, adaptation A1.
 */
#ifndef _WMT_DEV_WAIT_H_
#define _WMT_DEV_WAIT_H_

/* Wait budget in seconds: WMT_DEV_WAIT_SEC env var if set and valid,
 * otherwise def_sec. 0 means "try exactly once". */
int wmt_dev_wait_sec(int def_sec);

/* open(path, flags), retrying every 300 ms for up to timeout_sec seconds.
 * Returns the fd, or -1 after printing a clear diagnostic to stderr. */
int wmt_open_dev_bounded(const char *path, int flags, int timeout_sec);

#endif /* _WMT_DEV_WAIT_H_ */
