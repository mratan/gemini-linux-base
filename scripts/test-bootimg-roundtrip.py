#!/usr/bin/env python3
"""Round-trip + reproducibility test for scripts/bootimg.py (tracker #5,
acceptance criterion 1). Pure-python, no root, no device, no network: the
kernel/DTB/ramdisk fixtures are tiny synthetic files generated on the fly
(CI must not rebuild the real kernel for this).

Asserts:
  1. pack -> unpack round-trips: kernel Image.gz, DTB, cmdline and initramfs
     all byte-match what went in;
  2. pack is deterministic: packing the same inputs twice gives
     byte-identical images;
  3. re-packing from the unpacked pieces + unpacked header.json reproduces
     the original image byte-identically;
  4. header fields land where LK expects them (v0 layout, page-aligned
     sections).

Run:  python3 scripts/test-bootimg-roundtrip.py
"""
import gzip
import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BOOTIMG = os.path.join(HERE, "bootimg.py")
SPEC = os.path.join(HERE, "boot3-header.json")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def make_fixture_kernel(rng) -> bytes:
    """A gzip stream like a real Image.gz (deterministic: mtime=0)."""
    payload = bytes(rng.randrange(256) for _ in range(200_000))
    import io
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as f:
        f.write(payload)
    return buf.getvalue()


def make_fixture_dtb(rng) -> bytes:
    """A structurally-minimal FDT blob: correct magic + totalsize, random
    body — enough for the packer/unpacker's validation, no dtc needed."""
    body = bytes(rng.randrange(256) for _ in range(30_000))
    total = 4 + 4 + len(body)
    return b"\xd0\x0d\xfe\xed" + struct.pack(">I", total) + body


def make_fixture_ramdisk(rng) -> bytes:
    import io
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as f:
        f.write(bytes(rng.randrange(256) for _ in range(50_000)))
    return buf.getvalue()


def run(*argv):
    r = subprocess.run([sys.executable, BOOTIMG, *argv],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FAIL: bootimg.py {' '.join(argv)}\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    rng = random.Random(1519)  # fixed seed: fixtures are deterministic too
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        kernel = make_fixture_kernel(rng)
        dtb = make_fixture_dtb(rng)
        ramdisk = make_fixture_ramdisk(rng)
        cmdline = "bootopt=64S3,32N2,64N2 log_buf_len=4M"

        kf, df, rf = (os.path.join(td, n) for n in ("Image.gz", "board.dtb", "initramfs.cpio.gz"))
        for path, data in ((kf, kernel), (df, dtb), (rf, ramdisk)):
            with open(path, "wb") as f:
                f.write(data)

        img1 = os.path.join(td, "boot3-a.img")
        img2 = os.path.join(td, "boot3-b.img")
        run("pack", "--spec", SPEC, "--kernel", kf, "--dtb", df,
            "--ramdisk", rf, "--cmdline", cmdline, "--out", img1)
        run("pack", "--spec", SPEC, "--kernel", kf, "--dtb", df,
            "--ramdisk", rf, "--cmdline", cmdline, "--out", img2)

        a = open(img1, "rb").read()
        b = open(img2, "rb").read()

        def check(name, ok):
            nonlocal failures
            print(f"  [{'ok' if ok else 'FAIL'}] {name}")
            if not ok:
                failures += 1

        print("reproducibility:")
        check("pack twice -> byte-identical image", a == b)

        print("v0 layout:")
        page = 2048
        check("magic ANDROID! at offset 0", a[:8] == b"ANDROID!")
        ksize, kaddr = struct.unpack_from("<2I", a, 8)
        rsize, raddr = struct.unpack_from("<2I", a, 16)
        check("kernel_size == len(Image.gz)+len(dtb)",
              ksize == len(kernel) + len(dtb))
        check("ramdisk_size == len(initramfs)", rsize == len(ramdisk))
        check("kernel_addr / ramdisk_addr from spec",
              kaddr == 0x40080000 and raddr == 0x45000000)
        check("image length page-aligned", len(a) % page == 0)
        kpages = (ksize + page - 1) // page
        check("ramdisk section starts page-aligned after kernel",
              a[page + kpages * page: page + kpages * page + len(ramdisk)] == ramdisk)

        print("round-trip (unpack):")
        outdir = os.path.join(td, "unpacked")
        run("unpack", "--img", img1, "--out-dir", outdir)
        uk = open(os.path.join(outdir, "kernel.gz"), "rb").read()
        ud = open(os.path.join(outdir, "dtb"), "rb").read()
        ur = open(os.path.join(outdir, "ramdisk.cpio.gz"), "rb").read()
        uc = open(os.path.join(outdir, "cmdline.txt")).read().strip()
        check("kernel byte-match", uk == kernel)
        check("dtb byte-match", ud == dtb)
        check("initramfs byte-match", ur == ramdisk)
        check("cmdline match", uc == cmdline)

        print("re-pack from unpacked pieces:")
        img3 = os.path.join(td, "boot3-c.img")
        run("pack", "--spec", os.path.join(outdir, "header.json"),
            "--kernel", os.path.join(outdir, "kernel.gz"),
            "--dtb", os.path.join(outdir, "dtb"),
            "--ramdisk", os.path.join(outdir, "ramdisk.cpio.gz"),
            "--out", img3)
        c = open(img3, "rb").read()
        check("re-packed image byte-identical to original", c == a)

        print(f"\nimage sha256 {sha(a)}")
    if failures:
        sys.exit(f"{failures} check(s) FAILED")
    print("all round-trip checks passed")


if __name__ == "__main__":
    main()
