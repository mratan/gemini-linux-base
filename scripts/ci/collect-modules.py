#!/usr/bin/env python3
"""collect-modules.py — resolve a module dependency closure out of an
extracted Debian kernel package and emit a flat, insmod-ordered directory
for mkinitramfs.py --modules-dir (tracker #5 QEMU proof).

The QEMU test kernel (stock Debian arm64) ships virtio/ext4/loop as .ko.xz
modules; busybox insmod can load neither compressed modules nor resolve
dependencies, so this script walks modules.dep, decompresses, and names the
output files NN-<mod>.ko so a sorted insmod loop loads dependencies first.
Modules absent from modules.dep are assumed built-in and skipped with a
note (the Gemini device kernel has all of these built in, so its initramfs
simply gets no preload dir at all).

Usage:
  collect-modules.py --modules-base <dir>/usr/lib/modules/<version> \
      --out <preload-dir> virtio_pci virtio_blk loop ext4 crc32c_generic
"""
import argparse
import gzip
import lzma
import os
import subprocess
import sys


def norm(name):
    return name.replace("-", "_")


def strip_ext(path):
    base = os.path.basename(path)
    for ext in (".ko.xz", ".ko.gz", ".ko.zst", ".ko"):
        if base.endswith(ext):
            return base[: -len(ext)]
    return base


def read_module(path):
    if path.endswith(".xz"):
        with lzma.open(path, "rb") as f:
            return f.read()
    if path.endswith(".gz"):
        with gzip.open(path, "rb") as f:
            return f.read()
    if path.endswith(".zst"):
        return subprocess.run(["zstd", "-d", "-c", path], check=True,
                              capture_output=True).stdout
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--modules-base", required=True,
                    help=".../lib/modules/<version> containing modules.dep")
    ap.add_argument("--out", required=True, help="output preload dir")
    ap.add_argument("modules", nargs="+", help="module names to include")
    args = ap.parse_args()

    dep_path = os.path.join(args.modules_base, "modules.dep")
    by_name = {}   # normalized module name -> relpath
    deps = {}      # relpath -> [dep relpaths]
    with open(dep_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            path, _, rest = line.partition(":")
            by_name[norm(strip_ext(path))] = path
            deps[path] = rest.split()

    ordered = []   # relpaths, dependencies first
    seen = set()

    def visit(relpath):
        if relpath in seen:
            return
        seen.add(relpath)
        for d in deps.get(relpath, []):
            visit(d)
        ordered.append(relpath)

    missing = []
    for want in args.modules:
        rel = by_name.get(norm(want))
        if rel is None:
            missing.append(want)
            continue
        visit(rel)

    os.makedirs(args.out, exist_ok=True)
    for i, rel in enumerate(ordered):
        src = os.path.join(args.modules_base, rel)
        name = f"{i:02d}-{strip_ext(rel)}.ko"
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(read_module(src))
        print(f"  {name}  <- {rel}")
    for m in missing:
        print(f"  note: {m} not in modules.dep — assuming built-in")
    print(f"collected {len(ordered)} module(s) into {args.out}")


if __name__ == "__main__":
    main()
