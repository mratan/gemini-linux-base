# PACKAGING.md — Experimental-slot packaging pipeline (boot3 + loop image)

Status: **device-free, all artifacts staged and NOT-YET-FLASHED.** Nothing in
this pipeline runs against hardware; nothing gets flashed until the
remote-only period ends. Tracker: gemini_linux issue #5 ("Slice 4"),
decision record: ADR-0002 in the gemini_linux repo.

## The layout this packages for (ADR-0002 — binding)

The Experimental slot deliberately does **not** follow this repo's original
(bsg100) layout of "kernel in boot2, rootfs owning the `linux` partition" —
on the target device boot2 + the `linux` partition's own rootfs are **Gemian,
the working-Internal-Wi-Fi reference system, and must never be written,
resized or reformatted**. Instead:

| Piece | Where it goes | Packaged by |
|-------|---------------|-------------|
| 6.6 kernel (Image.gz + appended DTB) + our initramfs | **boot3** (replacing UBports, restorable from its TWRP backup) | `scripts/bootimg.py pack` |
| Debian 13 arm64 rootfs | **a loop-image file** (`/experimental.img`) placed on the `linux` partition **beside** Gemian's rootfs | `scripts/mkrootfs-loop.sh` |
| The glue that mounts the image and pivots | busybox initramfs inside the boot3 image | `initramfs/init` + `scripts/mkinitramfs.py` |

A reader following this repo's INSTALL.md verbatim would flash boot2/`linux`
and destroy the reference slot — do not. Only `boot3` (kernel) and a **file
copied onto** the `linux` partition's filesystem are ever involved, and even
those steps are out of scope until the device is in hand.

**Safety property of the pipeline itself:** every script here operates only
on regular files passed as arguments (images are populated with
`mkfs.ext4 -d`, never by mounting). No script references a block device
path; device names appear only in documentation like this file.

## 1. Boot image packer — `scripts/bootimg.py`

Packs (kernel `Image.gz` + appended DTB + header cmdline + our initramfs)
into the LK-compatible Android **v0** boot image that MediaTek LK on this
device boots. Header facts (hardware-verified in boot.md, cross-checked
against three local reference images):

- standard AOSP v0 header, no MTK section wrapper; page size 2048;
- `kernel_addr` 0x40080000 / `ramdisk_addr` 0x45000000 / `tags_addr`
  0x44000000 — recorded in the committed spec `scripts/boot3-header.json`
  (plain numbers, **no vendor blob or code is required to pack**);
- LK *ignores* the header `kernel_addr` and always loads at
  `dram_base+0x80000`, so the kernel must keep `CONFIG_RELOCATABLE=y`;
- the header cmdline round-trips but is overridden at boot by the project
  kernel's `CONFIG_CMDLINE_FORCE=y` (`configs/gemini-cmdline.config`) — any
  initramfs parameter (`gemini.rootimg=` etc.) for the device therefore goes
  in that config fragment, not in the boot image.

`scripts/pack-boot-img.py` (boot2, copies header+ramdisk from a local
reference image) is unchanged; `bootimg.py` is its spec-driven,
round-trippable successor for boot3.

```sh
# pack (kernel + DTB from the kernel-build CI artifact):
scripts/bootimg.py pack --spec scripts/boot3-header.json \
    --kernel Image.gz --dtb mt6797-gemini-pda.dtb \
    --ramdisk initramfs-gemini.cpio.gz --out boot3-experimental.img

# unpack / round-trip check:
scripts/bootimg.py unpack --img boot3-experimental.img --out-dir unpacked/
```

Packing is deterministic (same inputs → byte-identical image) and unpacking
round-trips byte-exactly: kernel, DTB, cmdline and initramfs all come back
as the exact bytes that went in (the DTB split is recovered from the gzip
stream boundary). `scripts/test-bootimg-roundtrip.py` asserts all of this in
CI on synthetic fixtures; run locally with
`python3 scripts/test-bootimg-roundtrip.py`. The tool also byte-exactly
re-packs the vendor reference images from their unpacked pieces (verified
locally 2026-08-12 against the stock flash set — those blobs stay outside
git).

## 2. Initramfs — `initramfs/init` + `scripts/mkinitramfs.py`

A busybox-static initramfs whose only job is to find the `linux` partition,
loop-mount `/experimental.img` from it, and `switch_root` into the image.

**Discovery strategy** (no device paths hardcoded):

1. `gemini.rootdev=<name>` on the kernel cmdline, if present, is tried first;
2. otherwise every partition whose **GPT partition name is `linux`**
   (kernel `PARTNAME=` uevent field) — on the device that GPT entry is
   `mmcblk0p29`; on the QEMU test disk it is `vda1`; the same probe finds
   both;
3. any remaining partition as a fallback.

