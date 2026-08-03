#!/usr/bin/env python3
"""
probecheck.py — report what our injected DXE drivers actually did, by decoding
the records they left in AMI's NVAR store, read back out of the SPI flash.

Why not just grep for the variable name: AMI does not rewrite a variable in
place. The first SetVariable creates a *named* entry (attr bit 0x02 ASCII_NAME);
every later update appends a nameless DATA_ONLY entry (attr bit 0x08) carrying
only the new payload. Grepping for the name therefore finds the ORIGINAL value
and silently reports stale data — which is exactly the mistake this tool made on
2026-07-30, reporting a probe as "not re-run" when its fresh record was sitting
in a DATA_ONLY entry a few hundred bytes further along.

So instead of matching names, this walks the NVAR chain and matches our record
*magics*, attributing each record to a module by the CallerId it carries. Names
are resolved where available, but identity comes from the GUID.

    python3 probecheck.py --rom readback.bin
    python3 probecheck.py --rom readback.bin --store 0x1037000:0x20000
"""

import argparse
import re
import struct
import uuid

PROBE_MAGIC = b"BOOPROBE"        # OobProbeDxe's OOBPROBE_RECORD
TLM_MAGIC   = b"OOBTLM0\x00"     # OobTelemetryLib's OOB_TELEMETRY_RECORD

# Bank 2 is the live one for Vermeer; bank 1 at 0x37000 is the dead Zen1 copy.
DEFAULT_STORE = (0x1037000, 0x20000)

KNOWN = {
    # Dispatch ladder: six byte-identical copies of OobProbeDxe differing only in
    # the caller GUID patched into each. Used to find out whether the AMI DXE
    # core dispatches every file appended to FVMAIN_COMPACT or only some of them.
    **{f"aa000000-0000-4000-8000-{i:012d}": f"ladder slot {i}" for i in range(1, 7)},
    "7c4d1a92-6f38-4b05-9e2a-3d81c0b7f4e6": "OobProbeDxe",
    "5e8a3c07-2b14-4d69-a0f3-71cc94e2b8d5": "OobProbe2Dxe (position control)",
    "4a2b7c8d-1234-5678-abcd-ef0123456789": "SmbiosBmcPushDxe",
    "9b3c62d1-4e78-4a05-8f21-6c07ae5db934": "BiosCfgOobDxe",
}

PHASES = {
    1: "DISPATCHED (entry ran, variable services not yet up)",
    2: "VARWRITE (variable write services available)",
    3: "ENDOFDXE",
    4: "READYTOBOOT",
}

TLM_FLAGS = [
    (0x00000001, "ENTRY"),        (0x00000002, "KCS_OK"),
    (0x00000004, "KCS_FAIL"),     (0x00000008, "ENDOFDXE"),
    (0x00000010, "READYTOBOOT"),  (0x00000020, "TIMER"),
    (0x00000040, "DATA_FOUND"),   (0x00000080, "DATA_MISSING"),
    (0x00000100, "XFER_OK"),      (0x00000200, "XFER_FAIL"),
    (0x00000400, "XFER_SKIP"),    (0x00000800, "PROTO_READY"),
    (0x00001000, "TIMER_ARMED"),
    (0x00010000, "CAP_SENT"),     (0x00020000, "APPLIED"),
    (0x00040000, "RESET_ISSUED"), (0x00080000, "SCHEMA_SENT"),
    (0x00100000, "SNAPSHOT_SENT"),
]

NVAR_VALID      = 0x80
NVAR_ASCII_NAME = 0x02
NVAR_DATA_ONLY  = 0x08
NVAR_GUID_INLINE = 0x04


def parse_span(text):
    lo, rest = text.split(":")
    return int(lo, 0), int(rest, 0)


def walk_nvar(rom, base, length):
    """Yield (offset, size, attrs, name_or_None) for each NVAR entry."""
    off = base
    end = base + length
    # The store's first entry follows the FV header; find it rather than
    # hardcoding, so a different image layout does not silently yield nothing.
    m = re.search(b"NVAR", rom[base:end])
    if not m:
        return
    off = base + m.start()

    while off + 10 <= end:
        if rom[off:off + 4] != b"NVAR":
            break
        size = struct.unpack_from("<H", rom, off + 4)[0]
        if size < 10 or size == 0xFFFF or off + size > end:
            break
        attrs = rom[off + 9]
        name = None
        if not (attrs & NVAR_DATA_ONLY):
            p = off + 10
            p += 16 if (attrs & NVAR_GUID_INLINE) else 1
            nul = rom.find(b"\x00", p, off + size)
            if nul > 0:
                try:
                    name = rom[p:nul].decode("ascii")
                except UnicodeDecodeError:
                    name = None
        yield off, size, attrs, name
        off += size


