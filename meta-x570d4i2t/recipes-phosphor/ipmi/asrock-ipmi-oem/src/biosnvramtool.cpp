// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// asrock-bios-nvram — manage BIOS attribute values against the host BIOS flash.
//
// The BIOS-pushed XML supplies a trustworthy *schema* (varstoreIndex, offset,
// size, option lists) but useless values: its CurrentVal is the knob's own
// default on every knob and every boot. The live values are in AMI NVAR
// variables inside the BIOS SPI flash, which the BMC owns while the host is
// powered off. This tool bridges the two:
//
//   dump  <flash.bin>              inspect resolved varstores + values
//   sync  <flash.bin>              publish real values into BaseBIOSTable
//   apply <flash.bin> <out.bin>    fold PendingAttributes into a flash image
//
// The wrapper script drives the SPI mux, dd and flashcp around these; this
// binary never touches the mux itself, so it is safe to run on a file.
//
// Varstore mapping is *self-validating* rather than hardcoded: for each
// varstore we score every candidate NVAR variable by how many of that
// varstore's knob bytes hold a value that is a declared option of that knob. A
// correct pairing scores ~100% (measured: varstore 1 -> "Setup" 293/293, and
// varstore 14 -> "ServerSetup" 29/29 with an exact 739-byte span match) while
// wrong pairings sit at 60-88%. Anything that fails the bar is left unresolved
// and its knobs are published read-only rather than with invented values.

#include "biosnvram.hpp"
#include "biosvarstore.hpp"
#include "biosxml.hpp"

#include <sdbusplus/bus.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace
{

constexpr const char* biosMgrService = "xyz.openbmc_project.BIOSConfigManager";
constexpr const char* biosMgrPath = "/xyz/openbmc_project/bios_config/manager";
constexpr const char* biosMgrIface = "xyz.openbmc_project.BIOSConfig.Manager";
constexpr const char* defaultXmlPath =
    "/var/lib/asrock-bios-config/Payload1.xml";

// a{s(sv)} — what Redfish writes when a user PATCHes /Bios/Settings.
using PendingAttributesType =
    std::map<std::string, std::tuple<std::string, ipmi::DbusVariant>>;

// A varstore we were able to tie to a specific NVRAM variable with confidence.
struct Resolved
{
    std::string varName;
    std::vector<asrock::nvram::Variable> copies; // all copies; writes hit all
    std::size_t scored = 0;
    std::size_t matched = 0;
};

// Confidence bar for accepting a varstore->variable pairing.
constexpr std::size_t minScoredKnobs = 5;
constexpr double minScore = 0.98;

// Value helpers are shared with the ipmid provider, which needs the identical
// number parsing and option spelling when it decodes a live varstore snapshot.
using asrock::varstore::formatValue;
using asrock::varstore::optionValues;
using asrock::varstore::parseNumber;

std::map<int, Resolved> resolveVarstores(
    const std::vector<bios::knob::knob>& knobs,
    const std::vector<asrock::nvram::Variable>& vars,
    const std::vector<uint8_t>& image, bool verbose)
{
    // Longest copy per variable name is the read candidate.
    std::map<std::string, asrock::nvram::Variable> candidates;
    for (const auto& v : vars)
    {
        auto it = candidates.find(v.name);
        if (it == candidates.end() || v.dataLength > it->second.dataLength)
        {
            candidates[v.name] = v;
        }
    }

    // Varstores actually referenced by knobs.
    std::map<int, std::vector<const bios::knob::knob*>> byVarstore;
    for (const auto& k : knobs)
    {
        if (k.varstoreIndex >= 0)
        {
            byVarstore[k.varstoreIndex].push_back(&k);
        }
    }

    std::map<int, Resolved> resolved;
    for (const auto& [vs, list] : byVarstore)
    {
        std::size_t span = 0;
        for (const auto* k : list)
        {
            span = std::max<std::size_t>(span, k->offset + k->size);
        }

        struct Candidate
        {
            std::string name;
            std::size_t length;
            std::size_t scored;
            std::size_t matched;
            double score;
        };
        std::vector<Candidate> ranked;

        for (const auto& [name, cand] : candidates)
        {
            if (cand.dataLength < span)
            {
                continue;
            }
            std::size_t scored = 0, matched = 0;
            for (const auto* k : list)
            {
                const auto opts = optionValues(*k);
                if (opts.empty())
                {
                    continue;
                }
                uint64_t value = 0;
                if (!asrock::nvram::readField(image, cand, k->offset, k->size,
                                              value))
                {
                    continue;
                }
                ++scored;
                if (std::find(opts.begin(), opts.end(), value) != opts.end())
                {
                    ++matched;
                }
            }
            if (scored < minScoredKnobs)
            {
                continue;
            }
            ranked.push_back(
                {name, cand.dataLength, scored, matched,
                 static_cast<double>(matched) / static_cast<double>(scored)});
        }

        // Best score first; ties broken by the tightest fit. A varstore's
        // backing variable is sized to hold exactly that varstore, so when
        // several candidates all validate (a short span validates against
        // almost any large variable by chance) the one whose length hugs the
        // span is the real one -- e.g. varstore 6 (span 48) validates 100%
        // against both AMITSESetup (65 B) and the much larger Setup (511 B).
        std::sort(ranked.begin(), ranked.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.score != b.score)
                      {
                          return a.score > b.score;
                      }
                      return a.length < b.length;
                  });

        if (ranked.empty() || ranked.front().score < minScore)
        {
            if (verbose)
            {
                std::printf(
                    "  varstore %-3d span %-5zu UNRESOLVED (best %s %.0f%%)\n",
                    vs, span, ranked.empty() ? "-" : ranked.front().name.c_str(),
                    ranked.empty() ? 0.0 : ranked.front().score * 100.0);
            }
            continue;
        }

        // Equal score *and* equal size is a genuine tie -- several 8-byte
        // variables can all satisfy an 8-byte varstore. Guessing there would
        // publish (or worse, write) values from an unrelated variable, so treat
        // it as unresolved and leave those knobs read-only.
        if (ranked.size() > 1 &&
            ranked[1].score == ranked[0].score &&
            ranked[1].length == ranked[0].length)
        {
            if (verbose)
            {
                std::printf("  varstore %-3d span %-5zu AMBIGUOUS (%s vs %s, "
                            "both %zu B at %.0f%%) - left unresolved\n",
                            vs, span, ranked[0].name.c_str(),
                            ranked[1].name.c_str(), ranked[0].length,
                            ranked[0].score * 100.0);
            }
            continue;
        }

        const std::string bestName = ranked.front().name;
        const double best = ranked.front().score;
        const std::size_t bestScored = ranked.front().scored;
        const std::size_t bestMatched = ranked.front().matched;

        Resolved r;
        r.varName = bestName;
        r.copies = asrock::nvram::copiesOf(vars, bestName);
        r.scored = bestScored;
        r.matched = bestMatched;
        if (verbose)
        {
            std::printf(
                "  varstore %-3d span %-5zu -> %-16s %zu/%zu valid (%.0f%%), "
                "%zu copies\n",
                vs, span, bestName.c_str(), bestMatched, bestScored,
                best * 100.0, r.copies.size());
        }
        resolved.emplace(vs, std::move(r));
    }

    return resolved;
}

