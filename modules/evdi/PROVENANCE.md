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

The module's own `Makefile` is kbuild-ready. Kernel-API compatibility in
**this** version is handled by `LINUX_VERSION_CODE` `#if` ladders in the
sources (upper-bounded around 6.15) — the `conftest.sh`/`evdi_detect.h`
build-time probing mechanism only exists from v1.15.0 upstream, NOT here;
when bumping the vendored version, audit the `#if` ladders (or move to a
conftest-era release deliberately). Supported kernel range per upstream
README: 4.15 → 6.15 (our base is 6.6). `evdi.ko` needs only in-tree DRM
symbols (`CONFIG_DRM=y` — already forced by `configs/gemini-display.config`);
it is default-safe: a virtual-display platform driver that does nothing until
the DisplayLinkManager userspace opens it.

## Known issues in the vendored code (kept unmodified by policy)

Found by code review 2026-08-13; fix upstream or at bump time, not by
patching the vendored copy in place:

- **USB-unplug vs remove_all race** (`evdi_platform_drv.c`, the
  `evdi_platform_drv_usb()` notifier): the notifier reads `g_ctx.devices[i]`
  and destroys the device *before* taking `g_ctx.lock` (the mutex covers only
  the count decrement), racing the sysfs `remove_all` path — potential
  use-after-free / double-unregister if an adapter is yanked concurrently
  with `echo 1 > /sys/devices/evdi/remove_all`. Practical guidance: don't
  drive `remove_all` while unplugging; only relevant to the evdi secondary
  route.
- **Build-host probing in the Makefile**: a fatal (non-dash) `include
  /etc/os-release` plus a Raspbian sniff of the build host's `/proc/cpuinfo`
  feeding `$(RPIFLAG)` into `ccflags-y`. A host without `/etc/os-release`
  aborts the build; a Raspberry Pi build host silently bakes `-DRPI` into
  `evdi.ko` (behaviorally inert on 6.6 — every RPI guard is OR'd with
  kernel ≥ 5.11 — but it makes the binary differ from the CI artifact).
  Build on CI or the mercury host, not the relay Pi.
- **Dead machinery for our use**: `dkms.conf`/`dkms_install.sh` (root-run
  installer editing modprobe.d — do not use; the packaging pipeline stages
  the prebuilt .ko) and the `tests/` KUnit scaffolding (not compilable in an
  out-of-tree `M=` build). Kept only to preserve the unmodified upstream
  tree shape.

## Relation to udl (the primary route)

The **primary** USB-display route (`udl`, `configs/gemini-usbdisplay.config`,
DL-165/DL-195 adapters) does NOT use evdi. evdi exists for the **secondary**
route only (modern DL-3xxx-class adapters + proprietary DisplayLinkManager,
ADR-0004). The two routes need different adapter hardware.