def decode_probe(buf):
    if len(buf) < 40 or buf[:8] != PROBE_MAGIC:
        return None
    phase, writes = struct.unpack_from("<II", buf, 8)
    tsc = struct.unpack_from("<Q", buf, 16)[0]
    return {"kind": "probe", "phase": phase, "writes": writes, "tsc": tsc,
            "caller": str(uuid.UUID(bytes_le=bytes(buf[24:40])))}


def decode_tlm(buf):
    if len(buf) < 56 or buf[:8] != TLM_MAGIC:
        return None
    flags, writes, ticks, dlen, status = struct.unpack_from("<IIIII", buf, 8)
    cc = buf[28]
    tsc = struct.unpack_from("<Q", buf, 32)[0]
    return {"kind": "telemetry", "flags": flags, "writes": writes,
            "ticks": ticks, "datalen": dlen, "status": status, "cc": cc,
            "tsc": tsc, "caller": str(uuid.UUID(bytes_le=bytes(buf[40:56])))}


def find_record(blob):
    """Locate one of our records anywhere inside an NVAR entry's bytes."""
    for magic, dec in ((PROBE_MAGIC, decode_probe), (TLM_MAGIC, decode_tlm)):
        i = blob.find(magic)
        if i >= 0:
            rec = dec(blob[i:])
            if rec:
                return rec
    return None


def render(rec, indent="      "):
    if rec["kind"] == "probe":
        print(f"{indent}phase   {rec['phase']}  {PHASES.get(rec['phase'], '?')}")
        print(f"{indent}writes  {rec['writes']}")
    else:
        names = [n for bit, n in TLM_FLAGS if rec["flags"] & bit]
        unknown = rec["flags"] & ~sum(bit for bit, _ in TLM_FLAGS)
        print(f"{indent}flags   0x{rec['flags']:08X}  {' '.join(names) or '(none)'}"
              + (f"  +unknown 0x{unknown:X}" if unknown else ""))
        print(f"{indent}writes  {rec['writes']}   ticks {rec['ticks']}")
        print(f"{indent}datalen {rec['datalen']}  status 0x{rec['status']:08X}"
              f"  ipmi_cc 0x{rec['cc']:02X}")
    print(f"{indent}tsc     0x{rec['tsc']:X}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--store", default=None,
                    help="NVAR store BASE:LEN (default 0x1037000:0x20000)")
    ap.add_argument("--expect", action="append", default=[],
                    help="module name that MUST appear; exit 1 if it does not")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    base, length = parse_span(args.store) if args.store else DEFAULT_STORE

    print(f"image: {args.rom} ({len(rom):,} bytes)")
    print(f"NVAR store: 0x{base:X}+0x{length:X}\n")

    entries = list(walk_nvar(rom, base, length))
    found = {}     # caller -> list of (offset, attrs, name, record)
    for off, size, attrs, name in entries:
        if not (attrs & NVAR_VALID):
            continue
        rec = find_record(rom[off:off + size])
        if rec:
            found.setdefault(rec["caller"], []).append((off, attrs, name, rec))

    print(f"{len(entries)} NVAR entries walked; "
          f"{sum(len(v) for v in found.values())} carry our records\n")

    if not found:
        print("VERDICT: nothing of ours wrote a variable this boot.")

    for caller, items in sorted(found.items(), key=lambda kv: kv[1][0][0]):
        module = KNOWN.get(caller, "unknown module")
        # Newest = highest offset. The store is append-only, so chain position
        # is the only reliable ordering: TSC resets on every cold boot, so a
        # previous boot's record can easily carry the larger value (it did on
        # 2026-07-30 — 0x58E4… from the older boot beat 0x58D3… from the newer).
        newest = max(items, key=lambda it: it[0])
        names = {n for _, _, n, _ in items if n}
        print(f"== {module} ==  {caller}")
        print(f"   variable: {', '.join(names) if names else '(update-only entries)'}")
        for off, attrs, name, rec in items:
            kind = "named" if not (attrs & NVAR_DATA_ONLY) else "DATA_ONLY update"
            mark = "  <- newest" if (off, attrs, name, rec) == newest else ""
            print(f"   0x{off:X}  attr=0x{attrs:02X}  {kind}{mark}")
        render(newest[3])
        print()

    missing = [m for m in args.expect
               if m not in {KNOWN.get(c, "") for c in found}]
    if missing:
        print("MISSING (expected but wrote nothing): " + ", ".join(missing))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
