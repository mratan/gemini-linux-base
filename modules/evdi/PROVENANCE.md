# Vendored evdi kernel module — provenance

Slice U1 (tracker issue #26 on mratan/gemini-linux, PRD #25, ADR-0004),
vendored 2026-08-13.

| | |
|---|---|
| Source repo | https://github.com/DisplayLink/evdi |
| Tag | `v1.14.15` |
| Commit | `3dafd623f5c59ce6fe53f0662107d3e88f868de3` (2026-02-25) |
| Copied tree | `module/` → `modules/evdi/` (unmodified; `git archive v1.14.15 module`) |
| License | GPL-2.0 (see `LICENSE`; kernel module only — no proprietary code here) |

## Why this exact version

v1.14.15 is the evdi build that ships inside the official Synaptics
DisplayLink Ubuntu release **6.3.0-48** (2026-04-30), whose payload contains a
real aarch64 `DisplayLinkManager` binary (verified by direct inspection —
see `04-docs/USB-DISPLAY-RESEARCH-2026-08-13.md` in the gemini_linux repo,
issue #28). Pinning the same evdi version keeps the kernel side
binary-compatible with that userspace if/when the modern-DisplayLink
secondary route is exercised. Upstream v1.15.0 also compiles against our
6.6 base (checked 2026-08-13) — bump deliberately, not by drift.

## Build

Out-of-tree, same carrier as `modules/connectivity`:

```sh
make -C <kernel-tree> M=$PWD/modules/evdi ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux- modules
```

The module's own `Makefile` is kbuild-ready; `conftest.sh` probes the target
kernel's API at build time and emits `evdi_detect.h` (so no source edits are
needed across kernel versions). Supported kernel range per upstream README:
4.15 → 6.15 (our base is 6.6). `evdi.ko` needs only in-tree DRM symbols
(`CONFIG_DRM=y` — already forced by `configs/gemini-display.config`); it is
default-safe: a virtual-display platform driver that does nothing until the
DisplayLinkManager userspace opens it.

## Relation to udl (the primary route)

The **primary** USB-display route (`udl`, `configs/gemini-usbdisplay.config`,
DL-165/DL-195 adapters) does NOT use evdi. evdi exists for the **secondary**
route only (modern DL-3xxx-class adapters + proprietary DisplayLinkManager,
ADR-0004). The two routes need different adapter hardware.
