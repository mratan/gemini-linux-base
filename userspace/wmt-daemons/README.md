# Native WMT daemons for Debian 13 (Slice 7, tracker issue #8)

The userspace half of the Vendor stack, rebuilt from source as native
aarch64 **glibc** binaries — no Android container, no vendor binaries:

- **`wmt_loader`** — oneshot. Reads the CONSYS SoC chip id from
  `/dev/wmtdetect` and publishes it to the WMT stub
  (`COMBO_IOCTL_GET_SOC_CHIP_ID` → `COMBO_IOCTL_SET_CHIP_ID` →
  `COMBO_IOCTL_DO_MODULE_INIT`, the last a no-op on our module-based
  kernel build).
- **`wmt_launcher`** (built from `src/stp_uart_launcher.c`, the upstream
  name for the same tool) — long-running. Opens `/dev/stpwmt`, queries the
  chip id the loader published, takes the MT6797 SoC/BTIF path
  (`STP_BTIF_FULL`), tells the kernel the STP mode, then services kernel
  upcalls: on `srh_patch` it scans the `-p` directory for ROM patches whose
  28-byte `WMT_PATCH` header matches the chip's firmware version and hands
  `{download seq, address, path}` per patch to the kernel
  (`WMT_IOCTL_SET_PATCH_NUM` / `WMT_IOCTL_SET_PATCH_INFO`). The kernel then
  fetches the files itself via `request_firmware()` — which is why the
  patches live in `/lib/firmware` and why `-p /lib/firmware` is the
  canonical invocation here (Android used `/vendor/firmware`).

Source lineage and license: see `PROVENANCE.md`. Pristine upstream copies:
`upstream/`. Kernel-side ioctl authority: `modules/connectivity/` in this
tree (`wmt_dev.c`, `wmt_detect.c`) — where daemon lineage and our kernel
disagree, the kernel tree wins (see A4 below, the only such case found).

## Build

Cross (the deliverable; CI does this on ubuntu-24.04):

    apt-get install gcc-aarch64-linux-gnu
    make all                      # -> build/wmt_loader, build/wmt_launcher
    make install DESTDIR=rootfs-overlay   # -> rootfs-overlay/usr/sbin/

Host-side checks (any arch, no cross tools needed):

    make test                     # unit tests for cfg + patch-header parsing
    make CROSS_COMPILE= all       # native-arch compile sanity
    ./tests/smoke-no-devnodes.sh build    # graceful-failure smoke test

CI: `.github/workflows/daemons.yml` (branch-scoped) runs all of the above,
executes the aarch64 binaries under `qemu-user`, and uploads the assembled
`rootfs-overlay/` as the `wmt-daemons-rootfs-overlay` artifact.

## Config handling (acceptance criterion 2)

The real 80-byte `WMT_SOC.cfg` from the staged payload is parsed **without
modification**:

- The kernel WMT core is the authoritative consumer — it loads
  `WMT_SOC.cfg` by bare name via `request_firmware()` (i.e. from
  `/lib/firmware`) and parses it in `wmt_conf.c`. Nothing about the file
  had to change for Debian; only its directory moved
  (`/vendor/firmware` → `/lib/firmware`), which is a staging decision
  recorded in 04-docs/PAYLOAD-CATALOG.md, not a config edit.
- The launcher additionally parses `<patchdir>/WMT_SOC.cfg` at startup with
  a userspace mirror of the kernel parser (`src/wmt_cfg.c`) and logs the
  values, so a broken or missing payload is visible in the journal before
  the first `srh_patch`. `tests/test_wmt_cfg.c` locks this against the
  byte-identical fixture `tests/fixtures/WMT_SOC.cfg`
  (coex_wmt_ant_mode=1, wmt_gps_lna_pin=0, wmt_gps_lna_enable=0,
  co_clock_flag=0).