Each candidate is mounted **read-only** and probed for the image file
(`gemini.rootimg=`, default `/experimental.img`); the image is then
loop-mounted read-only and validated (`/sbin/init` executable, overridable
via `gemini.init=`). **Everything stays read-only until the pivot is
committed**; the single write-enabling action is the `remount,rw` of the
host partition immediately before `switch_root`. On **any** failure before
that, the initramfs unmounts what it probed and drops to a rescue shell
having written nothing (`gemini.rescue` on the cmdline forces this). After
the pivot the host partition mount is moved to `/host` in the rootfs.

Build (deterministic gzipped newc cpio, no root/fakeroot needed):

```sh
scripts/mkinitramfs.py --busybox <static-arm64-busybox> \
    --init initramfs/init --out initramfs-gemini.cpio.gz
```

The busybox binary is **never committed**: CI fetches it from the Debian
`busybox-static` arm64 package (`scripts/ci/fetch-qemu-boot-bits.sh`). The
device kernel needs no modules in the initramfs (devtmpfs/ext4/loop are
built into the arm64 defconfig the build uses); the QEMU test variant adds
the stock Debian kernel's virtio/ext4/loop modules via
`scripts/ci/collect-modules.py` + `--modules-dir`.

## 3. Loop-image rootfs — `scripts/mkrootfs-loop.sh`

Scripted (never hand-built) Debian 13 (trixie) arm64 rootfs, packed into an
ext4 image with `mkfs.ext4 -d` (no mounting):

```sh
sudo scripts/mkrootfs-loop.sh --out experimental.img
```

Runs in CI on ubuntu-24.04 (mmdebstrap + qemu-user-static). It is the
loop-image counterpart of `scripts/mkrootfs.sh` (bsg100's whole-partition
builder, kept as reference): minbase + systemd/udev only, so the published
artifact stays small; the full desktop package set migrates over from
`mkrootfs.sh` once the boot path is proven on hardware. Dev conveniences
(serial autologin, passwordless root, boot markers) are for the QEMU proof
and early bring-up — harden before this ever becomes a daily driver. The
image carries `/etc/gemini-experimental-release` with build provenance and
`STATUS=NOT-YET-FLASHED`.

On the device the image would be copied (by hand, in a future physical
session, never by this pipeline) onto the `linux` partition's filesystem as
`/experimental.img`, exactly like the existing `ubuntu.img` precedent — a
plain file write, no partition-level operation of any kind.

## 4. QEMU proof — `scripts/mksynthetic-disk.sh` + `scripts/ci/run-qemu-test.sh`

CI boots a **stock Debian arm64 kernel** (the gemini kernel needn't boot
QEMU; the initramfs logic is kernel-agnostic) on `qemu-system-aarch64 -M
virt` against a synthetic disk that mimics the real layout: one GPT
partition named `linux` containing a fake Gemian tree (with canary file and
a decoy `/sbin/init`) and, for Test A, `experimental.img` beside it.

- **Test A**: initramfs finds the image by partition name, pivots, and the
  loop rootfs boots to an interactive shell — asserted via serial markers
  `GEMINI-INITRAMFS-PIVOT-OK`, `GEMINI-ROOTFS-SHELL-OK`,
  `GEMINI-LOGIN-SHELL-OK`; the decoy marker must never appear.
- **Test B**: image absent → `GEMINI-INITRAMFS-RESCUE` appears, pivot marker
  must not, and the disk file's SHA-256 is **unchanged** after the run —
  the fail-safe path provably writes nothing to the partition that (on the
  real device) holds Gemian.

## CI

Workflow: `.github/workflows/packaging.yml` (triggers scoped to the
`slice4-packaging` branch + manual dispatch; `kernel-build.yml` belongs to
another workstream and is untouched). Jobs:

1. `bootimg-roundtrip` — packer determinism + byte-exact round-trip on
   synthetic fixtures (no kernel rebuild, seconds);
2. `rootfs-qemu-proof` — builds `experimental.img`, both initramfs
   variants, the synthetic disks, runs Tests A and B, and uploads hashed
   artifacts (`experimental.img.xz`, `initramfs-gemini.cpio.gz`, serial
   logs, `SHA256SUMS`, `NOT-YET-FLASHED.txt`).

## What still needs the device (out of scope here)

- Flashing the packed boot3 image (`mtk w boot3 …` — single-partition
  targeted write only, never `mtk wl`, never anything touching the GPT or
  nvram) and copying `experimental.img` onto the `linux` partition.
- Verifying LK's boot3 handoff behaves like boot2's (the packer reuses the
  hardware-verified boot2 header facts; the silver-key boot3 selection is
  confirmed in boot.md).
- Real-hardware validation of the initramfs probe against the eMMC GPT.
