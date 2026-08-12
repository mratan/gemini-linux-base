/* SPDX-License-Identifier: GPL-2.0
 * WMT ROM-patch directory scan. Extracted from BPI-R2 BSP
 * stp_uart_launcher.c cmd_hdr_sch_patch (CUST_MULTI_PATCH flow); see
 * wmt_patch.h and userspace/wmt-daemons/PROVENANCE.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "wmt_patch.h"

int wmt_chip_patch_prefix(int chip_id, char *buf, size_t buflen)
{
	/* Chip-id aliases, exactly as upstream cmd_hdr_sch_patch: */
	if (chip_id == 0x0321 || chip_id == 0x0335 || chip_id == 0x0337)
		chip_id = 0x6735;	/* denali */
	if (chip_id == 0x0326)
		chip_id = 0x6755;	/* jade */
	if (chip_id == 0x0279)
		chip_id = 0x6797;	/* everest (Gemini PDA MT6797) */

	switch (chip_id) {
	case 0x6572: case 0x6582: case 0x6592:
		snprintf(buf, buflen, "ROMv1_patch");
		return 0;
	case 0x8127: case 0x6571:
		snprintf(buf, buflen, "ROMv2_patch");
		return 0;
	case 0x6755: case 0x6752: case 0x6735:
	case 0x8163: case 0x6580: case 0x7623:
		snprintf(buf, buflen, "ROMv2_lm_patch");
		return 0;
	case 0x6797:
		snprintf(buf, buflen, "ROMv3_patch");
		return 0;
	case 0x6620: case 0x6628: case 0x6630:
		snprintf(buf, buflen, "mt%04x_patch", chip_id);
		return 0;
	default:
		return -1;
	}
}

int wmt_patch_scan(const char *dir, int chip_id, int fw_version,
		   wmt_patch_cb cb, void *ctx)
{
	char prefix[32];
	char full_name[256];
	unsigned char hdr_ver[2];
	unsigned char info[4];
	DIR *d;
	struct dirent *ent;
	int fd;
	int matched = 0;
	int is_first = 1;
	unsigned int patch_num = 0;
	unsigned int patch_ver;

	if (wmt_chip_patch_prefix(chip_id, prefix, sizeof(prefix)) != 0) {
		fprintf(stderr, "unsupported chip id 0x%04x for patch search\n",
			chip_id);
		return WMT_PATCH_SCAN_EUNSUP;
	}
	printf("patch name pre-fix:%s\n", prefix);

	d = opendir(dir);
	if (d == NULL) {
		fprintf(stderr, "patch path (%s) cannot be opened: %s\n",
			dir, strerror(errno));
		return WMT_PATCH_SCAN_EOPENDIR;
	}

	while ((ent = readdir(d)) != NULL) {
		struct wmt_patch_entry entry;

		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;
		if (snprintf(full_name, sizeof(full_name), "%s/%s",
			     dir, ent->d_name) >= (int)sizeof(full_name)) {
			fprintf(stderr, "patch path too long, skipping %s\n",
				ent->d_name);
			continue;
		}
		printf("%s\n", full_name);

		fd = open(full_name, O_RDONLY);
		if (fd < 0) {
			printf("open patch file(%s) failed\n", full_name);
			continue;
		}
		/* Upstream: lseek to 22, read u2SwVer big-endian-wise into
		 * patchVer (byte 22 = high, byte 23 = low), then the 4
		 * "patch info" bytes at 24..27. */
		if (lseek(fd, 22, SEEK_SET) == -1 ||
		    read(fd, hdr_ver, 2) != 2) {
			fprintf(stderr, "read patch version failed (%s)\n",
				full_name);
			close(fd);
			continue;
		}
		patch_ver = ((unsigned int)hdr_ver[0] << 8) | hdr_ver[1];
		printf("fw Ver in patch: 0x%04x\n", patch_ver);
		if (((patch_ver ^ (unsigned int)fw_version) & 0x00ff) != 0) {
			close(fd);
			continue;
		}
		if (read(fd, info, 4) != 4) {
			fprintf(stderr, "read patch info failed (%s)\n",
				full_name);
			close(fd);
			continue;
		}
		close(fd);
		printf("read patch info:0x%02x,0x%02x,0x%02x,0x%02x\n",
		       info[0], info[1], info[2], info[3]);

		if (is_first) {
			patch_num = (info[0] & 0xF0) >> 4;
			printf("gpatchnum = [%u]\n", patch_num);
		}
		memset(&entry, 0, sizeof(entry));
		entry.download_seq = info[0] & 0x0F;
		printf("gdwonseq = [%u]\n", entry.download_seq);
		memcpy(entry.address, info, sizeof(entry.address));
		entry.address[0] = 0x00;
		strncpy(entry.patch_name, full_name,
			sizeof(entry.patch_name) - 1);
		entry.patch_name[sizeof(entry.patch_name) - 1] = '\0';

		matched++;
		if (cb != NULL && cb(&entry, patch_num, is_first, ctx) != 0)
			break;
		is_first = 0;
	}
	closedir(d);
	return matched;
}
