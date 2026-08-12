# Build evidence — Reference-lineage capture kernel (issue #9)

All artifacts staged, **NOT-YET-FLASHED**. Built 2026-08-12 on the project
workstation with rootless podman.

## Source tree

- `github.com/gemian/gemini-linux-kernel-3.18`, branch **`native`**
  (HEAD `ca2e0ca58` "Updating to latest value from upstream").
  This is the Reference slot's kernel lineage: `Makefile` says
  `VERSION=3 PATCHLEVEL=18 SUBLEVEL=41`, matching the on-device Gemian
  kernel 3.18.41 (`04-docs/STATE-2026-08-07.md`).
  The repo's own `README.md` titles itself `gemini-android-kernel-3.18` —
  the same lineage bsg100 instrumented on their Kali slot, which is why
  their mirrored patch 0002 applied **without a single hunk failure**
  (only the `kernel-3.18/` path prefix stripped).
  The `main` branch (3.18.79) is NOT used — it has no Wi-Fi driver
  (CLAUDE.md / CONTEXT.md warning).
- Clone location (gitignored): `07-kernel/gemian-3.18`, work branch
  `capture-kit` = `native` + the three patches in `capture-kit/patches/`.

## Period toolchain (containerized, reproducible)

- Why stretch/GCC 6.3: the gemian tree's `debian/changelog` targets
  `stretch` and `debian/rules` builds with the distro default compiler.
  Decisive cross-check: **the shipped Gemian kernel's own banner names
  the exact compiler** —

  ```
  Linux version 3.18.41+ (dguidi@nowhere) (gcc version 6.3.0 20170516 (Debian 6.3.0-18) ) #7 SMP PREEMPT Fri Mar 29 10:39:03 GMT 2019
  ```

  (extracted from the local `02-firmware/flash-set/debian_boot.img`
  kernel, 2026-08-12). Our container uses the same compiler as the
  stretch cross package.
- Base image: `docker.io/debian/eol:stretch`, pulled 2026-08-12, digest
  `sha256:9ae24aaa8a83aee801eef85327fd80d15a8d1340bbb3df4e1a00646795268d09`.
  Built toolchain image `localhost/gemian-3.18-stretch` digest
  `sha256:12bfa9f53917a117fb8398ab721d272ecab0764c751781bf4d8901016931538b`.
  Package integrity: apt verifies each .deb against the SHA256 hashes in
  the signed archive.debian.org Release/Packages files.
- Toolchain identity (recorded in `/toolchain-id.txt` inside the image):

  ```
  aarch64-linux-gnu-gcc (Debian 6.3.0-18) 6.3.0 20170516
  bc 1.06.95-9+b3
  binutils-aarch64-linux-gnu 2.28-5
  gcc-aarch64-linux-gnu 4:6.3.0-4
  make 4.1-9.1
  perl 5.24.1-3+deb9u7
  python2.7 2.7.13-2+deb9u6
  ```

## Build invocation

Exactly the gemian `debian/rules` sequence plus explicit CROSS_COMPILE
(`capture-kit/build.sh`):

```
make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gemini_modular_defconfig
make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
make O=/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) modules
```

Both builds (baseline = pristine `native`; capture = + 3 patches)
completed with zero errors on the first run — no build fixes of any kind
were needed with the period compiler (contrast: bsg100 needed 7 fix
patches to coax the same code through GCC 14).

## Results

| Build | Banner | Image.gz-dtb | Modules |
|---|---|---|---|
| baseline (`native` pristine) | `Linux version 3.18.41 (root@…) (gcc version 6.3.0 20170516 (Debian 6.3.0-18) ) #1 SMP PREEMPT` | 8 988 460 B | 1031 .ko |
| capture (+ patches 0001–0003) | same banner shape, `#1 SMP PREEMPT` | 8 989 798 B | 1031 .ko |

## Unmodified-build match to the shipped Gemian kernel

Evidence that the pristine `native` build is the same kernel the
Reference slot runs (no flashing involved — all local comparison against
`02-firmware/flash-set/debian_boot.img`, the shipped Gemian boot image):

1. **DTB byte-identical.** Our baseline `aeon6797_6m_n.dtb` has SHA-256
   `9e26929563f7682d1f7545d6007f0092c7e085a4edbd6e7be0ac8eaa5159b2f9` —
   **exactly** the DTB inside the shipped `debian_boot.img`.
2. **Same kernel release + same compiler.** Shipped banner
   `3.18.41+ … gcc 6.3.0 (Debian 6.3.0-18) #7 … 2019` vs ours `3.18.41 …
   gcc 6.3.0 (Debian 6.3.0-18) #1 … 2026`. The shipped `+` suffix means
   the 2019 build ran from a locally-modified tree; the release,
   compiler, and config lineage match.
3. **Same config path.** `debian/rules` configures with
   `gemini_modular_defconfig` — the config we build; connectivity
   options verified in the produced `.config`:
   `CONFIG_MTK_COMBO_CHIP_CONSYS_6797=y`, `CONFIG_MTK_COMBO=y`,
   `CONFIG_MTK_BTIF=y`, `CONFIG_MTK_COMBO_WIFI=y` (and the build log
   shows the wlan **gen3** Makefile included — CONTEXT.md's gen3 note
   confirmed at build level).
4. Kernel size differs from shipped (8 859 053 vs 8 299 080 gz bytes) —
   expected: 7 years of timestamp/toolchain-patch-level drift and the
   shipped tree's local modifications; the DTB identity plus
   banner/config identity is the match evidence that matters for
   "same lineage, boots the same userspace".

## CONSYS build products

The entire Vendor stack is **built-in** (`=y`), not modular — there are
no wmt/btif/wlan `.ko` files by design. Verified symbols in the capture
build's `System.map`:

```
ffffffc000721360 t harvest_snap
ffffffc000721730 T wmt_capture_regtable
ffffffc000721ab8 T wmt_capture_cpupcr_burst
ffffffc000725e60 t wmt_capture_write
```

## Staged boot image (see pack-boot2-compat.sh output)

- `boot3-capture-gemian.img` sha256
  `5716743b22afb6fe42ca18060f0e1cc97b8f364174887ec1d07d01e4bb791ef1`
  (15 347 712 B): capture kernel `Image.gz` + byte-identical gemian DTB +
  the **shipped Gemian ramdisk** (sha256 `a1ee05445e…` from
  `debian_boot.img`) + the shipped header facts (v0, page 2048,
  kernel_addr 0x40080000, cmdline `bootopt=64S3,32N2,64N2
  log_buf_len=4M`).
- Round-trip verified: unpacking the packed image returns kernel, DTB
  and ramdisk **byte-exact** (SHA-256 compared).
- Lives in `capture-kit/staging/` (gitignored — contains the Gemian
  ramdisk; blobs never enter git). Reproduce with
  `capture-kit/pack-boot2-compat.sh`.
