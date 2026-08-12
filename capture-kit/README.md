# Capture kit — Reference-slot instrumentation (issue #9, Slice 8)

**Everything here is Device-free work. All artifacts are staged and
NOT-YET-FLASHED; nothing touches hardware until the remote-only period
ends.** Method this kit implements: `04-docs/DIVERGENCE-DEBUG-PLAN.md`
(capture points C0a, C0b, C1). Terms per `CONTEXT.md`; layout rules per
ADR-0002 (boot2/Gemian = Reference slot, never flashed; instrumented
kernels go to boot3).

## What this is

The instrument for the decisive experiment: a reproducible,
period-toolchain build of the Reference slot's kernel lineage (gemian
`native` 3.18.41) with instrumentation implementing every capture point
of the divergence-debug plan, packed as a boot2-compatible boot image
(destined for **boot3**), plus the scripts that run the Pre-firmware
capture (C0a), the single-query experiment (C0b) and the
working-transition trace (C1) on the Gemian userspace over SSH.

## Contents

| Path | What |
|---|---|
| `toolchain/Containerfile` | Debian stretch (EOL archive) + GCC 6.3 aarch64 cross — the same compiler version the shipped Gemian kernel was built with (banner-proven, see BUILD-EVIDENCE.md) |
| `build.sh` | containerized build (rootless podman), re-runnable from scratch |
| `apply-patches.sh` | applies the patch series to a fresh gemian `native` clone |
| `patches/0001-…` | bsg100 harvest instrumentation ported (their 0002+0008+0003): HARVEST-WMT/BTIF hexdumps, debug levels, HARVEST-SNAP, UART unmute — C1 traces stay byte-format-diffable against the mirrored H18/H35 |
| `patches/0002-…` | full plan register table as kernel-side reads (incl. the pwrap/PMIC and EMI-CRC rows devmem can't do) at the bring-up sequence points, CPUPCR 32×1ms burst at MCU-reset-release, CAPTURE-INIT/STP-MODE/BTIF-FIFO-CLR/MCU-RST event markers |
| `patches/0003-…` | `/proc/driver/wmt_capture` control hook (modinit / snap / cpupcr) usable with the daemons fully held off |
| `pack-boot2-compat.sh` | packs the built kernel + gemian DTB + the shipped Gemian ramdisk into an LK-bootable v0 image; round-trip verifies; stages + checksums + NOT-YET-FLASHED marker |
| `device-scripts/` | C0a / C0b / C1 / post-assoc capture scripts + lib + normalize + scrub (run on the device during the physical session) |
| `CHECKLIST-first-session.md` | the physical-session runbook (slot keys, hold-off, capture order, retrieval, abort paths) |
| `COVERAGE.md` | plan row → implementation line coverage table |
| `BUILD-EVIDENCE.md` | toolchain identity, digests, banners, unmodified-build match to the shipped Gemian kernel |
| `staging/` (gitignored) | packed image + Gemian ramdisk + SHA256SUMS — blobs never enter git |

## Reproduce from scratch

```sh
git clone --branch native https://github.com/gemian/gemini-linux-kernel-3.18 gemian-3.18
capture-kit/apply-patches.sh gemian-3.18
capture-kit/build.sh --src gemian-3.18 --out gemian-3.18-out
capture-kit/pack-boot2-compat.sh --kernel-out gemian-3.18-out
```

Requires: rootless podman, network (first run pulls
`docker.io/debian/eol:stretch`), and the local
`02-firmware/flash-set/debian_boot.img` (supplies the Gemian ramdisk and
header reference at pack time — never committed).

## Design notes

- **Why the gemian tree builds clean with GCC 6.3 while bsg100 needed 7
  fix patches**: they used GCC 14; the period compiler needs none of it.
  Their real (non-toolchain) instrumentation lessons ARE carried:
  DEVAPC-safe gating of CONN-domain reads (their H16), bounded printk
  volume in the hot window (their H28 observer-effect), UART unmute
  (their H2).
- **Daemon hold-off (evidence)**: Gemian (Debian 9, systemd) starts the
  vendor daemons inside the Android LXC container:
  `multi-user.target.wants` → `lxc@android.service` +
  `droid-hal-init.service`; `/var/lib/lxc/android/config` runs Android
  `/init` from `/system/boot/android-ramdisk.img` (rootfs inspected
  read-only from the local flash-set `linux.img` via debugfs,
  2026-08-12). Kernel side: `connectivity/Makefile` defines
  `MTK_WCN_REMOVE_KERNEL_MODULE`, so every connectivity driver init is
  deferred to wmt_loader's `COMBO_IOCTL_DO_MODULE_INIT` ioctl
  (`wmt_detect.c`) — masking those two units holds the whole Vendor
  stack off and leaves CONSYS exactly as LK left it.
- **C0b needs no bespoke query injector**: `mtk_wcn_wmt_func_on(WIFI)`
  (vendor `wmt_dbg` 0x7) runs power-on then `mtk_wcn_soc_sw_init`,
  whose first BTIF act is the pre-patch `WMT_QUERY_STP`
  (`init_table_1_2`, `wmt_ic_soc.c:1011`); with wmt_launcher held off
  the patch search self-terminates after 2000 ms
  (`wmt_ctrl_ul_cmd`, `wmt_ctrl.c:423`). Single variable, vendor code
  end-to-end.
- **boot2-compatible, boot3-destined**: the packed image uses the
  shipped Gemian boot image's header facts and its exact ramdisk, so LK
  boots it into the Gemian rootfs from any slot; per ADR-0002 it is
  only ever flashed to boot3 (silver+Esc), keeping the Reference slot
  pristine while the captures still run on genuine Gemian userspace.

## CI

`.github/workflows/capture-kit.yml` (branch-scoped): shellcheck over the
kit's scripts + patch-apply check against a fresh shallow clone of the
gemian `native` tree. The full containerized 3.18 build is local-only by
design (period container + multi-GB tree are poor CI citizens; local
reproducibility documented in BUILD-EVIDENCE.md).
