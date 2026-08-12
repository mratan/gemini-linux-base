/* SPDX-License-Identifier: GPL-2.0
 * WMT_SOC.cfg parser. See wmt_cfg.h for why this exists and what it mirrors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "wmt_cfg.h"

void wmt_soc_cfg_init(struct wmt_soc_cfg *cfg)
{
	cfg->coex_wmt_ant_mode = WMT_CFG_UNSET;
	cfg->wmt_gps_lna_pin = WMT_CFG_UNSET;
	cfg->wmt_gps_lna_enable = WMT_CFG_UNSET;
	cfg->co_clock_flag = WMT_CFG_UNSET;
	cfg->pairs_parsed = 0;
	cfg->pairs_unknown = 0;
	cfg->lines_malformed = 0;
}

/* Mirrors wmt_conf_parse_char/short/int in the kernel's wmt_conf.c:
 * explicit "0x" prefix check selects base 16, otherwise base 10.
 * (Deliberately NOT strtol base 0, which would also accept octal.) */
static int wmt_cfg_parse_value(const char *pos)
{
	long res;

	if (strlen(pos) > 2 && pos[0] == '0' && pos[1] == 'x')
		res = strtol(pos + 2, NULL, 16);
	else
		res = strtol(pos, NULL, 10);
	return (int)res;
}

static void trim_token(char **p)
{
	char *s = *p;
	char *e;

	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	e = s;
	while (*e != ' ' && *e != '\t' && *e != '\0' && *e != '\n')
		e++;
	*e = '\0';
	*p = s;
}

static void wmt_cfg_store_pair(struct wmt_soc_cfg *cfg, const char *key,
			       const char *val)
{
	int v = wmt_cfg_parse_value(val);

	if (strcmp(key, "coex_wmt_ant_mode") == 0)
		cfg->coex_wmt_ant_mode = v;
	else if (strcmp(key, "wmt_gps_lna_pin") == 0)
		cfg->wmt_gps_lna_pin = v;
	else if (strcmp(key, "wmt_gps_lna_enable") == 0)
		cfg->wmt_gps_lna_enable = v;
	else if (strcmp(key, "co_clock_flag") == 0)
		cfg->co_clock_flag = v;
	else {
		cfg->pairs_unknown++;
		return;
	}
	cfg->pairs_parsed++;
}

int wmt_soc_cfg_parse_buf(const char *buf, size_t len, struct wmt_soc_cfg *cfg)
{
	char *copy, *pch, *line;

	copy = malloc(len + 1);
	if (copy == NULL)
		return -1;
	memcpy(copy, buf, len);
	copy[len] = '\0';

	/* Kernel: while ((pLine = osal_strsep(&pch, "\r\n")) != NULL) */
	pch = copy;
	while ((line = strsep(&pch, "\r\n")) != NULL) {
		char *key, *val;

		if (*line == '\0')
			continue;
		val = strchr(line, '=');
		if (val == NULL) {
			cfg->lines_malformed++;
			continue;
		}
		*val = '\0';
		val++;
		key = line;
		trim_token(&key);
		trim_token(&val);
		wmt_cfg_store_pair(cfg, key, val);
	}
	free(copy);
	return 0;
}

int wmt_soc_cfg_parse_file(const char *path, struct wmt_soc_cfg *cfg)
{
	FILE *f;
	char *buf;
	long sz;
	int ret;

	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(f);
		return -2;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	ret = wmt_soc_cfg_parse_buf(buf, (size_t)sz, cfg);
	free(buf);
	return ret ? -2 : 0;
}
