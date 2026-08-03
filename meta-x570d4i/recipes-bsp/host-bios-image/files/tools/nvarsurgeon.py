#!/usr/bin/env python3
"""
nvarsurgeon — inspect and patch AMI NVAR variable entries in a host BIOS ROM.

Why this exists
---------------
Some Setup values are boot requirements on this board, not preferences, and a
DXE driver that fixes them at runtime is always one boot too late for anything
consumed earlier (PCI resource assignment, the network stack gate). Writing them
into the NVAR store makes them true from the reset vector, exactly as if the
user had set them in Setup and saved.

NVAR entry layout (AMI Aptio V)
-------------------------------
    +0x00  'NVAR'            signature
    +0x04  Size              uint16 LE, whole entry including this header
    +0x06  Next              uint24 LE, 0xFFFFFF = end of chain
    +0x09  Attributes        bitfield, see below
    +0x0A  ...               GuidIndex (1 byte) unless GUID_INLINE,
                             then the name, then the data

    Attributes: 0x01 RUNTIME, 0x02 ASCII_NAME, 0x04 GUID_INLINE,
                0x08 DATA_ONLY, 0x10 EXT_HEADER, 0x20 HW_ERROR_RECORD,
                0x40 AUTH_WRITE, 0x80 VALID

    Live entries carry 0x80 (VALID). A superseded entry has it cleared, so the
    store can contain several generations of the same variable and only the
    valid one counts.

    The GUID table grows DOWNWARD from the end of the store: slot N occupies
    store_end - 16*(N+1). GuidIndex in the header selects a slot.

DATA_ONLY entries
-----------------
    0x08 marks an entry that carries no name/GUID of its own -- it is an update
    to an earlier entry, linked by the Next chain. Those are what the firmware
    writes when a single Setup value changes, and they are why "find the entry
    whose name matches" is not sufficient on its own: the newest DATA_ONLY link
    in the chain holds the current value.

Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import argparse
import struct
import sys

NVAR_SIG = b"NVAR"

ATTR_RUNTIME = 0x01
ATTR_ASCII_NAME = 0x02
ATTR_GUID_INLINE = 0x04
ATTR_DATA_ONLY = 0x08
ATTR_EXT_HEADER = 0x10
ATTR_AUTH_WRITE = 0x40
ATTR_VALID = 0x80


def u24(buf, off):
    return buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16)


def guid_str(raw):
    d1, d2, d3 = struct.unpack_from("<IHH", raw, 0)
    tail = "".join(f"{b:02X}" for b in raw[8:16])
    return f"{d1:08X}-{d2:04X}-{d3:04X}-{tail[:4]}-{tail[4:]}"


class NvarStore:
    def __init__(self, rom, base, length):
        self.rom = bytearray(rom)
        self.base = base
        self.length = length
        self.end = base + length

    def guid_at(self, index):
        off = self.end - 16 * (index + 1)
        return guid_str(self.rom[off:off + 16])

    def first_entry_offset(self):
        """
        Locate the first NVAR record.

        The store does NOT begin with one: on this ROM the base is 0x1037000 but
        the first 'NVAR' signature is at 0x1037090, i.e. there is a store header
        in front of the chain. Rather than hardcode that 0x90, find the
        signature -- a wrong constant here yields silently zero entries, which
        reads exactly like "nothing to patch".
        """
        idx = self.rom.find(NVAR_SIG, self.base, self.end)
        return idx if idx >= 0 else None

    def entries(self):
        """Yield every NVAR entry in store order."""
        off = self.first_entry_offset()
        if off is None:
            return
        while off < self.end - 16:
            if self.rom[off:off + 4] != NVAR_SIG:
                break
            size = struct.unpack_from("<H", self.rom, off + 4)[0]
            if size == 0 or size == 0xFFFF or off + size > self.end:
                break
            attrs = self.rom[off + 9]
            cur = off + 10
            guid = None
            name = None
            if not (attrs & ATTR_DATA_ONLY):
                if attrs & ATTR_GUID_INLINE:
                    guid = guid_str(self.rom[cur:cur + 16])
                    cur += 16
                else:
                    guid = self.guid_at(self.rom[cur])
                    cur += 1
                if attrs & ATTR_ASCII_NAME:
                    nul = self.rom.index(b"\x00", cur)
                    name = self.rom[cur:nul].decode("ascii", "replace")
                    cur = nul + 1
                else:
                    # UTF-16LE, NUL-terminated
                    nul = cur
                    while self.rom[nul:nul + 2] != b"\x00\x00":
                        nul += 2
                    name = self.rom[cur:nul].decode("utf-16-le", "replace")
                    cur = nul + 2
            yield {
                "offset": off,
                "size": size,
                "next": u24(self.rom, off + 6),
                "attrs": attrs,
                "guid": guid,
                "name": name,
                "data_off": cur,
                "data_len": off + size - cur,
            }
            off += size

    def nested(self, container="StdDefaults"):
        """
        Yield the entries nested inside a container entry.

        A shipped ROM does not contain the individual Setup variables at all --
        the only top-level record is StdDefaults, and the firmware materialises
        Setup, PCI_COMMON, NetworkStackVar and the rest from it. So patching
        defaults is what makes a value true from the reset vector, and it is
        also the only thing that survives: reflashing the ROM wipes the live
        NVAR region back to exactly these defaults, which is why runtime-only
        fixes have to be re-applied after every BIOS flash.
        """
        outer = None
        for e in self.entries():
            if e["name"] == container and (e["attrs"] & ATTR_VALID):
                outer = e
                break
        if outer is None:
            return

        off = outer["data_off"]
        end = outer["offset"] + outer["size"]
        while off < end - 8:
            if self.rom[off:off + 4] != NVAR_SIG:
                off += 1
                continue
            size = struct.unpack_from("<H", self.rom, off + 4)[0]
            if size == 0 or off + size > end:
                break
            attrs = self.rom[off + 9]
            cur = off + 10
            name = None
            if not (attrs & ATTR_DATA_ONLY):
                cur += 16 if (attrs & ATTR_GUID_INLINE) else 1
                if attrs & ATTR_ASCII_NAME:
                    nul = self.rom.index(b"\x00", cur)
                    name = self.rom[cur:nul].decode("ascii", "replace")
                    cur = nul + 1
            yield {
                "offset": off,
                "size": size,
                "attrs": attrs,
                "name": name,
                "data_off": cur,
                "data_len": off + size - cur,
            }
            off += size

    def patch(self, name, expect_len, offset, value):
        """
        Patch one byte of a nested default.

        Matched on name AND payload length, not on GUID. That is deliberate:
        two nested entries here are both called "Setup" -- varstore 1 (511-byte
        payload, 308 knobs) and varstore 13 (7-byte payload, 2 knobs) -- and the
        GUID indices inside StdDefaults do not resolve against the store's GUID
        table (they read back as all-FF), so the GUID is not usable as a key at
        this level. The payload length separates them unambiguously, and an
        ambiguous match is refused rather than guessed.
        """
        hits = [e for e in self.nested()
                if e["name"] == name and (e["attrs"] & ATTR_VALID)
                and (expect_len is None or e["data_len"] == expect_len)]
        if not hits:
            return None, f"no nested default named {name!r} with len={expect_len}"
        if len(hits) > 1:
            lens = ", ".join(str(h["data_len"]) for h in hits)
            return None, (f"{name!r} is ambiguous ({len(hits)} matches, lengths "
                          f"{lens}) -- give an explicit length")
        e = hits[0]
        if offset >= e["data_len"]:
            return None, (f"{name} offset 0x{offset:x} is past its {e['data_len']}-byte "
                          f"payload -- refusing to write into the next entry")
        abs_off = e["data_off"] + offset
        old = self.rom[abs_off]
        self.rom[abs_off] = value
        return (abs_off, old), None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x1037000,
                    help="NVAR store base (default 0x1037000, bank 2 = the LIVE "
                         "bank for Vermeer; bank 1 at 0x37000 is the dead one)")
    ap.add_argument("--length", type=lambda s: int(s, 0), default=0x20000)
    ap.add_argument("--list", action="store_true", help="list entries and exit")
    ap.add_argument("--set", action="append", default=[], metavar="NAME:LEN:OFF:VAL",
                    help="patch one byte of a nested default. LEN is the entry's "
                         "expected payload length, used to disambiguate -- two "
                         "nested entries are both named 'Setup' (511 and 7 bytes) "
                         "and the nested GUID indices are not resolvable. "
                         "e.g. PCI_COMMON:8:3:0x01")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    store = NvarStore(rom, args.base, args.length)

    if args.list:
        n = 0
        for e in store.entries():
            if e["name"] is None:
                continue
            flags = "VALID" if e["attrs"] & ATTR_VALID else "dead "
            print(f"  0x{e['offset']:08x} {flags} attrs=0x{e['attrs']:02x} "
                  f"len={e['data_len']:5d}  {e['name']:24s} {{{e['guid']}}}")
            n += 1
        print(f"\n{n} named entries")
        return 0

    changed = False
    for spec in args.set:
        try:
            name, elen, off, val = spec.split(":")
            elen = int(elen, 0)
            off = int(off, 0)
            val = int(val, 0)
        except ValueError:
            print(f"bad --set {spec!r}, want NAME:LEN:OFF:VAL", file=sys.stderr)
            return 2
        res, err = store.patch(name, elen, off, val)
        if err:
            print(f"  FAIL  {name}[0x{off:x}] -- {err}", file=sys.stderr)
            return 1
        abs_off, old = res
        if old == val:
            print(f"  ok    {name}[0x{off:x}] already 0x{val:02x}")
        else:
            print(f"  set   {name}[0x{off:x}] 0x{old:02x} -> 0x{val:02x} "
                  f"(ROM 0x{abs_off:08x})")
            changed = True

    if not args.out:
        print("\nno --out given; nothing written", file=sys.stderr)
        return 2
    if len(store.rom) != len(rom):
        print("REFUSING to write: size changed", file=sys.stderr)
        return 1
    with open(args.out, "wb") as fh:
        fh.write(store.rom)
    print(f"\nwrote {args.out} ({'modified' if changed else 'no change needed'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
