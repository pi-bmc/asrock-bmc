#!/usr/bin/env python3
"""
outerfv.py — append an FFS driver to the OUTER FVMAIN_COMPACT free space.

Why this exists separately from romsurgeon.py: romsurgeon mutates the *inner*
DXE volume, which means recompressing the 9E21FD93 LZMA container. That is
hardware-proven fatal on this board — every repack of that container, including
a no-op one whose payload decompressed byte-identically, hangs the host at POST
0x4F (DXE IPL). See .claude/host-bios-driver-injection-results.md.

This tool never touches a compressed byte. FVMAIN_COMPACT at 0x1AC1000 is a
plain uncompressed volume with ~583 KiB of erased tail, so a new FFS is simply
written into space that is already 0xFF. No existing byte changes, no checksum
outside the new file is affected, and the image is known to boot.

The open question this enables us to answer is whether the AMI DXE core
*dispatches* from this outer volume. EDK2's reference PeiCore installs an FV HOB
for every volume it discovers and DxeMain scans them all, so it should — but the
one time we tried it the verdict rested on IPMI request counts, which later
turned out to be irreproducible on this board. Pair this with OobProbeDxe, whose
NV-variable signal survives in the SPI flash regardless.

    python3 outerfv.py inspect --rom ROM
    python3 outerfv.py add --rom ROM --efi X64/OobProbeDxe.efi \
            --guid 7c4d1a92-... --name OobProbeDxe --out NEW.rom
"""

import argparse
import os
import struct
import sys
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import patch_rom as P

SEC_UI = 0x15


def ui_section_body(ui_name):
    """
    UTF-16 name for a UI section, padded so the SECTION'S DECLARED SIZE is a
    multiple of 4.

    This is not cosmetic. AMI's DXE core silently refuses to dispatch a file
    whose UI section size is not 4-aligned — proven on hardware 2026-07-30 with
    six byte-identical drivers that differed only in UI name length: the three
    with odd-length names (4-aligned UI sections) all ran, the three with
    even-length names all stayed silent. No error, no POST code, nothing; the
    file is simply skipped, and the FFS chain walk continues past it normally,
    which is why the failures looked like "every other file" at first.

    P.make_section pads its output to 4 bytes but declares size = len(data) + 4,
    excluding that pad — so an odd/even name flips whether the declared size is
    aligned. Padding the string itself with an extra NUL fixes it at the source
    and keeps the name a valid double-NUL-terminated UTF-16 string.
    """
    data = (ui_name + "\0").encode("utf-16-le")
    while (len(data) + 4) % 4:
        data += b"\0\0"
    return data


def guid_to_le(text):
    return uuid.UUID(text).bytes_le


def le_to_guid(raw):
    return str(uuid.UUID(bytes_le=bytes(raw)))


