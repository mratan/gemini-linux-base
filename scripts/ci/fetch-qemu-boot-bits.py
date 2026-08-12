#!/usr/bin/env python3
"""fetch-qemu-boot-bits.py — download, from the Debian archive, the pieces
the QEMU proof needs but which must never be committed to this public repo
as binaries (tracker #5):

  * the stock Debian arm64 kernel (vmlinuz + modules) that boots
    qemu -M virt — the gemini kernel needn't boot QEMU;
  * the static arm64 busybox from the Debian busybox-static package, for
    scripts/mkinitramfs.py.

Pure stdlib + no root: resolves the concrete linux-image-<ver>-arm64
package via the Packages.xz index (linux-image-arm64 itself is an empty
metapackage — extracting it yields no /boot, which is why the earlier
mmdebstrap --variant=extract approach failed), downloads the .debs, and
unpacks their data.tar members with a built-in ar parser + tarfile.
Runs depmod over the module tree afterwards (package postinst normally does
that; module-to-module deps need no System.map).

Usage:  fetch-qemu-boot-bits.py --dest DIR [--suite trixie] [--mirror URL]
Prints (stdout):  VMLINUZ=... MODULES_BASE=... BUSYBOX=...  (one per line,
GITHUB_ENV-ready). Everything else goes to stderr.
"""
import argparse
import io
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def fetch(url):
    log(f"GET {url}")
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def parse_packages(data):
    """Packages index -> {package-name: {field: value}} (last wins)."""
    pkgs = {}
    for stanza in data.decode("utf-8", "replace").split("\n\n"):
        fields = {}
        for line in stanza.split("\n"):
            if line[:1].isalpha() and ":" in line:
                k, _, v = line.partition(":")
                fields[k] = v.strip()
        if "Package" in fields:
            pkgs[fields["Package"]] = fields
    return pkgs


def ar_members(blob):
    """Minimal ar(5) reader: yields (name, bytes) for each member."""
    assert blob[:8] == b"!<arch>\n", "not an ar archive"
    off = 8
    while off + 60 <= len(blob):
        hdr = blob[off:off + 60]
        name = hdr[0:16].decode().strip().rstrip("/")
        size = int(hdr[48:58].decode().strip())
        data = blob[off + 60: off + 60 + size]
        yield name, data
        off += 60 + size + (size % 2)


def extract_deb(deb, dest):
    """Unpack a .deb's data.tar.{xz,gz,zst,bz2} into dest (no root)."""
    for name, data in ar_members(deb):
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            data = subprocess.run(["zstd", "-d", "-c"], input=data,
                                  check=True, capture_output=True).stdout
            name = "data.tar"
        with tarfile.open(fileobj=io.BytesIO(data),
                          mode="r:*" if "." in name else "r:") as tf:
            # strip leading "./"; skip anything trying to escape dest
            for m in tf.getmembers():
                p = os.path.normpath(m.name)
                if p.startswith("..") or os.path.isabs(p):
                    continue
                tf.extract(m, dest, filter="tar")
        return
    raise RuntimeError("no data.tar member found in .deb")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dest", required=True)
    ap.add_argument("--suite", default="trixie")
    ap.add_argument("--mirror", default="http://deb.debian.org/debian")
    args = ap.parse_args()

    os.makedirs(args.dest, exist_ok=True)
    idx = lzma.decompress(fetch(
        f"{args.mirror}/dists/{args.suite}/main/binary-arm64/Packages.xz"))
    pkgs = parse_packages(idx)

    # linux-image-arm64 is a metapackage; its Depends names the real image.
    meta = pkgs.get("linux-image-arm64")
    if not meta:
        sys.exit("linux-image-arm64 not found in Packages index")
    m = re.search(r"(linux-image-\S+-arm64)", meta.get("Depends", ""))
    if not m:
        sys.exit(f"cannot resolve kernel image from Depends: {meta.get('Depends')!r}")
    kernel_pkg = m.group(1)
    log(f"resolved kernel package: {kernel_pkg} "
        f"(version {pkgs[kernel_pkg]['Version']})")

    for name in (kernel_pkg, "busybox-static"):
        deb = fetch(f"{args.mirror}/{pkgs[name]['Filename']}")
        log(f"  {name}: {len(deb)} bytes, extracting")
        extract_deb(deb, args.dest)

    def one(pattern, globdir):
        import glob as g
        hits = sorted(g.glob(os.path.join(globdir, pattern)))
        return hits[-1] if hits else None

    vmlinuz = one("vmlinuz-*-arm64", os.path.join(args.dest, "boot"))
    modules_base = (one("*-arm64", os.path.join(args.dest, "usr/lib/modules"))
                    or one("*-arm64", os.path.join(args.dest, "lib/modules")))
    busybox = os.path.join(args.dest, "usr/bin/busybox")
    if not os.path.isfile(busybox):
        busybox = os.path.join(args.dest, "bin/busybox")
    if not vmlinuz or not modules_base or not os.path.isfile(busybox):
        sys.exit(f"extraction incomplete: vmlinuz={vmlinuz} "
                 f"modules_base={modules_base} busybox={busybox}")

    # The package's postinst (not run here) would have generated modules.dep.
    if not os.path.isfile(os.path.join(modules_base, "modules.dep")):
        kver = os.path.basename(modules_base)
        basedir = modules_base[: modules_base.rindex("/lib/modules/")]
        log(f"running depmod -b {basedir} {kver}")
        subprocess.run(["depmod", "-b", basedir, kver],
                       check=False, stderr=subprocess.DEVNULL)
    if not os.path.isfile(os.path.join(modules_base, "modules.dep")):
        sys.exit("depmod did not produce modules.dep")

    print(f"VMLINUZ={vmlinuz}")
    print(f"MODULES_BASE={modules_base}")
    print(f"BUSYBOX={busybox}")


if __name__ == "__main__":
    main()
