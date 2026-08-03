#!/usr/bin/env python3
"""
uefifv.py — UEFI firmware-volume / FFS / LZMA primitives for the X570D4I-2T ROM.

Extracted from edk2-x570d4i2t/scripts/patch_rom.py so romsurgeon.py is
self-contained inside this Yocto layer. Two deliberate differences from that
original are called out at their definitions:

  * lzma_compress() takes the properties from the stream being replaced instead
    of hardcoding a 1 MiB dictionary. The real stream uses 16 MiB, and the
    dictionary size in the header is what the firmware's GUIDED-section
    decompressor uses to size its DxeIpl-time scratch buffer.
  * The FFS header checksum excludes both the State byte and the File checksum
    field, per the PI spec — getting this wrong makes AMI's afulnx reject the
    image with "FFS Checksums ... Fail".

SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import lzma
import struct

# ---------------------------------------------------------------------------
# The dispatched DXE firmware volume.
#
# The 32 MiB image holds TWO compressed DXE volumes. Bank 1 at 0x069F000 is
# Zen1/Zen+ silicon support and is NOT dispatched on a Vermeer part; bank 2 at
# 0x1AC1000 is. Confirmed independently from the AMD Embedded Firmware Structure:
# only bank 2's EFS populates the Fam17h Model 30-3Fh/70-7Fh BIOS directory.
# ---------------------------------------------------------------------------
FV_OFFSET = 0x01AC_1000
FV_SIZE   = 0x003E_6000

# 9E21FD93-9C72-4C15-8C4B-E77F1DB2D792 — the LZMA container FFS inside that FV.
OUTER_FFS_GUID = bytes([
    0x93, 0xFD, 0x21, 0x9E, 0x72, 0x9C, 0x15, 0x4C,
    0x8C, 0x4B, 0xE7, 0x7F, 0x1D, 0xB2, 0xD7, 0x92,
])

# EFI section types
SEC_GUID_DEFINED = 0x02
SEC_PE32         = 0x10
SEC_DXE_DEPEX    = 0x13
SEC_VERSION      = 0x14
SEC_UI           = 0x15
SEC_RAW          = 0x19

# EFI FFS file types
FFS_TYPE_DRIVER = 0x07
FFS_TYPE_FV_IMAGE = 0x0B
FFS_TYPE_PAD    = 0xF0


def u24(buf, off):
    """Read a 3-byte little-endian integer."""
    return buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16)


def pack24(n):
    return bytes([n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF])


def ffs_header_checksum(header_24):
    """
    Compute and write the FFS header checksum in place.

    Per the PI spec the Header checksum is computed with the State field (byte 23)
    AND the IntegrityCheck.File field (byte 17) treated as ZERO. Including the
    real State byte (0xF8) leaves the checksum off by 0xF8, which AMI's afulnx
    rejects outright.
    """
    b = bytearray(header_24)
    b[16] = 0   # the header checksum field itself
    b[17] = 0   # File checksum, assumed zero
    b[23] = 0   # State, assumed zero
    header_24[16] = (-sum(b)) & 0xFF
    # IntegrityCheck.File = 0xAA when FFS_ATTRIB_CHECKSUM is clear (it is).
    header_24[17] = 0xAA
    return header_24


def make_section(data, sec_type):
    """Build an EFI section (4-byte header + data), padded to 4-byte alignment."""
    hdr = pack24(len(data) + 4) + bytes([sec_type])
    raw = hdr + data
    return raw + b"\x00" * ((4 - len(raw) % 4) % 4)


def make_ffs(guid, sections, ffs_type=FFS_TYPE_DRIVER, attributes=0x00):
    """Build a complete FFS file, padded to 8-byte alignment with erased flash."""
    size = 24 + len(sections)
    hdr = bytearray(24)
    hdr[:16]    = guid
    hdr[18]     = ffs_type
    hdr[19]     = attributes
    hdr[20:23]  = pack24(size)
    hdr[23]     = 0xF8   # HEADER_CONSTRUCTION | HEADER_VALID | DATA_VALID
    ffs_header_checksum(hdr)

    raw = bytes(hdr) + sections
    return raw + b"\xFF" * ((8 - len(raw) % 8) % 8)


def make_pad_file(total_size):
    """
    Build an EFI_FV_FILETYPE_FFS_PAD file of exactly total_size bytes (>= 24).

    A raw 0xFF gap looks like free space to the DXE core's sequential FFS walk,
    which truncates the volume — every later module vanishes. A pad file keeps
    the chain intact.
    """
    hdr = bytearray(24)
    # Pad-file name GUID is all zeros by EDK2 convention; a 0xFF first byte would
    # be read as free space.
    hdr[18]    = FFS_TYPE_PAD
    hdr[20:23] = pack24(total_size)
    hdr[23]    = 0xF8
    ffs_header_checksum(hdr)
    return bytes(hdr) + b"\xFF" * (total_size - 24)


def parse_fv(fv):
    """Return (hdr_len, fv_len) from an FV header."""
    if fv[0x28:0x2C] != b"_FVH":
        raise RuntimeError("not a firmware volume (no _FVH signature)")
    fv_len  = struct.unpack_from("<Q", fv, 0x20)[0]
    hdr_len = struct.unpack_from("<H", fv, 0x30)[0]
    return hdr_len, fv_len


def ffs_content_start(fv):
    """
    Offset within fv where FFS content begins.

    Accounts for the optional EFI_FIRMWARE_VOLUME_EXT_HEADER: when ExtHeaderOffset
    is non-zero, content starts after the ext header, 8-byte aligned.
    """
    hdr_len, _ = parse_fv(fv)
    ext_off = struct.unpack_from("<H", fv, 0x34)[0]
    if ext_off == 0:
        return hdr_len
    ext_size = struct.unpack_from("<I", fv, ext_off + 16)[0]
    return (ext_off + ext_size + 7) & ~7


def find_ffs_in_fv(fv, guid):
    """
    Scan an FV for an FFS file with a matching GUID.

    Searches for the GUID directly rather than walking the chain, so it still
    works when non-FFS AMI structures sit between files. Returns (offset, size)
    or (None, None).
    """
    _, fv_len = parse_fv(fv)
    idx = 0
    while True:
        idx = fv.find(guid, idx)
        if idx == -1 or idx + 24 > fv_len:
            return None, None
        if idx % 8 != 0:            # FFS files are 8-byte aligned
            idx += 1
            continue
        size = u24(fv, idx + 20)
        state = fv[idx + 23]
        if 24 <= size <= fv_len and state in (0xF8, 0xFC, 0xFE, 0x07):
            return idx, size
        idx += 1


def find_free_start(fv, fv_hdr_len, fv_len):
    """Walk FFS files and return the offset of the first free (0xFF) byte."""
    off = fv_hdr_len
    while off + 24 <= fv_len:
        if fv[off] == 0xFF:
            return off
        size = u24(fv, off + 20)
        if size == 0 or size == 0xFFFFFF:
            return off
        off = (off + size + 7) & ~7
    return off


def replace_ffs_in_fv(fv, fv_hdr_len, fv_len, old_offset, old_ffs_len, new_ffs):
    """
    Replace an FFS file in an FV, keeping the volume the same total length.

    Shrinking fills the freed bytes with a PAD file, not raw 0xFF. Growing shifts
    the tail down and consumes trailing erased flash.
    """
    old_aligned = (old_ffs_len + 7) & ~7
    new_aligned = len(new_ffs)          # make_ffs already 8-aligns

    free_start = find_free_start(fv, fv_hdr_len, fv_len)
    delta = new_aligned - old_aligned
    if delta > fv_len - free_start:
        raise ValueError(
            f"not enough FV free space: need {delta} more bytes, "
            f"have {fv_len - free_start}")

    before = fv[:old_offset]
    after  = fv[old_offset + old_aligned:]

    if delta <= 0:
        gap_size = -delta
        if gap_size == 0:
            gap = b""
        elif gap_size >= 24:
            gap = make_pad_file(gap_size)
        else:
            gap = b"\xFF" * gap_size    # too small for a header; harmless tail
        return bytearray(before + new_ffs + gap + after)

    combined = before + new_ffs + after
    if combined[fv_len:].strip(b"\xFF"):
        raise ValueError("not enough trailing free space to grow the FFS")
    return bytearray(combined[:fv_len])


def update_fv_checksum(fv):
    """Recalculate and write the FV header's 16-bit checksum (EFI_FVH_CHECKSUM)."""
    hdr_len = struct.unpack_from("<H", fv, 0x30)[0]
    fv[0x32] = 0
    fv[0x33] = 0
    total = sum(struct.unpack_from("<H", fv, i)[0] for i in range(0, hdr_len, 2))
    struct.pack_into("<H", fv, 0x32, (0x10000 - (total & 0xFFFF)) & 0xFFFF)