sdbusplus::bus_t getBus()
{
    return sdbusplus::bus::new_default_system();
}

int cmdDumpOrSync(const std::string& flashPath, const std::string& xmlPath,
                  bool publish)
{
    std::vector<uint8_t> image;
    if (!asrock::nvram::loadImage(flashPath, image))
    {
        std::fprintf(stderr, "cannot read flash image %s\n", flashPath.c_str());
        return 1;
    }

    bios::Xml xml(xmlPath.c_str());
    if (!xml.doDepexCompute())
    {
        std::fprintf(stderr, "warning: depex compute reported errors\n");
    }
    auto& knobs = xml.getKnobList();
    const auto vars = asrock::nvram::parseVariables(image);
    std::printf("flash %zu bytes, %zu NVRAM variables, %zu knobs\n",
                image.size(), vars.size(), knobs.size());

    const auto resolved = resolveVarstores(knobs, vars, image, true);

    bios::BiosBaseTableType table;
    std::size_t live = 0, unresolved = 0;
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
        bool readOnly = true; // no verified location => not writable
        auto it = resolved.find(k.varstoreIndex);
        uint64_t value = 0;
        if (it != resolved.end() && !it->second.copies.empty() &&
            asrock::nvram::readField(image, it->second.copies.front(), k.offset,
                                     k.size, value))
        {
            current = formatValue(k, value);
            readOnly = false; // real location known => a write can reach it
        }

        // The XML repeats some knob names (the same Setup byte surfaced under
        // more than one knob, e.g. CSM009/SLOTOPROM022), so emplace silently
        // drops later duplicates -- 431 knobs yield 373 attributes. Count only
        // what actually made it into the table or the totals disagree.
        const bool inserted =
            table
                .emplace(k.nameStr,
                         bios::BiosBaseTableTypeEntry(
                             "xyz.openbmc_project.BIOSConfig.Manager."
                             "AttributeType.Enumeration",
                             readOnly, k.promptStr, k.descriptionStr, "./",
                             current, k.defaultStr, options))
                .second;
        if (inserted)
        {
            readOnly ? ++unresolved : ++live;
        }
    }

    std::printf("attributes: %zu total, %zu from flash, %zu unresolved\n",
                table.size(), live, unresolved);

    if (!publish)
    {
        return 0;
    }
    if (live == 0)
    {
        std::fprintf(stderr,
                     "refusing to publish: no attribute resolved to flash\n");
        return 1;
    }

    try
    {
        auto bus = getBus();
        auto m = bus.new_method_call(biosMgrService, biosMgrPath,
                                     "org.freedesktop.DBus.Properties", "Set");
        m.append(std::string(biosMgrIface), std::string("BaseBIOSTable"),
                 std::variant<bios::BiosBaseTableType>(table));
        bus.call(m);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "failed to publish BaseBIOSTable: %s\n", e.what());
        return 1;
    }

    std::printf("published %zu attributes to BaseBIOSTable\n", table.size());
    return 0;
}

