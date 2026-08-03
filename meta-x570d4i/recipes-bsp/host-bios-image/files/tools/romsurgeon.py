#!/usr/bin/env python3
"""
romsurgeon.py — strip AMI modules from, and inject our own drivers into, the
X570D4I-2T's dispatched DXE firmware volume.

Why this shape
--------------
A from-scratch EDK2 build is not on the table for this board: the DXE volume is
reconstructable, but the PEI phase and AMD AGESA silicon init for AM4 (Vermeer)
exist only as vendor binaries, and no open replacement covers AM4.  So the
working model is subtractive — keep the stock ROM's boot path, delete the
proprietary out-of-band management modules, and graft in EDK2-built replacements.

How removal works
-----------------
Every FFS file carries a State byte at header offset 23.  With this FV's erase
polarity of 1, an asserted bit is a 0.  Marking EFI_FILE_DELETED (bit 4) makes a
spec-conformant DXE core's FFS walk skip the file entirely.  Two properties make
this the right primitive:

  * The State byte is excluded from the FFS header checksum (the PI spec has the
    checksum computed with State treated as zero), so a one-byte edit needs no
    checksum fixup and cannot desynchronise the header.
  * The file keeps its original size, so the FFS chain's offsets are untouched
    and the decompressed inner FV stays byte-for-byte the same length.  Nothing
    downstream has to be re-laid-out.

We then additionally blank the deleted file's body to 0xFF.  The DXE core never
reads it (it skips by header size), and a long run of 0xFF compresses to almost
nothing — which is what buys the space to inject our own drivers into the same
LZMA container.

Usage
-----
    romsurgeon.py inspect --rom ROM
    romsurgeon.py apply   --rom ROM --profile P --efi-dir D [--out O] [--dry-run]

Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import argparse
import os
import struct
import sys
import uuid

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uefifv as P  # FV/FFS/LZMA primitives, validated on this ROM

# ---------------------------------------------------------------------------
# FFS state bits.  Erase polarity 1 → a bit is "asserted" when it reads 0.
# ---------------------------------------------------------------------------
FFS_STATE_HEADER_CONSTRUCTION = 0x01
FFS_STATE_HEADER_VALID        = 0x02
FFS_STATE_DATA_VALID          = 0x04
FFS_STATE_MARKED_FOR_UPDATE   = 0x08
FFS_STATE_DELETED             = 0x10

#
# The DXE apriori file: an ordered list of 16-byte GUIDs the DXE core dispatches
# before falling back to DEPEX evaluation.  On this ROM it holds 15 entries, four
# of which name AMI OOB modules we want gone (DxeIpmiBmcInitialize,
# DxeSelStatusCode, DxeFrb, FruToSmbios123ByPcd).  A conformant core skips an
# apriori GUID it cannot find, but relying on that is a bet on AMI's core being
# conformant, so we prune the list explicitly.
#
DXE_APRIORI_GUID = "fc510ee7-ffdc-11d4-bd41-0080c73c8881"

FFS_TYPE_NAMES = {
    0x01: "RAW", 0x02: "FREEFORM", 0x03: "SEC_CORE", 0x04: "PEI_CORE",
    0x05: "DXE_CORE", 0x06: "PEIM", 0x07: "DRIVER", 0x08: "COMBO_PEIM_DRIVER",
    0x09: "APPLICATION", 0x0A: "SMM", 0x0B: "FIRMWARE_VOLUME_IMAGE",
    0x0C: "COMBO_SMM_DXE", 0x0D: "SMM_CORE", 0xF0: "FFS_PAD",
}

SEC_UI = 0x15


def guid_to_le(text):
    """'9df02dfd-8cf7-...' → the 16 little-endian bytes as stored in an FFS header."""
    return uuid.UUID(text).bytes_le


def le_to_guid(raw):
    return str(uuid.UUID(bytes_le=bytes(raw)))


class InnerVolume:
    """
    The LZMA-compressed inner DXE firmware volume: the one the DXE core actually
    dispatches.  Decompress on construction, mutate in place, recompress on save.
    """

    def __init__(self, rom):
        self.rom = bytearray(rom)
        outer = self.rom[P.FV_OFFSET:P.FV_OFFSET + P.FV_SIZE]
        self.outer_hdr_len, self.outer_len = P.parse_fv(outer)
        self.outer = outer

        off, size = P.find_ffs_in_fv(outer, P.OUTER_FFS_GUID)
        if off is None:
            raise RuntimeError("LZMA container FFS 9e21fd93 not found in the dispatched FV")
        self.outer_ffs_off, self.outer_ffs_size = off, size
        outer_ffs = outer[off:off + size]

        # Locate the GUIDED (LZMA) section holding the inner FV.
        pos, self.gsec_off, self.gsec_dataoff, self.gsec_size = 24, None, None, None
        while pos + 4 <= size:
            seclen = P.u24(outer_ffs, pos)
            if outer_ffs[pos + 3] == P.SEC_GUID_DEFINED:
                self.gsec_dataoff = struct.unpack_from("<H", outer_ffs, pos + 20)[0]
                self.gsec_off, self.gsec_size = pos, seclen
                break
            pos += (seclen + 3) & ~3
        if self.gsec_off is None:
            raise RuntimeError("GUID-defined (LZMA) section not found in the container FFS")

        self.outer_ffs = outer_ffs
        self.compressed_orig = outer_ffs[self.gsec_off + self.gsec_dataoff:
                                         self.gsec_off + self.gsec_size]
        raw = P.lzma_decompress(bytes(self.compressed_orig))
        # The GUIDED payload is a 16-byte FV_IMAGE section header then the FV.
        self.prefix = bytearray(raw[:0x10])
        self.fv = bytearray(raw[0x10:])
        self.hdr_len, self.fv_len = P.parse_fv(self.fv)
        self.content_start = P.ffs_content_start(self.fv)

    # -- walking ----------------------------------------------------------

    def files(self):
        """
        Yield (offset, size, guid_text, ffs_type, state, ui_name) for each FFS
        file, walking the chain by header size the way a DXE core does.
        """
        off = self.content_start
        while off + 24 <= self.fv_len:
            if self.fv[off:off + 16] == b"\xFF" * 16:
                break
            size = P.u24(self.fv, off + 20)
            if size < 24 or off + size > self.fv_len or size == 0xFFFFFF:
                break
            yield (off, size, le_to_guid(self.fv[off:off + 16]),
                   self.fv[off + 18], self.fv[off + 23], self._ui_name(off, size))
            off = (off + size + 7) & ~7

    def _ui_name(self, off, size):
        """Extract the UI section string, which is how humans identify a module."""
        pos = off + 24
        end = off + size
        while pos + 4 <= end:
            seclen = P.u24(self.fv, pos)
            if seclen < 4:
                break
            if self.fv[pos + 3] == SEC_UI:
                try:
                    return self.fv[pos + 4:pos + seclen].decode("utf-16-le").rstrip("\0")
                except UnicodeDecodeError:
                    return None
            pos += (seclen + 3) & ~3
        return None

    def free_span(self):
        """(start, length) of the trailing erased region available for new files."""
        end = self.fv_len
        while end > self.content_start and self.fv[end - 1] == 0xFF:
            end -= 1
        start = (end + 7) & ~7
        return start, self.fv_len - start

    # -- mutation ---------------------------------------------------------

    def delete(self, offset, size, blank_body=True):
        """
        Mark an FFS file EFI_FILE_DELETED, and optionally blank its body so the
        LZMA container shrinks.  Returns nothing; raises if already deleted.
        """
        state = self.fv[offset + 23]
        if (state & FFS_STATE_DELETED) == 0:
            raise RuntimeError(f"file at 0x{offset:x} is already marked deleted")

        # Assert DELETED by clearing its bit (erase polarity 1).
        self.fv[offset + 23] = state & ~FFS_STATE_DELETED

        if blank_body:
            self.fv[offset + 24:offset + size] = b"\xFF" * (size - 24)

    def compact(self):
        """
        Physically remove every FFS file marked EFI_FILE_DELETED, shifting the
        survivors forward so the reclaimed bytes join the trailing free region.

        Marking a file deleted is enough to stop it dispatching, but it does not
        free space in the *decompressed* FV — the slot is still there. Injecting a
        new driver needs contiguous room at the tail, so the deleted slots have to
        actually go.

        Shifting is safe here only because no file in this volume sets
        FFS_ATTRIB_DATA_ALIGNMENT (verified: all 382 files have attributes 0).
        A file with an alignment requirement would need a pad file inserted ahead
        of it to preserve its offset, so this asserts rather than silently
        corrupting such a volume.

        Returns (files_dropped, bytes_reclaimed).
        """
        survivors = []
        dropped = 0
        reclaimed = 0

        for off, size, _guid, _ftype, state, name in self.files():
            if (self.fv[off + 19] & 0x38) != 0:
                raise RuntimeError(
                    f"{name or hex(off)} sets FFS_ATTRIB_DATA_ALIGNMENT "
                    f"({self.fv[off + 19]:#02x}); compaction would break its alignment")
            if (state & FFS_STATE_DELETED) == 0:
                dropped += 1
                reclaimed += (size + 7) & ~7
                continue
            survivors.append(bytes(self.fv[off:off + size]))

        cursor = self.content_start
        for blob in survivors:
            self.fv[cursor:cursor + len(blob)] = blob
            cursor += (len(blob) + 7) & ~7

        # Everything past the last survivor becomes erased flash. This is what
        # buys the room to inject, and it compresses to nearly nothing.
        self.fv[cursor:self.fv_len] = b"\xFF" * (self.fv_len - cursor)
        return dropped, reclaimed

    def apriori_entries(self):
        """
        Locate the DXE apriori file and return (guid_list_offset, [guid_text, ...]).

        The file is a FREEFORM whose single RAW section body is a packed array of
        little-endian GUIDs.
        """
        target = guid_to_le(DXE_APRIORI_GUID)
        for off, size, guid, ftype, state, _name in self.files():
            if self.fv[off:off + 16] != target:
                continue
            # Single RAW section: 4-byte section header, then the GUID array.
            body = off + 24
            seclen = P.u24(self.fv, body)
            start = body + 4
            count = (seclen - 4) // 16
            return start, [le_to_guid(self.fv[start + i * 16:start + (i + 1) * 16])
                           for i in range(count)]
        return None, []

    def prune_apriori(self, drop_guids):
        """
        Remove the named GUIDs from the DXE apriori list, compacting the survivors
        forward and zero-filling the tail.

        The file's size is deliberately left unchanged so the FFS chain does not
        shift. Trailing all-zero GUIDs are looked up, not found, and skipped — the
        same tolerance a conformant core already applies to a stale entry, but now
        we are not depending on it for the modules we actually deleted.
        """
        start, entries = self.apriori_entries()
        if start is None:
            return []

        drop = {g.lower() for g in drop_guids}
        keep = [g for g in entries if g.lower() not in drop]
        removed = [g for g in entries if g.lower() in drop]
        if not removed:
            return []

        for i, guid in enumerate(keep):
            self.fv[start + i * 16:start + (i + 1) * 16] = guid_to_le(guid)
        tail = start + len(keep) * 16
        self.fv[tail:start + len(entries) * 16] = b"\x00" * (16 * len(removed))
        return removed

    def add_driver(self, guid_text, pe32, ui_name):
        """
        Append a new DXE driver FFS (DEPEX=TRUE) at the start of the trailing
        free region, so it stays contiguous with the existing chain and the
        dispatcher's sequential walk reaches it.
        """
        sections  = P.make_section(bytes([0x06, 0x08]), P.SEC_DXE_DEPEX)   # TRUE; END
        sections += P.make_section(pe32, P.SEC_PE32)
        sections += P.make_section((ui_name + "\0").encode("utf-16-le"), SEC_UI)
        ffs = P.make_ffs(guid_to_le(guid_text), sections, P.FFS_TYPE_DRIVER)

        start, avail = self.free_span()
        if len(ffs) > avail:
            raise RuntimeError(
                f"{ui_name}: needs {len(ffs)} bytes, only {avail} free in the inner FV")
        self.fv[start:start + len(ffs)] = ffs
        return start, len(ffs)

    def save(self):
        """Recompress and splice back into the ROM. Returns the new ROM bytes."""
        P.update_fv_checksum(self.fv)

        raw = bytearray(self.prefix) + self.fv
        raw[0x0C:0x0F] = P.pack24(4 + len(self.fv))

        # Reuse the original stream's LZMA properties and vary only the preset, so
        # the 13-byte header stays byte-identical to what the firmware already
        # decompresses while the stronger search shrinks the payload.
        props = P.lzma_props_from_stream(self.compressed_orig)
        compressed = P.lzma_compress(raw, props)

        if bytes(compressed[:5]) != bytes(self.compressed_orig[:5]):
            raise RuntimeError(
                "recompressed LZMA properties differ from the original stream; "
                f"got {compressed[:5].hex()}, expected {bytes(self.compressed_orig[:5]).hex()}")

        ghdr = bytearray(self.outer_ffs[self.gsec_off:self.gsec_off + self.gsec_dataoff])
        ghdr[0:3] = P.pack24(self.gsec_dataoff + len(compressed))
        new_sections = self.outer_ffs[24:self.gsec_off] + bytes(ghdr) + compressed
        new_outer_ffs = P.make_ffs(P.OUTER_FFS_GUID, new_sections, ffs_type=0x0B)

        outer = P.replace_ffs_in_fv(bytearray(self.outer), self.outer_hdr_len,
                                   self.outer_len, self.outer_ffs_off,
                                   self.outer_ffs_size, new_outer_ffs)
        P.update_fv_checksum(outer)
        self.rom[P.FV_OFFSET:P.FV_OFFSET + P.FV_SIZE] = outer
        return bytes(self.rom), len(self.compressed_orig), len(compressed)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_inspect(args):
    vol = InnerVolume(open(args.rom, "rb").read())
    print(f"ROM            {args.rom}")
    print(f"dispatched FV  0x{P.FV_OFFSET:08x} + 0x{P.FV_SIZE:x}")
    print(f"inner FV       len=0x{vol.fv_len:x} content starts 0x{vol.content_start:x}")
    print(f"LZMA payload   {len(vol.compressed_orig)} bytes compressed\n")

    live = deleted = 0
    print(f"{'offset':>8} {'size':>8}  {'type':22} {'state':>5}  name / guid")
    for off, size, guid, ftype, state, name in vol.files():
        is_deleted = (state & FFS_STATE_DELETED) == 0
        live, deleted = (live, deleted + 1) if is_deleted else (live + 1, deleted)
        if args.deleted_only and not is_deleted:
            continue
        tag = "DELETED" if is_deleted else ""
        print(f"{off:8x} {size:8} {FFS_TYPE_NAMES.get(ftype, hex(ftype)):22} "
              f"{state:5x}  {name or guid} {tag}")

    start, avail = vol.free_span()
    print(f"\n{live} live files, {deleted} deleted")
    print(f"free span      0x{start:x}..0x{vol.fv_len:x} ({avail} bytes)")

    _, entries = vol.apriori_entries()
    print(f"\nDXE apriori    {len(entries)} entries (dispatched before DEPEX evaluation)")
    for i, guid in enumerate(entries):
        hit = by_guid_name(vol, guid)
        print(f"  {i:2d}  {guid}  {hit or ''}")


def by_guid_name(vol, guid):
    """Resolve an apriori GUID to its module's UI name, if that module is present."""
    for _off, _size, g, _t, _s, name in vol.files():
        if g.lower() == guid.lower():
            return name or "(no UI name)"
    return None


