/* SPDX-License-Identifier: GPL-2.0
 * Host unit test: wmt_cfg.c must parse the REAL Gemini WMT_SOC.cfg
 * (tests/fixtures/WMT_SOC.cfg, byte-identical to the staged payload file,
 * SHA-256 f4a59b62..., see 04-docs/PAYLOAD-CATALOG.md) without modification
 * and yield the values the kernel parser would.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/wmt_cfg.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void test_real_fixture(const char *path)
{
	struct wmt_soc_cfg cfg;

	wmt_soc_cfg_init(&cfg);
	CHECK(wmt_soc_cfg_parse_file(path, &cfg) == 0);
	CHECK(cfg.coex_wmt_ant_mode == 1);
	CHECK(cfg.wmt_gps_lna_pin == 0);
	CHECK(cfg.wmt_gps_lna_enable == 0);
	CHECK(cfg.co_clock_flag == 0);
	CHECK(cfg.pairs_parsed == 4);
	CHECK(cfg.pairs_unknown == 0);
	CHECK(cfg.lines_malformed == 0);
	printf("real fixture: ant_mode=%d gps_lna_pin=%d gps_lna_enable=%d co_clock=%d\n",
	       cfg.coex_wmt_ant_mode, cfg.wmt_gps_lna_pin,
	       cfg.wmt_gps_lna_enable, cfg.co_clock_flag);
}

static void test_kernel_parser_semantics(void)
{
	struct wmt_soc_cfg cfg;
	/* CRLF endings, tabs/spaces around tokens, hex value, unknown key,
	 * malformed line — all tolerated the way wmt_conf.c tolerates them. */
	static const char buf[] =
		"co_clock_flag = 0x1A\r\n"
		"\tcoex_wmt_ant_mode\t=  2 trailing-junk\r\n"
		"coex_wmt_wifi_path=0x20\n"     /* known to kernel, not tracked here */
		"this line has no equals sign\n"
		"\r\n";

	wmt_soc_cfg_init(&cfg);
	CHECK(wmt_soc_cfg_parse_buf(buf, sizeof(buf) - 1, &cfg) == 0);
	CHECK(cfg.co_clock_flag == 0x1A);
	CHECK(cfg.coex_wmt_ant_mode == 2);
	CHECK(cfg.wmt_gps_lna_pin == WMT_CFG_UNSET);
	CHECK(cfg.pairs_parsed == 2);
	CHECK(cfg.pairs_unknown == 1);
	CHECK(cfg.lines_malformed == 1);
}

static void test_missing_file(void)
{
	struct wmt_soc_cfg cfg;

	wmt_soc_cfg_init(&cfg);
	CHECK(wmt_soc_cfg_parse_file("/nonexistent/WMT_SOC.cfg", &cfg) == -1);
}

int main(int argc, char *argv[])
{
	const char *fixture = (argc > 1) ? argv[1] : "tests/fixtures/WMT_SOC.cfg";

	test_real_fixture(fixture);
	test_kernel_parser_semantics();
	test_missing_file();

	if (failures) {
		fprintf(stderr, "test_wmt_cfg: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_wmt_cfg: all checks passed\n");
	return EXIT_SUCCESS;
}
