# WMT daemon rootfs overlay (Slice 7 → consumed by Slice 10)

This tree is copied verbatim onto the Experimental-slot Debian 13 rootfs by
the Slice 10 assembly. It stages:

- `usr/lib/systemd/system/wmt-loader.service` — oneshot, first
- `usr/lib/systemd/system/wmt-launcher.service` — `Requires=`/`After=` the
  loader, runs with `-p /lib/firmware`
- `usr/sbin/wmt_loader`, `usr/sbin/wmt_launcher` — NOT in git; the CI
  `daemons.yml` workflow builds them (aarch64 glibc) and `make install`
  places them here. Slice 10 takes them from the CI artifact.

## Why the units are inert by default

Both units are shipped **disabled** (no `multi-user.target.wants/` symlinks
in this overlay) and Slice 10 must not enable them. Nothing may power or
touch CONSYS as a boot side effect until the first *supervised* bring-up
session on the device: the whole point of the Slice 12 plan is to bring the
Vendor stack up step by step next to the Reference slot, capturing state
between steps (cf. "Pre-firmware capture" in CONTEXT.md). An auto-started
launcher would race that procedure and contaminate the first captures.

Bring-up order (manual, in the supervised session):

    modprobe mtk_btif && modprobe mtk_stp_wmt_soc   # creates the dev nodes
    systemctl start wmt-loader.service
    systemctl start wmt-launcher.service
    journalctl -u wmt-launcher -f

Only after Internal Wi-Fi works and the sequence is trusted do we
`systemctl enable` them (a later slice's decision, not this one's).

The vendor bring-up order being mirrored (Android init.rc lineage):
`wmt_loader` (oneshot) → `wmt_launcher -p <patchdir>` (long-running).