def cmd_apply(args):
    profile = yaml.safe_load(open(args.profile))
    rom_bytes = open(args.rom, "rb").read()
    vol = InnerVolume(rom_bytes)

    # Index the volume by both GUID and UI name so a profile can use either.
    by_guid, by_name = {}, {}
    for off, size, guid, ftype, state, name in vol.files():
        by_guid[guid.lower()] = (off, size, name, state)
        if name:
            by_name.setdefault(name.lower(), (off, size, guid, state))

    print(f"profile: {profile.get('name', args.profile)}")
    if profile.get("description"):
        print(f"         {profile['description']}")
    print()

    # -- strip ---------------------------------------------------------
    removed = kept = 0
    freed = 0
    stripped_guids = set()
    for group in profile.get("strip", []):
        gname = group.get("group", "(ungrouped)")
        print(f"strip [{gname}] — {group.get('why', '')}")
        for item in group.get("modules", []):
            guid = str(item.get("guid", "")).lower()
            name = str(item.get("name", ""))
            hit = by_guid.get(guid) or by_name.get(name.lower())
            if hit is None:
                print(f"  MISS  {name or guid}  (not present in this ROM)")
                kept += 1
                continue
            off, size = hit[0], hit[1]
            state = hit[3]
            if (state & FFS_STATE_DELETED) == 0:
                print(f"  skip  {name}  (already deleted)")
                continue
            if not args.dry_run:
                vol.delete(off, size)
            print(f"  del   {name:34} 0x{off:06x}  {size:7} bytes")
            removed += 1
            freed += size
            stripped_guids.add(le_to_guid(vol.fv[off:off + 16]).lower())

    # -- prune the apriori list ----------------------------------------
    # Must happen after strip so we know exactly what went away, and before
    # inject so a freshly added driver is never pruned by name collision.
    print()
    if stripped_guids:
        _, entries = vol.apriori_entries()
        doomed = [g for g in entries if g.lower() in stripped_guids]
        if doomed:
            print("prune DXE apriori — these entries name modules we just deleted:")
            for guid in doomed:
                print(f"  del   {by_guid_name(vol, guid) or guid}")
            if not args.dry_run:
                vol.prune_apriori(doomed)
        else:
            print("DXE apriori — no entries reference stripped modules")

    # -- compact -------------------------------------------------------
    # Turn the deleted slots into contiguous trailing free space, which is the
    # only place a new FFS can go.
    if not args.dry_run and removed:
        dropped, reclaimed = vol.compact()
        start, avail = vol.free_span()
        print(f"\ncompacted {dropped} file(s), reclaimed {reclaimed} bytes; "
              f"free span now 0x{start:x}..0x{vol.fv_len:x} ({avail} bytes)")
    elif removed:
        print(f"\nwould compact {removed} file(s), reclaiming ~{freed} bytes")

    # -- inject --------------------------------------------------------
    print()
    added = 0
    for item in profile.get("inject", []):
        efi = os.path.join(args.efi_dir, item["efi"])
        if not os.path.exists(efi):
            print(f"  MISS  {item['efi']}  (build it first: make drivers)")
            continue
        pe32 = open(efi, "rb").read()
        if pe32[:2] != b"MZ":
            print(f"  BAD   {item['efi']}  (not a PE image)")
            continue
        if args.dry_run:
            print(f"  add   {item['name']:34} {len(pe32):7} bytes (dry run)")
        else:
            off, total = vol.add_driver(item["guid"], pe32, item["name"])
            print(f"  add   {item['name']:34} 0x{off:06x}  {total:7} bytes")
        added += 1

    print(f"\n{removed} removed ({freed} bytes of FFS), {added} injected, {kept} not found")

    if args.dry_run:
        print("\ndry run — nothing written")
        return 0

    if not args.out:
        print("\nno --out given; nothing written", file=sys.stderr)
        return 2

    new_rom, old_comp, new_comp = vol.save()
    if len(new_rom) != len(rom_bytes):
        print(f"\nREFUSING to write: size changed {len(rom_bytes)} -> {len(new_rom)}",
              file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(new_rom)
    print(f"\nLZMA payload {old_comp} -> {new_comp} bytes "
          f"({new_comp - old_comp:+d})")
    print(f"wrote {len(new_rom)} bytes -> {args.out}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("inspect", help="list FFS files in the dispatched DXE volume")
    p.add_argument("--rom", required=True)
    p.add_argument("--deleted-only", action="store_true")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("apply", help="strip and inject per a profile")
    p.add_argument("--rom", required=True)
    p.add_argument("--profile", required=True)
    p.add_argument("--efi-dir", default=".")
    p.add_argument("--out")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=cmd_apply)

    args = ap.parse_args()
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