int cmdApply(const std::string& flashPath, const std::string& outPath,
             const std::string& xmlPath)
{
    std::vector<uint8_t> image;
    if (!asrock::nvram::loadImage(flashPath, image))
    {
        std::fprintf(stderr, "cannot read flash image %s\n", flashPath.c_str());
        return 1;
    }

    PendingAttributesType pending;
    try
    {
        auto bus = getBus();
        auto m = bus.new_method_call(biosMgrService, biosMgrPath,
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append(std::string(biosMgrIface), std::string("PendingAttributes"));
        auto reply = bus.call(m);
        std::variant<PendingAttributesType> v;
        reply.read(v);
        pending = std::get<PendingAttributesType>(v);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "cannot read PendingAttributes: %s\n", e.what());
        return 1;
    }

    if (pending.empty())
    {
        std::printf("no pending attributes; nothing to write\n");
        return 2; // distinct: caller skips the flash cycle entirely
    }

    bios::Xml xml(xmlPath.c_str());
    xml.doDepexCompute();
    auto& knobs = xml.getKnobList();
    const auto vars = asrock::nvram::parseVariables(image);
    const auto resolved = resolveVarstores(knobs, vars, image, false);

    std::size_t applied = 0, skipped = 0;
    for (const auto& [name, entry] : pending)
    {
        const auto* knob = static_cast<const bios::knob::knob*>(nullptr);
        for (const auto& k : knobs)
        {
            if (k.nameStr == name)
            {
                knob = &k;
                break;
            }
        }
        if (!knob)
        {
            std::fprintf(stderr, "  %-14s unknown attribute, skipped\n",
                         name.c_str());
            ++skipped;
            continue;
        }

        auto it = resolved.find(knob->varstoreIndex);
        if (it == resolved.end())
        {
            std::fprintf(stderr,
                         "  %-14s varstore %d unresolved, skipped\n",
                         name.c_str(), knob->varstoreIndex);
            ++skipped;
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
            std::fprintf(stderr, "  %-14s value %s not numeric, skipped\n",
                         name.c_str(), valueStr.c_str());
            ++skipped;
            continue;
        }

        const auto opts = optionValues(*knob);
        if (!opts.empty() &&
            std::find(opts.begin(), opts.end(), value) == opts.end())
        {
            std::fprintf(stderr,
                         "  %-14s value %s is not a declared option, skipped\n",
                         name.c_str(), valueStr.c_str());
            ++skipped;
            continue;
        }

        // Every copy of the variable must agree, or the BIOS may pick a stale
        // one.
        bool ok = true;
        for (const auto& copy : it->second.copies)
        {
            if (copy.dataLength < knob->offset + knob->size)
            {
                continue;
            }
            ok &= asrock::nvram::writeField(image, copy, knob->offset,
                                            knob->size, value);
        }
        if (!ok)
        {
            std::fprintf(stderr, "  %-14s write failed, skipped\n",
                         name.c_str());
            ++skipped;
            continue;
        }

        std::printf("  %-14s %s <- %s (varstore %d off 0x%04X size %u)\n",
                    name.c_str(), it->second.varName.c_str(), valueStr.c_str(),
                    knob->varstoreIndex, knob->offset, knob->size);
        ++applied;
    }

    if (applied == 0)
    {
        std::fprintf(stderr, "no attribute could be applied\n");
        return 1;
    }

    if (!asrock::nvram::saveImage(outPath, image))
    {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }

    std::printf("applied %zu attribute(s), skipped %zu -> %s\n", applied,
                skipped, outPath.c_str());
    return 0;
}

void usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  asrock-bios-nvram dump  <flash.bin> [xml]\n"
                 "  asrock-bios-nvram sync  <flash.bin> [xml]\n"
                 "  asrock-bios-nvram apply <flash.bin> <out.bin> [xml]\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        usage();
        return 1;
    }
    const std::string cmd = argv[1];

    if (cmd == "dump" || cmd == "sync")
    {
        const std::string xmlPath = (argc > 3) ? argv[3] : defaultXmlPath;
        return cmdDumpOrSync(argv[2], xmlPath, cmd == "sync");
    }
    if (cmd == "apply")
    {
        if (argc < 4)
        {
            usage();
            return 1;
        }
        const std::string xmlPath = (argc > 4) ? argv[4] : defaultXmlPath;
        return cmdApply(argv[2], argv[3], xmlPath);
    }

    usage();
    return 1;
}