class OuterVolume:
    """FVMAIN_COMPACT itself, addressed directly in the flash image."""

    def __init__(self, rom):
        self.rom = bytearray(rom)
        self.base = P.FV_OFFSET
        fv = self.rom[self.base:self.base + P.FV_SIZE]
        self.hdr_len, self.fv_len = P.parse_fv(fv)
        self.content_start = P.ffs_content_start(fv)

    # -- view -------------------------------------------------------------

    def _slice(self, off, length):
        return bytes(self.rom[self.base + off:self.base + off + length])

    def files(self):
        off = self.content_start
        while off + 24 <= self.fv_len:
            # An all-0xFF *name* is not end-of-chain: FFS_PAD files legitimately
            # carry one, and this volume has a 24-byte pad at FV+0x310C0 right
            # before the LZMA container. Only a fully erased header terminates
            # the walk, which is what a conformant dispatcher does too.
            if self._slice(off, 24) == b"\xFF" * 24:
                break
            size = P.u24(self.rom, self.base + off + 20)
            if size < 24 or off + size > self.fv_len or size == 0xFFFFFF:
                break
            yield (off, size,
                   le_to_guid(self._slice(off, 16)),
                   self.rom[self.base + off + 18],
                   self.rom[self.base + off + 23])
            off = (off + size + 7) & ~7

    def free_span(self):
        """(start, length) of the trailing erased region, offsets FV-relative."""
        end = self.fv_len
        while end > self.content_start and self.rom[self.base + end - 1] == 0xFF:
            end -= 1
        start = (end + 7) & ~7
        return start, self.fv_len - start

    # -- mutate -----------------------------------------------------------

    def add_driver(self, guid_text, pe32, ui_name):
        """
        Write a DEPEX=TRUE DXE_DRIVER FFS at the head of the erased tail.

        Placed contiguously with the existing chain so the dispatcher's
        sequential walk reaches it — a gap of 0xFF would terminate that walk and
        the file would be invisible.
        """
        sections  = P.make_section(bytes([0x06, 0x08]), P.SEC_DXE_DEPEX)   # TRUE; END
        sections += P.make_section(pe32, P.SEC_PE32)
        sections += P.make_section(ui_section_body(ui_name), SEC_UI)
        ffs = P.make_ffs(guid_to_le(guid_text), sections, P.FFS_TYPE_DRIVER)

        start, avail = self.free_span()
        if len(ffs) > avail:
            raise RuntimeError(
                f"{ui_name}: needs {len(ffs)} bytes, only {avail} free in FVMAIN_COMPACT")

        abs_off = self.base + start
        if bytes(self.rom[abs_off:abs_off + len(ffs)]) != b"\xFF" * len(ffs):
            raise RuntimeError("target span is not erased; refusing to overwrite live data")

        self.rom[abs_off:abs_off + len(ffs)] = ffs
        return start, abs_off, len(ffs)

    def save(self, path, original):
        if len(self.rom) != len(original):
            raise RuntimeError("image size changed; refusing to write")
        # Everything outside the newly written span must be untouched. Cheap to
        # assert and it has caught real mistakes on this ROM before.
        with open(path, "wb") as fh:
            fh.write(self.rom)


def diff_report(a, b):
    """Byte ranges that differ, so the change can be eyeballed before flashing."""
    runs, start = [], None
    for i in range(len(a)):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(a)))
    return runs


def cmd_inspect(args):
    rom = open(args.rom, "rb").read()
    vol = OuterVolume(rom)
    print(f"FVMAIN_COMPACT @ 0x{vol.base:X}  hdr={vol.hdr_len}  len=0x{vol.fv_len:X}")
    for off, size, guid, ftype, state in vol.files():
        print(f"  FV+0x{off:07X}  abs 0x{vol.base + off:08X}  "
              f"{size:>9,}  type 0x{ftype:02X}  state 0x{state:02X}  {guid}")
    start, avail = vol.free_span()
    print(f"  free: FV+0x{start:X} (abs 0x{vol.base + start:X})  {avail:,} bytes")


def cmd_add(args):
    original = open(args.rom, "rb").read()
    vol = OuterVolume(original)
    pe32 = open(args.efi, "rb").read()

    start, abs_off, size = vol.add_driver(args.guid, pe32, args.name)
    print(f"{args.name}: {len(pe32):,} B PE32 -> {size:,} B FFS "
          f"at FV+0x{start:X} (abs 0x{abs_off:X})")

    out = args.out or (args.rom + ".injected")
    vol.save(out, original)

    new = open(out, "rb").read()
    runs = diff_report(original, new)
    print(f"changed byte ranges: {len(runs)}")
    for s, e in runs:
        print(f"  0x{s:08X}-0x{e:08X}  ({e - s:,} bytes)")
    expected = [(abs_off, abs_off + size)]
    # The FFS may end in bytes that happened to already be 0xFF, so compare the
    # containing span rather than requiring an exact match.
    ok = all(abs_off <= s and e <= abs_off + size for s, e in runs)
    print("all changes inside the new FFS:", ok)
    if not ok:
        raise SystemExit("unexpected modifications outside the injected file")
    print(f"wrote {out} ({len(new):,} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("inspect", help="list FFS files in FVMAIN_COMPACT")
    p.add_argument("--rom", required=True)
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("add", help="append a driver FFS to the erased tail")
    p.add_argument("--rom", required=True)
    p.add_argument("--efi", required=True)
    p.add_argument("--guid", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_add)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
