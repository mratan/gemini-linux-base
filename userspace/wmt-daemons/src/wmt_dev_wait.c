/* SPDX-License-Identifier: GPL-2.0
 * Bounded device-node wait helper. See wmt_dev_wait.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "wmt_dev_wait.h"

#define WMT_DEV_WAIT_INTERVAL_MS 300

int wmt_dev_wait_sec(int def_sec)
{
	const char *s = getenv("WMT_DEV_WAIT_SEC");
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return def_sec;
	v = strtol(s, &end, 10);
	if (*end == '\0' && v >= 0 && v <= 3600)
		return (int)v;
	fprintf(stderr, "ignoring invalid WMT_DEV_WAIT_SEC=\"%s\", using %d s\n",
		s, def_sec);
	return def_sec;
}

int wmt_open_dev_bounded(const char *path, int flags, int timeout_sec)
{
	int waited_ms = 0;
	int reported = 0;
	int fd;

	for (;;) {
		fd = open(path, flags);
		if (fd >= 0)
			return fd;
		if (!reported) {
			fprintf(stderr, "waiting up to %d s for %s (%s)\n",
				timeout_sec, path, strerror(errno));
			reported = 1;
		}
		if (waited_ms >= timeout_sec * 1000)
			break;
		usleep(WMT_DEV_WAIT_INTERVAL_MS * 1000);
		waited_ms += WMT_DEV_WAIT_INTERVAL_MS;
	}
	fprintf(stderr,
		"ERROR: cannot open %s after %d s: %s (kernel connectivity modules not loaded?)\n",
		path, timeout_sec, strerror(errno));
	return -1;
}
