// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// See biosvarstore.hpp for the wire formats and the rationale.

#include "biosvarstore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace asrock
{
namespace varstore
{

namespace
{

constexpr std::size_t snapshotHeaderSize = 12; // magic(4) ver(2) count(2) rsvd(4)
constexpr std::size_t recordHeaderSize = 24;   // idx nameLen dataLen attrs guid
constexpr std::size_t pendingHeaderSize = 12;  // magic(4) ver(2) count(2) gen(4)
constexpr std::size_t pendingRecordSize = 12;  // idx size offset value(8)

// A malformed length field must never be able to allocate or read wildly; the
// firmware caps its own snapshot at 8 KiB.
constexpr std::size_t maxSnapshotRecords = 64;
constexpr std::size_t maxRecordData = 4096;

uint16_t rd16(const std::vector<uint8_t>& b, std::size_t off)
{
    return static_cast<uint16_t>(b[off]) |
           static_cast<uint16_t>(static_cast<uint16_t>(b[off + 1]) << 8);
}

uint32_t rd32(const std::vector<uint8_t>& b, std::size_t off)
{
    return static_cast<uint32_t>(b[off]) |
           (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

void wr16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void wr32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void wr64(std::vector<uint8_t>& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

} // namespace

const Record* Snapshot::find(int index) const
{
    for (const auto& r : records)
    {
        if (r.index == index)
        {
            return &r;
        }
    }
    return nullptr;
}

bool parseSnapshot(const std::vector<uint8_t>& blob, Snapshot& out)
{
    out.records.clear();

    if (blob.size() < snapshotHeaderSize || blob[0] != 'A' || blob[1] != 'S' ||
        blob[2] != 'V' || blob[3] != 'S')
    {
        return false;
    }
    if (rd16(blob, 4) != 1)
    {
        return false;
    }
    const std::size_t count = rd16(blob, 6);
    if (count == 0 || count > maxSnapshotRecords)
    {
        return false;
    }

    std::size_t off = snapshotHeaderSize;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (off + recordHeaderSize > blob.size())
        {
            return false;
        }
        Record r;
        r.index = blob[off];
        const std::size_t nameLen = blob[off + 1];
        const std::size_t dataLen = rd16(blob, off + 2);
        r.attributes = rd32(blob, off + 4);
        std::memcpy(r.guid.data(), blob.data() + off + 8, r.guid.size());

        if (dataLen == 0 || dataLen > maxRecordData)
        {
            return false;
        }
        const std::size_t body = recordHeaderSize + nameLen + dataLen;
        if (off + body > blob.size())
        {
            return false;
        }

        r.name.assign(reinterpret_cast<const char*>(blob.data() + off +
                                                    recordHeaderSize),
                      nameLen);
        r.data.assign(blob.begin() + static_cast<long>(off + recordHeaderSize +
                                                       nameLen),
                      blob.begin() +
                          static_cast<long>(off + recordHeaderSize + nameLen +
                                            dataLen));
        out.records.push_back(std::move(r));

        off += (body + 3) & ~static_cast<std::size_t>(3);
    }

    return !out.records.empty();
}

bool readField(const Record& rec, unsigned offset, unsigned size, uint64_t& out)
{
    if (size == 0 || size > 8 ||
        static_cast<std::size_t>(offset) + size > rec.data.size())
    {
        return false;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < size; ++i)
    {
        v |= static_cast<uint64_t>(rec.data[offset + i]) << (8 * i);
    }
    out = v;
    return true;
}

bool parseNumber(const std::string& s, uint64_t& out)
{
    if (s.empty())
    {
        return false;
    }
    const int base =
        (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, base);
    if (end == s.c_str() || (end && *end != '\0'))
    {
        return false;
    }
    out = v;
    return true;
}

std::vector<uint64_t> optionValues(const bios::knob::knob& k)
{
    std::vector<uint64_t> out;
    for (const auto& o : k.options)
    {
        uint64_t v = 0;
        if (parseNumber(o.value, v))
        {
            out.push_back(v);
        }
    }
    return out;
}

std::string formatValue(const bios::knob::knob& k, uint64_t value)
{
    for (const auto& o : k.options)
    {
        uint64_t v = 0;
        if (parseNumber(o.value, v) && v == value)
        {
            return o.value;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%02llX",
                  static_cast<unsigned long long>(value));
    return buf;
}

std::size_t buildTable(const std::vector<bios::knob::knob>& knobs,
                       const Snapshot& snap, bios::BiosBaseTableType& table,
                       std::size_t& live)
{
    live = 0;
    for (const auto& k : knobs)
    {
        if (!k.depex)
        {
            continue;
        }

        bios::OptionTypeVector options;
        for (const auto& o : k.options)
        {
            options.emplace_back(
                "xyz.openbmc_project.BIOSConfig.Manager.BoundType.OneOf",
                o.value, o.text);
        }

        std::string current = k.currentValStr;
        bool readOnly = true;
        uint64_t value = 0;
        const Record* rec = snap.find(k.varstoreIndex);
        if (rec != nullptr && readField(*rec, k.offset, k.size, value))
        {
            current = formatValue(k, value);
            // A verified location the firmware will write back through.
            readOnly = k.readOnly;
        }

        // The XML repeats some knob names (one Setup byte surfaced under more
        // than one knob, e.g. CSM009/SLOTOPROM022), so emplace silently drops
        // later duplicates -- 431 knobs yield 373 attributes. Count only what
        // actually landed or the totals disagree.
        const bool inserted =
            table
                .emplace(k.nameStr,
                         bios::BiosBaseTableTypeEntry(
                             "xyz.openbmc_project.BIOSConfig.Manager."
                             "AttributeType.Enumeration",
                             readOnly, k.promptStr, k.descriptionStr, "./",
                             current, k.defaultStr, options))
                .second;
        if (inserted && !readOnly)
        {
            ++live;
        }
    }
    return table.size();
}

std::size_t buildPendingBlob(const std::vector<bios::knob::knob>& knobs,
                             const Snapshot& snap,
                             const biosconfig::PendingAttributes& pending,
                             uint32_t generation, std::vector<uint8_t>& blob)
{
    std::vector<uint8_t> records;
    std::size_t count = 0;

    for (const auto& [name, entry] : pending)
    {
        const bios::knob::knob* knob = nullptr;
        for (const auto& k : knobs)
        {
            if (k.nameStr == name)
            {
                knob = &k;
                break;
            }
        }
        if (knob == nullptr || knob->varstoreIndex < 0 || knob->size == 0 ||
            knob->size > 8 || knob->offset > 0xFFFF)
        {
            continue;
        }
        // Only what buildTable() published as writable: no snapshot record
        // means the firmware never read that varstore, so it must not be told
        // to write into it.
        const Record* rec = snap.find(knob->varstoreIndex);
        if (rec == nullptr ||
            static_cast<std::size_t>(knob->offset) + knob->size >
                rec->data.size())
        {
            continue;
        }

        const auto& variant = std::get<1>(entry);
        std::string valueStr;
        if (std::holds_alternative<std::string>(variant))
        {
            valueStr = std::get<std::string>(variant);
        }
        else
        {
            valueStr = std::to_string(std::get<int64_t>(variant));
        }

        uint64_t value = 0;
        if (!parseNumber(valueStr, value))
        {
            continue;
        }
        const auto opts = optionValues(*knob);
        if (!opts.empty() &&
            std::find(opts.begin(), opts.end(), value) == opts.end())
        {
            continue;
        }

        records.push_back(static_cast<uint8_t>(knob->varstoreIndex));
        records.push_back(static_cast<uint8_t>(knob->size));
        wr16(records, static_cast<uint16_t>(knob->offset));
        wr64(records, value);
        ++count;
    }

    blob.clear();
    blob.push_back('A');
    blob.push_back('S');
    blob.push_back('P');
    blob.push_back('S');
    wr16(blob, 1);
    wr16(blob, static_cast<uint16_t>(count));
    wr32(blob, generation);
    blob.insert(blob.end(), records.begin(), records.end());

    static_assert(pendingHeaderSize == 12);
    static_assert(pendingRecordSize == 12);
    return count;
}

} // namespace varstore
} // namespace asrock