def lzma_decompress(data):
    return lzma.decompress(bytes(data), format=lzma.FORMAT_ALONE)


def lzma_props_from_stream(data):
    """
    Recover the LZMA1 properties from an existing ALONE-format stream header.

    Reproducing these exactly matters: the GUIDED-section decompressor sizes its
    scratch buffer from the dictionary size recorded here, and that allocation
    happens in DxeIpl before the full DXE allocator exists. This ROM's stream uses
    lc=3 lp=0 pb=2 with a 16 MiB dictionary.
    """
    prop = data[0]
    return {
        "id": lzma.FILTER_LZMA1,
        "lc": prop % 9,
        "lp": (prop // 9) % 5,
        "pb": (prop // 9) // 5,
        "dict_size": struct.unpack_from("<I", bytes(data), 1)[0],
    }


def lzma_compress(data, props, preset=9 | lzma.PRESET_EXTREME):
    """
    Compress in EDK2's ALONE (raw LZMA1) format using the given properties.

    Only the preset varies; the properties come from the stream being replaced so
    the 13-byte header stays byte-identical to what the firmware already handles.

    THE SIZE FIELD IS LOAD-BEARING. Python's lzma is a streaming compressor and
    writes 0xFFFFFFFFFFFFFFFF ("unknown") into the ALONE header's 8-byte
    uncompressed-length field. EDK2's LzmaUefiDecompressGetInfo() reads exactly
    that field to size the buffer it decompresses into, so leaving it unknown
    makes DXE IPL fail to expand the DXE firmware volume — the board hangs at
    POST 0x4F (DXE IPL started) after a perfectly normal PEI phase, which looks
    like anything but a compression bug.

    Confirmed on hardware 2026-07-29: a no-op repack that changed only the LZMA
    encoding hung at 0x4F; writing the real length here is what made it boot.
    """
    filters = dict(props)
    filters["preset"] = preset
    out = bytearray(lzma.compress(bytes(data), format=lzma.FORMAT_ALONE,
                                  filters=[filters]))
    struct.pack_into("<Q", out, 5, len(data))
    return bytes(out)
