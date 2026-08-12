#!/usr/bin/env python3
"""mkinitramfs.py — build the Experimental-slot busybox initramfs as a
deterministic gzipped newc cpio (tracker #5, ADR-0002).

No root, no fakeroot, no mounts: the archive is written directly in python
with every entry owned by uid/gid 0, mtime 0, sorted paths and fixed inode
numbering, so the same inputs always produce a byte-identical
initramfs.cpio.gz (which in turn keeps bootimg.py pack output reproducible).

Contents:
  /init                    — initramfs/init from this repo (busybox ash)
  /bin/busybox             — STATIC arm64 busybox, passed via --busybox.
                             Never committed to the repo: CI fetches it from
                             the Debian busybox-static package at build time.
  /bin/<applet> symlinks   — the applets /init needs
  /lib/modules/preload/*.ko— optional, from --modules-dir (QEMU test kernels
                             need virtio/ext4/loop; the device kernel has
                             them built in — see PACKAGING.md)
  /dev/console, /dev/null  — static nodes so /init can speak before devtmpfs
  empty mount points       — /proc /sys /dev /host /newroot ...

Usage:
  mkinitramfs.py --busybox path/to/busybox --init initramfs/init \
      --out initramfs.cpio.gz [--modules-dir DIR]
"""
import argparse
import gzip
import io
import os
import stat
import sys

APPLETS = [
    "sh", "mount", "umount", "switch_root", "losetup", "insmod",
    "mkdir", "mknod", "cat", "ls", "echo", "sleep", "setsid", "cttyhack",
    "grep", "sed", "sort", "cut", "head", "uname", "readlink",
    "test", "[", "ln", "cp", "mv", "rm", "sync", "dmesg", "ps", "df",
]

DIRS = [
    "bin", "dev", "etc", "host", "lib", "lib/modules",
    "lib/modules/preload", "newroot", "proc", "run", "sys", "tmp",
]


class NewcWriter:
    """Minimal deterministic newc (SVR4 no-CRC) cpio writer."""

    def __init__(self, fileobj):
        self.f = fileobj
        self.ino = 721  # arbitrary fixed base; kernel ignores, determinism matters

    def _entry(self, name, mode, filesize, rdev=(0, 0), nlink=1):
        self.ino += 1
        fields = (
            self.ino, mode, 0, 0, nlink, 0, filesize,
            0, 0, rdev[0], rdev[1], len(name) + 1, 0,
        )
        hdr = b"070701" + b"".join(b"%08X" % f for f in fields)
        self.f.write(hdr + name.encode() + b"\x00")
        self._pad()

    def _pad(self):
        pos = self.f.tell()
        if pos % 4:
            self.f.write(b"\x00" * (4 - pos % 4))

    def directory(self, name, mode=0o755):
        self._entry(name, stat.S_IFDIR | mode, 0, nlink=2)

    def file(self, name, data, mode=0o644):
        self._entry(name, stat.S_IFREG | mode, len(data))
        self.f.write(data)
        self._pad()

    def symlink(self, name, target):
        t = target.encode()
        self._entry(name, stat.S_IFLNK | 0o777, len(t))
        self.f.write(t)
        self._pad()

    def chardev(self, name, major, minor, mode=0o600):
        self._entry(name, stat.S_IFCHR | mode, 0, rdev=(major, minor))

    def trailer(self):
        self._entry("TRAILER!!!", 0, 0, nlink=1)
        pos = self.f.tell()
        if pos % 512:
            self.f.write(b"\x00" * (512 - pos % 512))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--busybox", required=True,
                    help="static busybox binary (arm64 for the target)")
    ap.add_argument("--init", required=True, help="init script (initramfs/init)")
    ap.add_argument("--modules-dir", default=None,
                    help="dir of .ko files copied to /lib/modules/preload "
                         "in sorted-name order (name them NN-mod.ko to control "
                         "insmod order)")
    ap.add_argument("--out", required=True, help="output initramfs.cpio.gz")
    args = ap.parse_args()

    with open(args.busybox, "rb") as f:
        busybox = f.read()
    if busybox[:4] != b"\x7fELF":
        sys.exit(f"{args.busybox}: not an ELF binary")
    with open(args.init, "rb") as f:
        init = f.read()
    if not init.startswith(b"#!/bin/sh"):
        sys.exit(f"{args.init}: does not look like the init script")

    modules = []
    if args.modules_dir:
        for name in sorted(os.listdir(args.modules_dir)):
            if not name.endswith(".ko"):
                continue
            with open(os.path.join(args.modules_dir, name), "rb") as f:
                modules.append((name, f.read()))

    raw = io.BytesIO()
    w = NewcWriter(raw)
    for d in DIRS:
        w.directory(d)
    w.chardev("dev/console", 5, 1, 0o600)
    w.chardev("dev/null", 1, 3, 0o666)
    w.file("init", init, 0o755)
    w.file("bin/busybox", busybox, 0o755)
    for a in sorted(APPLETS):
        w.symlink(f"bin/{a}", "busybox")
    for name, data in modules:
        w.file(f"lib/modules/preload/{name}", data, 0o644)
    w.trailer()

    with open(args.out, "wb") as f:
        with gzip.GzipFile(filename="", fileobj=f, mode="wb", mtime=0,
                           compresslevel=9) as gz:
            gz.write(raw.getvalue())
    import hashlib
    digest = hashlib.sha256(open(args.out, "rb").read()).hexdigest()
    print(f"wrote {args.out}: {os.path.getsize(args.out)} bytes "
          f"(cpio {raw.tell()} bytes, {len(modules)} preload modules)")
    print(f"  sha256={digest}")


if __name__ == "__main__":
    main()
