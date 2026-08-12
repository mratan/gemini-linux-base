#!/usr/bin/env python3
"""bootimg.py — reproducible pack/unpack of LK-compatible Android v0 boot
images for the Gemini PDA Experimental slot (boot3 — ADR-0002, tracker #5).

Differences from scripts/pack-boot-img.py (which is kept unchanged, it is the
proven boot2 packer used by build-pack.sh):

  * header fields come from a small committed JSON spec
    (scripts/boot3-header.json) instead of requiring a proprietary reference
    boot.img on disk — the spec holds only integer/string facts extracted
    from the known-good images (values cross-checked against three local
    reference images on 2026-08-12 and against boot.md "Android boot image
    header"), no vendor code or blobs;
  * the ramdisk is OUR initramfs (see scripts/mkinitramfs.py), not a copy of
    the vendor Mer Boat Loader ramdisk;
  * there is an `unpack` subcommand that round-trips: the unpacked kernel
    Image.gz, DTB, cmdline and initramfs byte-match what was packed
    (tested by scripts/test-bootimg-roundtrip.py, run in CI);
  * output is deterministic: same inputs -> byte-identical image.

LK facts this relies on (boot.md, hardware-verified 2026-07-04):
  * standard AOSP v0 header, magic ANDROID! at offset 0, no MTK per-section
    wrapper;
  * LK ignores the header kernel_addr and always loads at dram_base+0x80000
    (0x40080000); the kernel must be CONFIG_RELOCATABLE=y (arm64 default);
  * the DTB is appended to Image.gz inside the "kernel" section;
  * the header cmdline is passed by LK but the project kernel builds with
    CONFIG_CMDLINE_FORCE=y, so the effective cmdline is configs/
    gemini-cmdline.config — the header cmdline still round-trips here.

Safety (ADR-0002): this tool only ever reads/writes regular files passed as
arguments. It never touches block devices or partitions. Output images are
staged artifacts, NOT-YET-FLASHED.

Usage:
  bootimg.py pack --spec scripts/boot3-header.json \
      --kernel Image.gz --dtb mt6797-gemini-pda.dtb \
      --ramdisk initramfs.cpio.gz --out boot3-experimental.img \
      [--cmdline "..."]
  bootimg.py unpack --img boot3-experimental.img --out-dir unpacked/
  bootimg.py inspect --img some_boot.img
"""
import argparse
import hashlib
import json
import struct
import sys

MAGIC = b"ANDROID!"
GZIP_MAGIC = b"\x1f\x8b"
DTB_MAGIC = b"\xd0\x0d\xfe\xed"  # big-endian FDT magic

SPEC_INT_FIELDS = (
    "kernel_addr", "ramdisk_addr", "second_addr", "tags_addr",
    "page_size", "header_version", "os_version",
)


def _to_int(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def load_spec(path):
    with open(path) as f:
        raw = json.load(f)
    spec = {k: _to_int(raw[k]) for k in SPEC_INT_FIELDS}
    spec["name"] = raw.get("name", "")
    spec["cmdline"] = raw.get("cmdline", "")
    return spec


def pad(data: bytes, page_size: int) -> bytes:
    rem = len(data) % page_size
    return data if rem == 0 else data + b"\x00" * (page_size - rem)


def parse_header(buf: bytes) -> dict:
    if buf[0:8] != MAGIC:
        raise ValueError(f"not an Android boot image (magic={buf[0:8]!r})")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
     second_size, second_addr, tags_addr, page_size,
     header_version, os_version) = struct.unpack_from("<10I", buf, 8)
    if header_version != 0:
        raise ValueError(f"only v0 headers are supported (got {header_version})")
    return dict(
        kernel_size=kernel_size, kernel_addr=kernel_addr,
        ramdisk_size=ramdisk_size, ramdisk_addr=ramdisk_addr,
        second_size=second_size, second_addr=second_addr,
        tags_addr=tags_addr, page_size=page_size,
        header_version=header_version, os_version=os_version,
        name=buf[48:64].rstrip(b"\x00").decode(),
        cmdline=buf[64:576].split(b"\x00", 1)[0].decode(),
        ids=buf[576:608],
    )


def aosp_id(kernel: bytes, ramdisk: bytes, second: bytes) -> bytes:
    """SHA-1 over sections+sizes, zero-padded to 32 bytes — same digest
    mkbootimg computes into the id[] field. Purely informational (LK does
    not verify it) but deterministic and useful as an integrity tag."""
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        h.update(blob)
        h.update(struct.pack("<I", len(blob)))
    return h.digest().ljust(32, b"\x00")


def build_image(spec: dict, kernel_blob: bytes, ramdisk: bytes,
                cmdline: str) -> bytes:
    page_size = spec["page_size"]
    name_b = spec["name"].encode()
    cmdline_b = cmdline.encode()
    if len(name_b) > 16:
        raise ValueError("name longer than 16 bytes")
    if len(cmdline_b) > 512:
        raise ValueError("cmdline longer than 512 bytes")
    hdr = bytearray(page_size)
    hdr[0:8] = MAGIC
    struct.pack_into(
        "<10I", hdr, 8,
        len(kernel_blob), spec["kernel_addr"],
        len(ramdisk), spec["ramdisk_addr"],
        0, spec["second_addr"],
        spec["tags_addr"], page_size,
        spec["header_version"], spec["os_version"],
    )
    hdr[48:48 + len(name_b)] = name_b
    hdr[64:64 + len(cmdline_b)] = cmdline_b
    hdr[576:608] = aosp_id(kernel_blob, ramdisk, b"")
    return bytes(hdr) + pad(kernel_blob, page_size) + pad(ramdisk, page_size)


