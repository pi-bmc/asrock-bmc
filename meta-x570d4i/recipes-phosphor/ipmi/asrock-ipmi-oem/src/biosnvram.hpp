// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// AMI NVAR store reader/writer for the X570D4I-2T host BIOS flash.
//
// Why this exists
// ---------------
// The BIOS pushes its knob registry to the BMC over KCS (NetFn 0x30 cmd 0xD5),
// but the CurrentVal in that XML is NOT the live configuration: every knob's
// CurrentVal equals its own default, the document is byte-identical on every
// POST, and two knobs that share a single Setup byte (CSM009/SLOTOPROM022, both
// offset 0x01BA) report different "current" values -- impossible for a real
// read. Measured against the flash, 194 of 263 knobs disagreed with it.
//
// Only the *schema* in that XML is trustworthy: varstoreIndex, offset, size and
// the option lists. The live values live in AMI NVAR variables ("Setup",
// "ServerSetup", "AMITSESetup", ...) inside the 32 MB BIOS SPI flash, which the
// BMC can read and write through the SPI mux while the host is powered off.
//
// NVAR entry layout (little endian)
//   0  'NVAR'
//   4  u16 size       total entry size including this header
//   6  u24 next       relative link, 0xFFFFFF = none
//   9  u8  attrs      bit0 RT, bit1 ASCII name, bit2 full GUID, bit3 data-only,
//                     bit4 ext header, bit5 hwerr, bit6 auth, bit7 VALID
//   .. GUID(16) if attrs&0x04 else 1-byte GUID index
//   .. name: NUL-terminated ASCII if attrs&0x02, else UTF-16LE (absent if
//      attrs&0x08)
//   .. data, running to the end of the entry
//
// A variable normally appears several times (older superseded copies, plus a
// mirrored store 16 MB further in). Writes must update every valid copy.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace asrock
{
namespace nvram
{

// One valid, named NVAR entry located in the flash image.
struct Variable
{
    std::string name;
    std::size_t headerOffset = 0; // file offset of the 'NVAR' signature
    std::size_t dataOffset = 0;   // file offset of the value bytes
    std::size_t dataLength = 0;
};

// Every valid named entry in the image, in file order. Invalid/deleted entries
// (VALID bit clear) and data-only entries are skipped.
std::vector<Variable> parseVariables(const std::vector<uint8_t>& image);

// All copies of a given variable name.
std::vector<Variable> copiesOf(const std::vector<Variable>& vars,
                               const std::string& name);

// Read a little-endian integer of `size` bytes at `offset` within `var`.
// Returns false if the field would run past the end of the variable.
bool readField(const std::vector<uint8_t>& image, const Variable& var,
               unsigned offset, unsigned size, uint64_t& out);

// Write a little-endian integer of `size` bytes at `offset` within `var`.
bool writeField(std::vector<uint8_t>& image, const Variable& var,
                unsigned offset, unsigned size, uint64_t value);

// Load/save a whole flash image.
bool loadImage(const std::string& path, std::vector<uint8_t>& out);
bool saveImage(const std::string& path, const std::vector<uint8_t>& image);

} // namespace nvram
} // namespace asrock
