// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.

#include "biosnvram.hpp"

#include <cstring>
#include <fstream>

namespace asrock
{
namespace nvram
{

namespace
{

// Guard rails for a plausible NVAR entry. The store is scanned by searching for
// the 'NVAR' signature, so a byte sequence inside variable *data* can alias the
// signature; these bounds reject the obvious impostors and anything that would
// index outside the image.
constexpr std::size_t minEntrySize = 0x10;
constexpr std::size_t maxEntrySize = 0x2000;

constexpr uint8_t attrAsciiName = 0x02;
constexpr uint8_t attrFullGuid = 0x04;
constexpr uint8_t attrDataOnly = 0x08;
constexpr uint8_t attrValid = 0x80;

} // namespace

std::vector<Variable> parseVariables(const std::vector<uint8_t>& image)
{
    std::vector<Variable> out;
    if (image.size() < minEntrySize)
    {
        return out;
    }

    for (std::size_t i = 0; i + minEntrySize <= image.size(); ++i)
    {
        if (std::memcmp(image.data() + i, "NVAR", 4) != 0)
        {
            continue;
        }

        const std::size_t size =
            static_cast<std::size_t>(image[i + 4]) |
            (static_cast<std::size_t>(image[i + 5]) << 8);
        if (size < minEntrySize || size > maxEntrySize ||
            i + size > image.size())
        {
            continue;
        }

        const uint8_t attrs = image[i + 9];
        if (!(attrs & attrValid) || (attrs & attrDataOnly))
        {
            continue;
        }

        std::size_t p = i + 10;
        p += (attrs & attrFullGuid) ? 16 : 1;
        if (p >= i + size)
        {
            continue;
        }

        std::string name;
        if (attrs & attrAsciiName)
        {
            while (p < i + size && image[p] != 0)
            {
                name.push_back(static_cast<char>(image[p]));
                ++p;
            }
            ++p; // NUL
        }
        else
        {
            // UTF-16LE; these names are ASCII in practice, so keep the low byte
            // and stop at the double NUL.
            while (p + 1 < i + size &&
                   !(image[p] == 0 && image[p + 1] == 0))
            {
                name.push_back(static_cast<char>(image[p]));
                p += 2;
            }
            p += 2;
        }

        if (name.empty() || p > i + size)
        {
            continue;
        }

        Variable v;
        v.name = std::move(name);
        v.headerOffset = i;
        v.dataOffset = p;
        v.dataLength = (i + size) - p;
        out.push_back(std::move(v));
    }

    return out;
}

std::vector<Variable> copiesOf(const std::vector<Variable>& vars,
                               const std::string& name)
{
    std::vector<Variable> out;
    for (const auto& v : vars)
    {
        if (v.name == name)
        {
            out.push_back(v);
        }
    }
    return out;
}

bool readField(const std::vector<uint8_t>& image, const Variable& var,
               unsigned offset, unsigned size, uint64_t& out)
{
    if (size == 0 || size > 8 ||
        static_cast<std::size_t>(offset) + size > var.dataLength ||
        var.dataOffset + offset + size > image.size())
    {
        return false;
    }

    uint64_t v = 0;
    for (unsigned b = 0; b < size; ++b)
    {
        v |= static_cast<uint64_t>(image[var.dataOffset + offset + b])
             << (8 * b);
    }
    out = v;
    return true;
}

bool writeField(std::vector<uint8_t>& image, const Variable& var,
                unsigned offset, unsigned size, uint64_t value)
{
    if (size == 0 || size > 8 ||
        static_cast<std::size_t>(offset) + size > var.dataLength ||
        var.dataOffset + offset + size > image.size())
    {
        return false;
    }

    for (unsigned b = 0; b < size; ++b)
    {
        image[var.dataOffset + offset + b] =
            static_cast<uint8_t>((value >> (8 * b)) & 0xFF);
    }
    return true;
}

bool loadImage(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        return false;
    }
    const std::streamsize len = f.tellg();
    if (len <= 0)
    {
        return false;
    }
    f.seekg(0);
    out.resize(static_cast<std::size_t>(len));
    return static_cast<bool>(
        f.read(reinterpret_cast<char*>(out.data()), len));
}

bool saveImage(const std::string& path, const std::vector<uint8_t>& image)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        return false;
    }
    f.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(f);
}

} // namespace nvram
} // namespace asrock