def split_kernel_blob(blob: bytes):
    """Split an (Image.gz + appended DTB) kernel section back into its two
    inputs, byte-exactly.

    Primary method: the kernel is a gzip stream — decompress it and
    zlib reports exactly where the stream ends (unused_data == the DTB).
    Fallback (uncompressed Image): scan for an FDT magic whose big-endian
    totalsize field reaches exactly the end of the blob."""
    if blob[:2] == GZIP_MAGIC:
        import zlib
        d = zlib.decompressobj(31)
        d.decompress(blob)
        if not d.eof:
            raise ValueError("truncated gzip kernel stream")
        dtb = d.unused_data
        kernel = blob[:len(blob) - len(dtb)]
        if dtb and dtb[:4] != DTB_MAGIC:
            raise ValueError("bytes after gzip kernel are not a DTB")
        return kernel, dtb
    # Uncompressed kernel: find the last FDT magic that closes the blob.
    pos = len(blob)
    while True:
        pos = blob.rfind(DTB_MAGIC, 0, pos)
        if pos < 0:
            return blob, b""  # no appended DTB
        if pos + 8 <= len(blob):
            (totalsize,) = struct.unpack_from(">I", blob, pos + 4)
            if pos + totalsize == len(blob):
                return blob[:pos], blob[pos:]


def cmd_pack(args):
    spec = load_spec(args.spec)
    with open(args.kernel, "rb") as f:
        kernel = f.read()
    with open(args.dtb, "rb") as f:
        dtb = f.read()
    if dtb[:4] != DTB_MAGIC:
        sys.exit(f"{args.dtb}: not a DTB (bad magic)")
    with open(args.ramdisk, "rb") as f:
        ramdisk = f.read()
    cmdline = args.cmdline if args.cmdline is not None else spec["cmdline"]
    img = build_image(spec, kernel + dtb, ramdisk, cmdline)
    with open(args.out, "wb") as f:
        f.write(img)
    print(f"wrote {args.out}: {len(img)} bytes  "
          f"sha256={hashlib.sha256(img).hexdigest()}")
    print(f"  kernel blob: {len(kernel) + len(dtb)} bytes "
          f"(kernel {len(kernel)} + dtb {len(dtb)})")
    print(f"  ramdisk:     {len(ramdisk)} bytes")
    print(f"  cmdline:     {cmdline!r}")
    print("  status:      staged artifact — NOT-YET-FLASHED")


def cmd_unpack(args):
    import os
    with open(args.img, "rb") as f:
        buf = f.read()
    hdr = parse_header(buf)
    page = hdr["page_size"]

    def pages(n):
        return (n + page - 1) // page

    koff = page
    roff = koff + pages(hdr["kernel_size"]) * page
    soff = roff + pages(hdr["ramdisk_size"]) * page
    kernel_blob = buf[koff:koff + hdr["kernel_size"]]
    ramdisk = buf[roff:roff + hdr["ramdisk_size"]]
    second = buf[soff:soff + hdr["second_size"]]
    kernel, dtb = split_kernel_blob(kernel_blob)

    os.makedirs(args.out_dir, exist_ok=True)

    def w(rel, data):
        p = os.path.join(args.out_dir, rel)
        with open(p, "wb") as f:
            f.write(data)
        print(f"  {rel}: {len(data)} bytes  "
              f"sha256={hashlib.sha256(data).hexdigest()}")

    print(f"unpacking {args.img} -> {args.out_dir}/")
    w("kernel.gz" if kernel[:2] == GZIP_MAGIC else "kernel", kernel)
    if dtb:
        w("dtb", dtb)
    w("ramdisk.cpio.gz" if ramdisk[:2] == GZIP_MAGIC else "ramdisk", ramdisk)
    if second:
        w("second", second)
    with open(os.path.join(args.out_dir, "cmdline.txt"), "w") as f:
        f.write(hdr["cmdline"] + "\n")
    spec_out = {k: hex(hdr[k]) if k.endswith("_addr") else hdr[k]
                for k in SPEC_INT_FIELDS}
    spec_out["name"] = hdr["name"]
    spec_out["cmdline"] = hdr["cmdline"]
    with open(os.path.join(args.out_dir, "header.json"), "w") as f:
        json.dump(spec_out, f, indent=2)
        f.write("\n")
    print(f"  cmdline.txt: {hdr['cmdline']!r}")
    print("  header.json: re-packable header spec")


def cmd_inspect(args):
    with open(args.img, "rb") as f:
        buf = f.read(4096)
    hdr = parse_header(buf)
    for k in ("kernel_size", "ramdisk_size", "second_size"):
        print(f"{k:16} {hdr[k]}")
    for k in ("kernel_addr", "ramdisk_addr", "second_addr", "tags_addr"):
        print(f"{k:16} {hdr[k]:#x}")
    print(f"{'page_size':16} {hdr['page_size']}")
    print(f"{'os_version':16} {hdr['os_version']:#x}")
    print(f"{'name':16} {hdr['name']!r}")
    print(f"{'cmdline':16} {hdr['cmdline']!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="pack kernel+dtb+ramdisk into a v0 boot image")
    p.add_argument("--spec", required=True, help="header spec JSON (scripts/boot3-header.json)")
    p.add_argument("--kernel", required=True, help="Image.gz (or uncompressed Image)")
    p.add_argument("--dtb", required=True, help="board DTB, appended after the kernel")
    p.add_argument("--ramdisk", required=True, help="initramfs cpio.gz")
    p.add_argument("--cmdline", default=None, help="override the spec's header cmdline")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("unpack", help="unpack a v0 boot image (round-trip)")
    p.add_argument("--img", required=True)
    p.add_argument("--out-dir", required=True)
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("inspect", help="print header fields")
    p.add_argument("--img", required=True)
    p.set_defaults(func=cmd_inspect)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
