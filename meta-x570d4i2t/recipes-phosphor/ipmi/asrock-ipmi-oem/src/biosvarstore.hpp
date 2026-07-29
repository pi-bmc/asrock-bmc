// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Live BIOS varstore snapshots, and the settings blob sent back the other way.
//
// Background
// ----------
// The knob schema the host pushes (SetPayload type 1, an Aptio-style XML) is
// trustworthy about *where* each knob lives -- varstoreIndex, offset, size and
// the option list -- but not about its value: CurrentVal is the knob's own
// default on every knob, because the document is a static defaults template
// compiled into the firmware, not a reading of anything.
//
// BiosCfgOobDxe (recipes-bsp/host-bios-image) closes that gap from the host
// side. Once per boot it reads the backing UEFI variables with
// gRT->GetVariable and pushes their raw bytes as SetPayload type 2, and it
// pulls staged Redfish changes back as GetPayload type 3 and writes them with
// gRT->SetVariable. This header is the BMC's half of those two formats.
//
// Why this beats reading the SPI flash
// ------------------------------------
// The flash path (asrock-bios-nvram) can only run while the host is powered
// off, has to steer the SPI mux, and cannot always tell which variable backs a
// varstore: varstores 1 and 13 are BOTH named "Setup" and differ only by
// vendor GUID, which the AMI NVAR store does not let a name lookup resolve.
// The firmware indexes varstores directly, so a snapshot has no such ambiguity.
// The flash path remains useful when the host never boots.
//
// Wire formats (little-endian, produced by BiosCfgOobDxe)
//
//   type 2  "ASVS" ver(2) count(2) reserved(4)
//           per record, 4-byte aligned:
//             varIndex(1) nameLen(1) dataLen(2) attributes(4) guid(16)
//             name[nameLen] data[dataLen]
//
//   type 3  "ASPS" ver(2) count(2) generation(4)
//           per record, fixed 12 bytes:
//             varIndex(1) size(1) offset(2) value(8)

#pragma once

#include "biosconfigcommands.hpp"
#include "biosxml.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace asrock
{
namespace varstore
{

// One varstore as the firmware read it.
struct Record
{
    int index = -1;
    std::string name;
    std::array<uint8_t, 16> guid{};
    uint32_t attributes = 0;
    std::vector<uint8_t> data;
};

struct Snapshot
{
    std::vector<Record> records;

    const Record* find(int index) const;
};

// Decode a type-2 payload. Returns false on a bad magic/version or any record
// that would run past the end of the buffer.
bool parseSnapshot(const std::vector<uint8_t>& blob, Snapshot& out);

// Read `size` little-endian bytes at `offset` from a record.
bool readField(const Record& rec, unsigned offset, unsigned size,
               uint64_t& out);

// Parse a knob option value ("0x07", "3"). Base is taken from the "0x" prefix
// rather than passing 0 to strtoull, which would read a zero-padded decimal as
// octal.
bool parseNumber(const std::string& s, uint64_t& out);

// A knob's declared option values, as integers.
std::vector<uint64_t> optionValues(const bios::knob::knob& k);

// Render a value using the exact spelling of the matching declared option, so
// Redfish sees a currentValue that is a member of the enumeration.
std::string formatValue(const bios::knob::knob& k, uint64_t value);

// Build BaseBIOSTable from the schema plus a snapshot. Knobs whose varstore is
// absent from the snapshot keep the schema's value and stay read-only -- there
// is no verified location to write, so offering them as settable would be a
// lie. `live` receives the number of attributes backed by real bytes.
std::size_t buildTable(const std::vector<bios::knob::knob>& knobs,
                       const Snapshot& snap, bios::BiosBaseTableType& table,
                       std::size_t& live);

// Encode PendingAttributes as a type-3 blob for the host to apply.
//
// An attribute is dropped unless it is a knob we know, its varstore is present
// in `snap`, its value parses, and -- when the knob declares options -- the
// value is one of them. Requiring the varstore keeps this in step with
// buildTable(): exactly the attributes published as writable are the ones that
// can be staged, so a write is never sent for a variable the firmware could not
// even read. The firmware writes these bytes straight into a varstore, so
// anything not positively understood here must not be sent.
//
// Returns the number of records encoded.
std::size_t buildPendingBlob(const std::vector<bios::knob::knob>& knobs,
                             const Snapshot& snap,
                             const biosconfig::PendingAttributes& pending,
                             uint32_t generation, std::vector<uint8_t>& blob);

} // namespace varstore
} // namespace asrock
