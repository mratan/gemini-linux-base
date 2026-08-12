# release/ — Experimental-slot integration capstone (Slice 10, issue #11)

This directory assembles what Slices 2–9 produced into ONE flash-ready
Gemini PDA Experimental-slot deliverable, and documents the first physical
session. **Nothing here flashes anything** — every output is a staged,
checksummed, NOT-YET-FLASHED artifact, gated on the remote-only period ending
(ADR-0002, CLAUDE.md hard constraints).

## Contents

| File | What it is |
|---|---|
| `assemble.sh` | One-command assembly. Builds `boot3-experimental.img` (kernel + DTB + initramfs, via `scripts/bootimg.py`) and `experimental.img` (Debian 13 loop-image rootfs, via `scripts/mkrootfs-loop.sh`) with the ported `.ko` under `/lib/modules/<kver>/updates`, the CONSYS firmware in `/lib/firmware`, and the INERT WMT daemons installed. Records `MANIFEST.txt`. |
| `RUNBOOK.md` | First-session runbook: back up boot3 → capture the Reference slot → flash the Experimental slot → first bring-up, each step with its risk + abort/recovery path. Gated at the top on the remote-only period being over. |
| `MANIFEST.txt` | Recorded manifest of every component's provenance + SHA-256 (hashes only, no device-unique data). Committed reference copy; `release.yml` regenerates the authoritative per-run copy. |
| `rootfs-overlay/` | Boot-time self-check (`gemini-slice10-verify.sh` + inert systemd unit) that emits the serial markers the QEMU proof asserts. Copied into the assembled rootfs. |
| `out/` | **gitignored** staging/output — the actual images, the staged (real) firmware, the built `.ko`. Blobs never enter git. |

## Assemble (device-free)

```sh
# real firmware (private blobs), boot3 image + manifest, no rootfs (no root):
release/assemble.sh --kbuild /path/to/linux-6.6 \
  --kernel-dir /path/to/linux-6.6/arch/arm64/boot \
  --payload /path/to/05-gemini-payload/consys \
  --busybox /path/to/static-arm64-busybox

# full deliverable incl. rootfs loop image (needs root: mmdebstrap + mkfs.ext4 -d):
sudo release/assemble.sh ... --firmware-placeholder --build-rootfs
```

## CI — `.github/workflows/release.yml`

On push to `main` (+ `workflow_dispatch`): pulls the kernel-build artifact
(no kernel rebuild), builds the daemons, runs `assemble.sh --build-rootfs`,
proves the boot3 image **round-trips byte-exactly**, and boots the assembled
rootfs in **QEMU** asserting `GEMINI-MODULES-STAGED-OK`,
`GEMINI-FIRMWARE-STAGED-OK`, `GEMINI-DAEMONS-INERT-OK`, `GEMINI-SLICE10-VERIFY-OK`
(plus the Slice 4 pivot markers). Real firmware blobs never enter this public
workflow — CI stages named placeholders; the manifest still pins the catalog
hashes.

### Release qualification — green ≠ flash-ready unless proven (issue #24)

A `push`/default `workflow_dispatch` run is **release-qualified** and **fails
closed**:

- it uses the **real** `Image.gz` + DTB + three `.ko` from `kernel-build`; if
  none resolve it **errors out** (no placeholder fallback for a release run);
- `assemble.sh --verify-modules-dep` runs `depmod` on the staged overlay (the
  prebuilt path too, not just `--kbuild`) and asserts `modprobe -n
  --show-depends` resolves all three modules in the acyclic order
  `mtk_btif → mtk_stp_wmt_soc → wlan_gen3`;
- the run asserts the manifest's `fork_git_commit` **equals the release commit**
  (a stale committed manifest cannot ship), and that the manifest is stamped
  `release_qualified: yes` / `modules_dep_verified: yes`.

`workflow_dispatch` with **`mode: smoke`** keeps the placeholder path for
plumbing-only smoke tests; such a run stamps `release_qualified: no`, names its
artifact `…-NONRELEASE-smoke-…`, and can never be mistaken for flash-ready.

## Module set (issue #22)

Three modules load in the acyclic order `mtk_btif → mtk_stp_wmt_soc →
wlan_gen3`. The former `wmt_chrdev_wifi`↔`wlan_gen3` `EXPORT_SYMBOL` cycle was
resolved by merging the chardev into `wlan_gen3` (issue #22), so `depmod` emits
a valid `modules.dep` and `modprobe` works — the release job now **proves** this
on the shipped overlay. `wmt_chrdev_wifi.ko` no longer exists separately. See
`RUNBOOK.md` §4a.