- ROM patches are NOT committed; `tests/test_wmt_patch.c` synthesizes dummy
  patch files carrying the documented 28-byte header (research.md, "WMT
  Firmware-Push Protocol") with the real payload's header facts
  (u2SwVer 0x8a00; patch info `21 00 0a f0` / `22 00 09 00`) and verifies
  the scan reproduces the vendor behavior: 2 patches, seq 1 = `1_1`,
  seq 2 = `1_0`, `addRess[0]` zeroed.

## Adaptation log (every change vs upstream BPI-R2 BSP)

All marked `GEMINI-PORT (Ax)` in the sources. Upstream reference:
`upstream/src_stp_uart_launcher.c`, `upstream/src_wmt_loader.c`.

| # | Change | Why |
|---|---|---|
| A1 | Unbounded retry loops replaced with bounded waits (`src/wmt_dev_wait.c`; default 10 s, `WMT_DEV_WAIT_SEC` overrides): loader's `/dev/wmtdetect` open, launcher's `/dev/stpwmt` open and chip-id query. On timeout: clear error naming the node, exit 1. | Acceptance criterion 3: no device nodes → clean nonzero exit, no hang. Upstream spun forever. |
| A2 | Default patch dir `/etc/firmware` → `/lib/firmware`; combo-only `WMT.cfg` lookup `/system/etc/firmware` → `/lib/firmware`. | Debian firmware layout; matches where Slice 2 staged the payload and where the kernel's `request_firmware()` looks. |
| A3 | `cmd_hdr_sch_patch`'s directory-scan/header-parse body extracted into `src/wmt_patch.c` (launcher supplies the ioctl callback). Same semantics, including quirks: version gate `((hdr[22]<<8|hdr[23]) ^ fwVer) & 0xff`, patch count from the first match's `info[0]>>4`, `addRess[0]` zeroed, silent `return 0` when nothing matches (now with a loud warning, but the kernel-visible response is unchanged). | Makes the parse logic unit-testable on the host without devices; single implementation. |
| A4 | Removed `WMT_IOCTL_GET_APO_FLAG` (ioctl 28) probe and the `launcher_pwr_on_chip` thread (with `launcher_set_fwdbg_flag` and the pthread dependency). | **Kernel-wins rule:** our kernel tree does not implement ioctl 28; it returns `-EINVAL`, which upstream misreads as "always-power-on platform" and would power CONSYS from a daemon thread at startup. The supervised bring-up plan forbids powering CONSYS as a side effect; function drivers request power via `func_on`. |
| A5 | New startup step: parse `<patchdir>/WMT_SOC.cfg` with `src/wmt_cfg.c` (mirror of kernel `wmt_conf.c` line/value semantics) and log the result; warn if unreadable. Validation only — deliberately does NOT send `WMT_IOCTL_WMT_CFG_NAME` on the SoC path (upstream didn't either; an absolute path would break the kernel's `request_firmware` name resolution). | Acceptance criterion 2 visibility; catches a mis-staged payload at daemon start instead of mid-handshake. |
| A6 | Android-isms removed (upstream already had them compiled out or dead under `STATIC_BUILD=1`): property waits (`service.wcn.driver.ready` etc.), `query_chip_id`/`check_chip_id` property+`/dev/hifsdiod` dance (replaced by the bounded `WMT_IOCTL_WMT_QUERY_CHIPID` loop, A1), `set_coredump_flag` reduced to its STATIC_BUILD effect (`WMT_IOCTL_WMT_COREDUMP_CTRL, 0`), legacy positional-argument parser, `#if 0` blocks, unused baud-3.2M/3.25M handlers. The `0x0279 → 0x6797` (everest) alias is applied to the main-flow chip id too, not just inside the patch search. | Native Debian build; identical kernel-visible ioctl sequence for the MT6797 SoC path. |
| A7 | Bug fixes: `sStpParaConfig` was used uninitialized when `-p` was omitted on the SoC path (stack-garbage pointer) — now zeroed + defaulted to `/lib/firmware`; unchecked `strcpy` on `optarg` → bounded copies; loader's fd leak and shadowed global tidied; `stdout` line-buffered for journald. | Correctness; no behavioral change on the vendor-equivalent invocation `wmt_launcher -p <dir>`. |

Combo-chip (MT6620/28/30 UART) support is retained as upstream wrote it —
it is dead code on the Gemini (SoC flow) but keeping it minimizes divergence
from the lineage.

## Startup integration (acceptance criterion 4)

`rootfs-overlay/usr/lib/systemd/system/` stages `wmt-loader.service`
(oneshot) and `wmt-launcher.service` (`Requires=`/`After=` the loader,
`-p /lib/firmware`), mirroring the vendor bring-up order. Both are **inert
by default** — see `rootfs-overlay/README.md` for the reasoning (nothing
auto-starts until the first supervised bring-up session).

## How Slice 10 consumes this

Take the `wmt-daemons-rootfs-overlay` CI artifact (or run
`make all install`) and copy `rootfs-overlay/` over the Experimental-slot
rootfs, alongside the Slice 2 firmware overlay
(`05-gemini-payload/consys/rootfs-overlay/lib/firmware/`). Do not enable
the units. Everything remains NOT-YET-FLASHED per the remote-only rules.

## Status / limitations

- Built and smoke-tested in CI under qemu-user; **never yet run against the
  real kernel modules or hardware** (device-free slice). First real
  execution happens in the supervised bring-up (Slice 12 territory).
- BT/GPS/FM helper daemons (`stp_dump3`, `wmt_concurrency`, `wifi2agps`)
  are out of scope per the PRD (Wi-Fi gate; BT deferred).
