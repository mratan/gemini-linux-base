# Provenance — WMT daemon sources (Slice 7, issue #8)

## Upstream origin

| | |
|---|---|
| Repository | `https://github.com/BPI-SINOVOIP/BPI-R2-bsp-4.14` |
| Commit (pinned) | `694d9fd1918988b304f36e2d1687dc59499e868a` ("Update the base code to kernel 4.14.32", 2018-04-18 — last commit touching these paths) |
| Directory | `linux-mt/utils/wmt/` |
| Retrieved | 2026-08-12, per-file via `raw.githubusercontent.com` pinned to the commit above (repo too large to clone usefully) |

This is the BPI-R2 BSP copy of MediaTek's open combo_tool / 6620_launcher
userspace, the lineage named by the PRD ("wmt_loader / stp_uart_launcher").
The Gemini's own vendor binaries (`wmt_launcher`, `wmt_loader` in the
extracted Payload — see 04-docs/PAYLOAD-CATALOG.md) are the same tools built
by MediaTek for Android/bionic; strings in the vendor `wmt_launcher`
(usage text, "srh_patch", patch-info log formats) match this source family.

## Files taken (verbatim copies in `upstream/`, SHA-256 of retrieved bytes)

| Upstream path | Local pristine copy | SHA-256 |
|---|---|---|
| `linux-mt/utils/wmt/src/stp_uart_launcher.c` | `upstream/src_stp_uart_launcher.c` | `35cc80590fcfeb0e5b5d2ce8c2b4432b2d8d433cfabde8fd1db644971214e238` |
| `linux-mt/utils/wmt/src/wmt_loader.c` | `upstream/src_wmt_loader.c` | `18fe6a2d79d7ba3c5c780eaaa0839276900199786e77a41df9a97a13efe44d15` |
| `linux-mt/utils/wmt/src/wmt_ioctl.h` | `upstream/src_wmt_ioctl.h` | `6c953a68cd3a82f148a02070d7d3b0b71147479910cb94d4d5941e740f9abb0d` |
| `linux-mt/utils/wmt/src/wmt_loopback.c` | `upstream/src_wmt_loopback.c` | `11dffd9baefa24d4cf3f24ff4b86f11336ce1efe7dbab81a9ca2618583f9759b` |
| `linux-mt/utils/wmt/src/Makefile` | `upstream/src_Makefile` | `dd801ccea10c78b5e3c3599549207f85dd582c8e13ad9d9469bf7070f8e00ca2` |
| `linux-mt/utils/wmt/Makefile` | `upstream/Makefile` | `fdba596644df10044040e2477f16c18ac3b83cbc2c6f42c0a255b17a9a9036d3` |
| `linux-mt/utils/wmt/README` | `upstream/README` | `3b3133309a269742419d6329696ee46bc6cb42899d574e289bb055ac493beb12` |

Derived files in `src/` (adapted; see README.md "Adaptation log"):
`stp_uart_launcher.c` (from `src_stp_uart_launcher.c`), `wmt_loader.c`
(from `src_wmt_loader.c`), `wmt_ioctl.h` (byte-identical copy),
`wmt_detect_ioctl.h` (the `/dev/wmtdetect` defines embedded in upstream
`wmt_loader.c`, split into a header), `wmt_patch.c/h` (the
`cmd_hdr_sch_patch` directory-scan logic of `stp_uart_launcher.c`,
extracted for unit testing). `wmt_cfg.c/h` and `wmt_dev_wait.c/h` are new
files written for this port (no upstream counterpart);
`wmt_cfg.c` mirrors the parser in the kernel tree's
`modules/connectivity/common_main/core/wmt_conf.c` (GPL-2.0, MediaTek).

`wmt_loopback.c` is retained as a pristine reference only; it is not built
or shipped (loopback testing is a supervised-bring-up tool, out of Slice 7
scope).

## Cross-references (same lineage, checked 2026-08-12, not used as source)

- `frank-w/openwrt-bpi-r2` — `package/utils/wmt/` packages the same tools
  for OpenWrt (the frank-w BPI-R2 precedent named in ADR-0001).
- Numerous Android vendor trees carry the ancestor as
  `.../connectivity/combo_tool/src/stp_uart_launcher.c`.

## Kernel-side interface authority

The ioctl numbers and semantics were verified 2026-08-12 against the
vendored kernel modules in the gemini-linux-base tree (branch lineage:
UBports 3.18.60 connectivity stack):

- `/dev/stpwmt`: `modules/connectivity/common_main/linux/wmt_dev.c`
  (`WMT_IOC_MAGIC 0xa0`; upstream's `wmt_ioctl.h` matches for every command
  it defines; the kernel does NOT implement `WMT_IOCTL_GET_APO_FLAG` (28) —
  see adaptation A4).
- `/dev/wmtdetect`: `modules/connectivity/common_detect/wmt_detect.{c,h}`
  (`WMT_DETECT_IOC_MAGIC 'w'`, commands 0..8 — identical to upstream
  `wmt_loader.c`).
- `WMT_PATCH_INFO` struct layout: `common_main/core/include/wmt_lib.h`
  (`{u32; u8[4]; u8[256]}` — upstream's `STP_PATCH_INFO` matches on LP64).

Patch-header format authority: 04-docs/mirrors/bsg100/research.md,
"WMT Firmware-Push Protocol" (28-byte `WMT_PATCH` header; derived from the
vendor 3.18 `wmt_ic_soc.c`/`wmt_core.h` and confirmed against the real
Gemini ROM patch blobs).

## License

The upstream files carry no license headers of their own. They are
MediaTek connectivity userspace distributed in the BPI-R2 BSP alongside and
in lockstep with the GPL-2.0 kernel half of the same WMT protocol (the
kernel counterpart files carry explicit GPL-2.0 headers, e.g.
`wmt_detect.c`); the BSP publishes them as source without a separate
license grant. This port treats the whole set as **GPL-2.0** (the safest
consistent reading), adds `SPDX-License-Identifier: GPL-2.0` to the adapted
and new files, and keeps the pristine upstream copies byte-identical in
`upstream/`. No Android/bionic vendor binaries are included anywhere in
this tree.

## What is deliberately NOT here

- The vendor Android daemon binaries (reference copies live outside git in
  `05-gemini-payload/consys/reference/vendor-bin/`, used only to compare
  strings/behavior).
- Any firmware: WMT ROM patches, `WIFI_RAM_CODE_6797` (gitignored payload;
  tests synthesize dummy patch files from the documented header format).
  The only payload-derived file in git is the 80-byte **text config**
  fixture `tests/fixtures/WMT_SOC.cfg`
  (SHA-256 `f4a59b622a4e0c1470e475ce33f3edae43b27f1fbdeba54dc7cf07503d132880`,
  identical to the staged payload copy, per PAYLOAD-CATALOG.md).
