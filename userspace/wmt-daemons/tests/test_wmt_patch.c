/* SPDX-License-Identifier: GPL-2.0
 * Host unit test: wmt_patch.c directory scan against synthesized dummy
 * patch files carrying the 28-byte WMT_PATCH header documented in
 * 04-docs/mirrors/bsg100/research.md ("WMT Firmware-Push Protocol").
 * Header facts mirrored from the real Gemini payload (which is NOT
 * committed): u2SwVer bytes {0x8a,0x00} at offsets 22..23; patch info
 * ROMv3_patch_1_1_hdr.bin = {0x21,0x00,0x0a,0xf0} (2 patches, seq 1),
 * ROMv3_patch_1_0_hdr.bin = {0x22,0x00,0x09,0x00} (2 patches, seq 2).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/wmt_patch.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Write a dummy patch: 28-byte WMT_PATCH header + tiny body. */
static void make_dummy_patch(const char *dir, const char *name,
			     unsigned char sw_hi, unsigned char sw_lo,
			     const unsigned char info[4])
{
	char path[512];
	unsigned char hdr[28];
	FILE *f;

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "20180615091545a\n", 16);   /* ucDateTime */
	memcpy(hdr + 16, "ALPS", 4);            /* ucPLat */
	hdr[20] = 0x8a; hdr[21] = 0x00;         /* u2HwVer */
	hdr[22] = sw_hi; hdr[23] = sw_lo;       /* u2SwVer (launcher's gate) */
	memcpy(hdr + 24, info, 4);              /* u4PatchVer / "patch info" */

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite("dummybody", 1, 9, f);           /* body content is irrelevant */
	fclose(f);
}

struct recorded {
	struct wmt_patch_entry entries[8];
	unsigned int patch_num[8];
	int is_first[8];
	int count;
};

static int record_cb(const struct wmt_patch_entry *e, unsigned int patch_num,
		     int is_first, void *ctx)
{
	struct recorded *r = ctx;

	if (r->count < 8) {
		r->entries[r->count] = *e;
		r->patch_num[r->count] = patch_num;
		r->is_first[r->count] = is_first;
	}
	r->count++;
	return 0;
}

static const struct wmt_patch_entry *find_by_seq(const struct recorded *r,
						 unsigned int seq)
{
	int i;

	for (i = 0; i < r->count && i < 8; i++)
		if (r->entries[i].download_seq == seq)
			return &r->entries[i];
	return NULL;
}

static void test_prefix_mapping(void)
{
	char buf[32];

	CHECK(wmt_chip_patch_prefix(0x6797, buf, sizeof(buf)) == 0 &&
	      strcmp(buf, "ROMv3_patch") == 0);
	CHECK(wmt_chip_patch_prefix(0x0279, buf, sizeof(buf)) == 0 &&
	      strcmp(buf, "ROMv3_patch") == 0); /* everest icId alias */
	CHECK(wmt_chip_patch_prefix(0x6580, buf, sizeof(buf)) == 0 &&
	      strcmp(buf, "ROMv2_lm_patch") == 0);
	CHECK(wmt_chip_patch_prefix(0x6572, buf, sizeof(buf)) == 0 &&
	      strcmp(buf, "ROMv1_patch") == 0);
	CHECK(wmt_chip_patch_prefix(0x6628, buf, sizeof(buf)) == 0 &&
	      strcmp(buf, "mt6628_patch") == 0);
	CHECK(wmt_chip_patch_prefix(0x9999, buf, sizeof(buf)) == -1);
}

static void test_scan(const char *dir)
{
	static const unsigned char info_1_1[4] = {0x21, 0x00, 0x0a, 0xf0};
	static const unsigned char info_1_0[4] = {0x22, 0x00, 0x09, 0x00};
	static const unsigned char info_bad[4] = {0x21, 0x00, 0x00, 0x00};
	struct recorded rec;
	const struct wmt_patch_entry *e;
	int n;

	/* the two real-payload-shaped patches (fw version low byte 0x00) */
	make_dummy_patch(dir, "ROMv3_patch_1_1_hdr.bin", 0x8a, 0x00, info_1_1);
	make_dummy_patch(dir, "ROMv3_patch_1_0_hdr.bin", 0x8a, 0x00, info_1_0);
	/* wrong fw version low byte -> must be skipped */
	make_dummy_patch(dir, "ROMv3_patch_wrongver.bin", 0x8a, 0x77, info_bad);
	/* wrong prefix -> must be ignored */
	make_dummy_patch(dir, "ROMv2_lm_patch_1_1_hdr.bin", 0x8a, 0x00, info_1_1);

	/* chip id as GET_CHIP_INFO(0) reports it on the Gemini (icId 0x0279),
	 * fw version with low byte 0x00 like the real chip reports */
	memset(&rec, 0, sizeof(rec));
	n = wmt_patch_scan(dir, 0x0279, 0x8a00, record_cb, &rec);
	CHECK(n == 2);
	CHECK(rec.count == 2);
	/* patch_num comes from info[0] high nibble of the first match */
	CHECK(rec.patch_num[0] == 2);
	CHECK(rec.is_first[0] == 1);
	if (rec.count > 1)
		CHECK(rec.is_first[1] == 0);

	e = find_by_seq(&rec, 1); /* ROMv3_patch_1_1_hdr.bin */
	CHECK(e != NULL);
	if (e != NULL) {
		CHECK(e->address[0] == 0x00 && e->address[1] == 0x00 &&
		      e->address[2] == 0x0a && e->address[3] == 0xf0);
		CHECK(strstr(e->patch_name, "ROMv3_patch_1_1_hdr.bin") != NULL);
		CHECK(e->patch_name[0] == '/');
	}
	e = find_by_seq(&rec, 2); /* ROMv3_patch_1_0_hdr.bin */
	CHECK(e != NULL);
	if (e != NULL) {
		CHECK(e->address[0] == 0x00 && e->address[1] == 0x00 &&
		      e->address[2] == 0x09 && e->address[3] == 0x00);
		CHECK(strstr(e->patch_name, "ROMv3_patch_1_0_hdr.bin") != NULL);
	}
}

static void test_error_paths(const char *dir)
{
	struct recorded rec;

	memset(&rec, 0, sizeof(rec));
	CHECK(wmt_patch_scan("/nonexistent-patch-dir", 0x6797, 0x8a00,
			     record_cb, &rec) == WMT_PATCH_SCAN_EOPENDIR);
	CHECK(wmt_patch_scan(dir, 0x9999, 0x8a00, record_cb, &rec)
	      == WMT_PATCH_SCAN_EUNSUP);
	/* no matching version at all -> 0 matches, no callback */
	memset(&rec, 0, sizeof(rec));
	CHECK(wmt_patch_scan(dir, 0x6797, 0x8a55, record_cb, &rec) == 0);
	CHECK(rec.count == 0);
}

int main(void)
{
	char dir[] = "/tmp/wmt_patch_test.XXXXXX";

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return EXIT_FAILURE;
	}

	test_prefix_mapping();
	test_scan(dir);
	test_error_paths(dir);

	if (failures) {
		fprintf(stderr, "test_wmt_patch: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_wmt_patch: all checks passed\n");
	return EXIT_SUCCESS;
}
