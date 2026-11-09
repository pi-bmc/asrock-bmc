// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Adapted from openbmc/intel-ipmi-oem src/biosconfigcommands.cpp
// (Copyright 2020 Intel Corporation, Apache-2.0).
//
// ---------------------------------------------------------------------------
// What this is
// ---------------------------------------------------------------------------
// The OpenBMC "BIOS OOB config" IPMI command set, which lets the host BIOS push
// its full attribute registry to the BMC over the system interface (KCS) so the
// stock bmcweb Redfish endpoints
//
//     /redfish/v1/Systems/system/Bios
//     /redfish/v1/Systems/system/Bios/Settings
//
// are populated, and pull staged changes (PendingAttributes) back on the next
// boot. It is the in-band replacement for this board's dead USB Redfish Host
// Interface config path (see biosconfig-manager_%.bbappend / RHI-HANDOFF.md).
//
// ---------------------------------------------------------------------------
// What was adapted from the Intel original (and why)
// ---------------------------------------------------------------------------
//   * Intel transfers its proprietary BIOS Setup *XML* and parses it with
//     biosxml.hpp + a spawned converter (boost::process). AMI firmware never
//     emits Intel XML, so that whole chain is removed.
//   * The NetFn/cmd numbers stay on Intel's path (NetFn 0x30, cmds 0xD3-0xD8):
//     a live KCS capture shows the stock AMI BIOS driving exactly this protocol
//     (it sends SetBIOSCap 0xD3 on NetFn 0x30 during POST). The chunked transfer
//     state machine, packed struct layouts, and CRC32 integrity are kept
//     identical to Intel's — no oemcommands.hpp, no /var/oob.
//
// PAYLOAD FORMAT (confirmed by capture): payloadType 1, a ~13.7 KB body that is
// **LZMA-compressed** (.lzma "alone" format, props 0x5D) Intel-Aptio BIOS-config
// XML — ~184 KB of <SYSTEM><biosknobs><knob> once inflated (Intel server BIOS is
// AMI Aptio, so this board's AMI BIOS emits the same schema). This handler runs
// the full chunked transfer + CRC32, persists the raw payload to
// /var/lib/asrock-bios-config/PayloadN, LZMA-decompresses it (liblzma) to
// PayloadN.xml, then parses that into the BaseBIOSTable D-Bus property via
// biosxml.hpp (Intel's tinyxml2 Aptio parser: bios::Xml -> doDepexCompute ->
// getBaseTable -> commit), mirroring intel-ipmi-oem's ipmiOEMSetPayload. A
// decode/parse failure is logged but the transfer is still ACKed so the BIOS
// handshake completes.
//
// The kcsmonitor filter (same library) logs every inbound request over KCS.

#include "biosconfigcommands.hpp"

#include "biosvarstore.hpp"
#include "biosxml.hpp"

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/message/types.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sys/eventfd.h>
#include <unistd.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/crc.hpp>
#include <lzma.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace asrock
{
namespace biosconfig
{

using phosphor::logging::entry;
using phosphor::logging::level;
using phosphor::logging::log;
using Json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// In-memory state. ipmid runs a single-threaded asio event loop, so the
// handlers below are never re-entered concurrently — no locking needed. The
// one worker thread (the payload commit pipeline) touches none of this: it
// works on copies and hands its results back to the event loop.
// ---------------------------------------------------------------------------
struct PayloadMeta
{
    uint16_t version = 0; // Intel PayloadStartTransfer.payloadVersion is u16
    uint8_t type = 0;
    uint32_t totalSize = 0;
    uint32_t totalChecksum = 0;
    uint8_t flag = 0;
    uint8_t status = static_cast<uint8_t>(PayloadStatus::unknown);
    uint32_t timestamp = 0;
};

struct Session
{
    bool active = false;
    uint8_t type = 0;
    uint32_t reservationId = 0;
    uint32_t totalSize = 0;
    uint32_t totalChecksum = 0;
    uint16_t version = 0;
    std::vector<uint8_t> buffer;
};

static uint8_t g_capability = 0;
static bool g_capabilityInit = false;
static std::array<PayloadMeta, maxPayloadTypes> g_meta{};
static std::array<std::vector<uint8_t>, maxPayloadTypes> g_payload{};
static Session g_session{};
// CRC32, per payload type, of the last payload that was both parsed and
// committed to BaseBIOSTable. Persisted next to the capability byte so the
// identical document this BIOS re-pushes on every POST can be short-circuited.
// 0 means "nothing committed yet".
static std::array<uint32_t, maxPayloadTypes> g_committedChecksum{};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static uint32_t crc32(const uint8_t* data, size_t len)
{
    boost::crc_32_type result;
    result.process_bytes(data, len);
    return result.checksum();
}

static uint32_t rd32(const std::vector<uint8_t>& v, size_t off)
{
    return static_cast<uint32_t>(v[off]) |
           (static_cast<uint32_t>(v[off + 1]) << 8) |
           (static_cast<uint32_t>(v[off + 2]) << 16) |
           (static_cast<uint32_t>(v[off + 3]) << 24);
}

static void appendLE32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 24) & 0xFF);
}

static uint32_t nextReservationId()
{
    static uint32_t counter = 0;
    if (++counter == 0)
    {
        counter = 1; // reservation id 0 is reserved for "none"
    }
    return counter;
}

static void ensureStateDir()
{
    std::error_code ec;
    fs::create_directories(stateDir, ec);
}

// ---------------------------------------------------------------------------
// Capability persistence (replaces Intel's /var/oob/nvoobdata.dat).
// ---------------------------------------------------------------------------
static void loadCapability()
{
    if (g_capabilityInit)
    {
        return;
    }
    g_capabilityInit = true;
    std::ifstream f(std::string(stateDir) + "/nvdata.json");
    if (!f)
    {
        return;
    }
    try
    {
        Json j;
        f >> j;
        g_capability = j.value("capability", 0);
        auto it = j.find("committedChecksum");
        if (it != j.end() && it->is_array())
        {
            for (size_t i = 0; i < g_committedChecksum.size() && i < it->size();
                 ++i)
            {
                g_committedChecksum[i] = (*it)[i].get<uint32_t>();
            }
        }
    }
    catch (const std::exception&)
    {
        // fall back to defaults
    }

    // Republish what we already hold so GetPayload(Info) can answer it across a
    // BMC reboot. Without this the host's skip gate only works for the life of
    // one ipmid instance and every BMC restart costs a 13.7 KB KCS transfer
    // during the next POST. The size comes from the payload persisted by
    // recordPayload(); the checksum is the one we committed against it.
    for (size_t i = 0; i < maxPayloadTypes; ++i)
    {
        if (g_committedChecksum[i] == 0)
        {
            continue;
        }
        std::error_code ec;
        const auto path =
            std::string(stateDir) + "/Payload" + std::to_string(i);
        const auto size = fs::file_size(path, ec);
        if (ec || size == 0 || size > maxPayloadSize)
        {
            continue;
        }
        PayloadMeta& md = g_meta[i];
        md.version = 1;
        md.type = static_cast<uint8_t>(i);
        md.totalSize = static_cast<uint32_t>(size);
        md.totalChecksum = g_committedChecksum[i];
        md.status = static_cast<uint8_t>(PayloadStatus::valid);
    }
}

static void storeCapability()
{
    ensureStateDir();
    Json j;
    j["capability"] = g_capability;
    j["committedChecksum"] = g_committedChecksum;
    std::ofstream f(std::string(stateDir) + "/nvdata.json", std::ios::trunc);
    if (f)
    {
        f << j.dump();
    }
}

// ---------------------------------------------------------------------------
// Commit a parsed BIOS attribute table to the BIOSConfig.Manager BaseBIOSTable
// property. The table type comes straight from bios::Xml::getBaseTable(), so no
// conversion is needed — it already has the D-Bus shape a{s(sbsssvva(svs))}.
//
// Asynchronous on purpose: the property is a few hundred attributes that
// bios-settings-manager re-validates, and a blocking bus->call() here held
// ipmid's only thread for ~0.7 s while the host was mid-POST and waiting on
// KCS replies. `done` runs on the event loop with the outcome.
// ---------------------------------------------------------------------------
static void commitBaseBIOSTable(bios::BiosBaseTableType table,
                                const char* source,
                                std::function<void(bool)> done)
{
    const std::size_t count = table.size();
    ::getSdBus()->async_method_call(
        [count, source, done = std::move(done)](
            const boost::system::error_code& ec) {
            if (ec)
            {
                log<level::ERR>("biosconfig: failed to set BaseBIOSTable",
                                entry("ERR=%s", ec.message().c_str()));
                done(false);
                return;
            }
            log<level::INFO>("biosconfig: BaseBIOSTable populated",
                             entry("SOURCE=%s", source),
                             entry("COUNT=%zu", count));
            done(true);
        },
        biosMgrService, biosMgrPath, "org.freedesktop.DBus.Properties", "Set",
        std::string(biosMgrIface), std::string("BaseBIOSTable"),
        std::variant<bios::BiosBaseTableType>(std::move(table)));
}

// ---------------------------------------------------------------------------
// Attribute count currently published in BaseBIOSTable, or nullopt if it could
// not be read. Used only to decide whether an unchanged payload may be skipped;
// callers treat "unknown" as "re-publish", so a transient D-Bus failure costs a
// redundant parse instead of leaving Redfish with an empty table. Asynchronous
// for the same reason as the commit: the reply is the whole table.
// ---------------------------------------------------------------------------
static void baseBiosTableSize(std::function<void(std::optional<size_t>)> done)
{
    ::getSdBus()->async_method_call(
        [done = std::move(done)](
            const boost::system::error_code& ec,
            const std::variant<bios::BiosBaseTableType>& value) {
            if (ec)
            {
                done(std::nullopt);
                return;
            }
            done(std::get<bios::BiosBaseTableType>(value).size());
        },
        biosMgrService, biosMgrPath, "org.freedesktop.DBus.Properties", "Get",
        std::string(biosMgrIface), std::string("BaseBIOSTable"));
}

// ---------------------------------------------------------------------------
// Decompress a legacy LZMA "alone" (.lzma) stream. The AMI BIOS LZMA-compresses
// the BIOS-config XML before pushing it (StartTransfer body begins with the
// alone header: props byte 0x5D, 4-byte dict size, 8-byte streamed size). Uses
// liblzma's alone decoder with a bounded memlimit and output cap. Returns true
// and fills `out` with the decompressed bytes on success.
// ---------------------------------------------------------------------------
static bool lzmaAloneDecode(const std::vector<uint8_t>& in,
                            std::vector<uint8_t>& out)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    // 128 MiB memlimit bounds the dictionary a (possibly corrupt) header asks
    // for; the real dict here is 64 MiB.
    if (lzma_alone_decoder(&strm, 128ULL * 1024 * 1024) != LZMA_OK)
    {
        return false;
    }
    strm.next_in = in.data();
    strm.avail_in = in.size();

    std::array<uint8_t, 64 * 1024> chunk;
    lzma_ret ret;
    do
    {
        strm.next_out = chunk.data();
        strm.avail_out = chunk.size();
        ret = lzma_code(&strm, LZMA_FINISH);
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + (chunk.size() - strm.avail_out));
        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
        {
            lzma_end(&strm);
            return false;
        }
        if (out.size() > 16u * 1024 * 1024) // sane cap for BIOS Setup XML
        {
            lzma_end(&strm);
            return false;
        }
    } while (ret != LZMA_STREAM_END);

    lzma_end(&strm);
    return true;
}

// ---------------------------------------------------------------------------
// Turn a received schema payload into a BaseBIOSTable, mirroring intel-ipmi-oem
// ipmiOEMSetPayload. The AMI BIOS sends the Aptio XML LZMA-compressed, so we
// decompress first (payloads that already start with '<' are treated as plain
// XML), write the XML out for tinyxml2 (which reads a file path), then
// bios::Xml -> doDepexCompute -> getBaseTable. The knob list from the same
// parse is returned too, so the schema never has to be parsed a second time.
//
// Runs on the commit worker thread, so apart from writing xmlPath it touches
// no shared state. The XML is written to a temporary file and renamed into
// place, so a reader on the event loop never sees a half-written schema.
// ---------------------------------------------------------------------------
static bool parseBiosXml(const std::vector<uint8_t>& payload,
                         const std::string& xmlPath,
                         bios::BiosBaseTableType& table,
                         std::vector<bios::knob::knob>& knobs)
{
    std::vector<uint8_t> xml;
    if (!payload.empty() && payload[0] == '<')
    {
        xml = payload; // already plain XML
    }
    else if (!lzmaAloneDecode(payload, xml))
    {
        log<level::ERR>("biosconfig: SetPayload LZMA decompression failed");
        return false;
    }

    // tinyxml2 (bios::Xml) loads from a file; persist the decompressed XML.
    const std::string tmpPath = xmlPath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(xml.data()),
                static_cast<std::streamsize>(xml.size()));
        if (!f)
        {
            log<level::ERR>("biosconfig: cannot write decompressed XML",
                            entry("PATH=%s", tmpPath.c_str()));
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmpPath, xmlPath, ec);
    if (ec)
    {
        log<level::ERR>("biosconfig: cannot install decompressed XML",
                        entry("PATH=%s", xmlPath.c_str()),
                        entry("ERR=%s", ec.message().c_str()));
        return false;
    }

    try
    {
        bios::Xml biosxml(xmlPath.c_str());
        if (!biosxml.doDepexCompute())
        {
            log<level::WARNING>(
                "biosconfig: depex compute reported errors; continuing");
        }
        if (!biosxml.getBaseTable(table))
        {
            log<level::ERR>("biosconfig: getBaseTable produced no attributes");
            return false;
        }
        knobs = biosxml.getKnobList();

        // Force every attribute read-only *in this table*. The schema alone
        // carries no live values: it is a defaults template compiled into the
        // firmware, identical on every POST, in which every knob's CurrentVal
        // equals its own default and two knobs sharing one Setup byte
        // (CSM009/SLOTOPROM022 at 0x01BA) claim different "current" values --
        // impossible for a real read.
        //
        // This is the bootstrap table only. The type-2 varstore snapshot that
        // follows in the same boot republishes these with the values the
        // firmware actually read, and marks them writable, because that path
        // does have a write-back route (type 3 -> gRT->SetVariable). Until it
        // lands, read-only is the honest answer.
        for (auto& attribute : table)
        {
            std::get<1>(attribute.second) = true;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("biosconfig: BIOS XML parse failed",
                        entry("ERR=%s", e.what()));
        return false;
    }
}

// ===========================================================================
// Live values (payload type 2) and staged writes (payload type 3)
//
// The type-1 XML is a defaults template: it says where every knob lives but
// its CurrentVal is the knob's own default. BiosCfgOobDxe on the host closes
// that gap by reading the backing UEFI variables with GetVariable and pushing
// their raw bytes as type 2, and by pulling type 3 back and applying it with
// SetVariable. Everything below is the BMC half of those two exchanges.
// ===========================================================================

// Schema knobs, parsed once from the last committed type-1 payload. Invalidated
// whenever a new schema is committed.
static std::vector<bios::knob::knob> g_knobs;
static bool g_knobsValid = false;

// The most recent type-2 snapshot, kept so GetPayload(type 3) can restrict the
// writes it hands back to varstores the firmware demonstrably has.
static asrock::varstore::Snapshot g_snapshot;

static std::string schemaXmlPath()
{
    return std::string(stateDir) + "/Payload" +
           std::to_string(static_cast<unsigned>(PayloadType::xmlType1)) +
           ".xml";
}

// Parse the knob list out of the committed schema. Touches no shared state,
// so the commit worker and the startup preload can call it off the event loop.
static std::vector<bios::knob::knob> loadSchemaKnobs()
{
    try
    {
        bios::Xml xml(schemaXmlPath().c_str());
        xml.doDepexCompute();
        return xml.getKnobList();
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("biosconfig: cannot load knob schema",
                        entry("PATH=%s", schemaXmlPath().c_str()),
                        entry("ERR=%s", e.what()));
        return {};
    }
}

// The knob list, parsed on demand. Normally already in memory -- preloaded at
// startup and replaced whenever a schema is committed -- so the synchronous
// parse here is only a fallback.
static const std::vector<bios::knob::knob>* schemaKnobs()
{
    if (!g_knobsValid)
    {
        g_knobs = loadSchemaKnobs();
        g_knobsValid = !g_knobs.empty();
    }
    return g_knobs.empty() ? nullptr : &g_knobs;
}

static bool getPendingAttributes(PendingAttributes& out)
{
    try
    {
        auto bus = ::getSdBus();
        auto m = bus->new_method_call(biosMgrService, biosMgrPath,
                                      "org.freedesktop.DBus.Properties", "Get");
        m.append(std::string(biosMgrIface), std::string("PendingAttributes"));
        auto reply = bus->call(m);
        std::variant<PendingAttributes> v;
        reply.read(v);
        out = std::get<PendingAttributes>(v);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Retire the staged attributes that the freshly received snapshot proves are
// now in effect.
//
// The host sends no explicit "applied" acknowledgement, and deliberately so: a
// SetVariable that returns EFI_SUCCESS is not evidence the BIOS kept the value.
// Using the next snapshot as the acknowledgement means an attribute only leaves
// PendingAttributes once the firmware has read the new value back out of its
// own varstore. Anything that silently failed simply stays pending and is
// retried on the next boot.
// ---------------------------------------------------------------------------
static std::size_t retireApplied(const std::vector<bios::knob::knob>& knobs,
                                 const asrock::varstore::Snapshot& snap,
                                 const PendingAttributes& pending,
                                 PendingAttributes& remaining)
{
    std::size_t retired = 0;

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

        bool applied = false;
        if (knob != nullptr)
        {
            const auto* rec = snap.find(knob->varstoreIndex);
            uint64_t live = 0;
            uint64_t want = 0;
            const auto& variant = std::get<1>(entry);
            const std::string wantStr =
                std::holds_alternative<std::string>(variant)
                    ? std::get<std::string>(variant)
                    : std::to_string(std::get<int64_t>(variant));

            if (rec != nullptr &&
                asrock::varstore::readField(*rec, knob->offset, knob->size,
                                            live) &&
                asrock::varstore::parseNumber(wantStr, want) && live == want)
            {
                applied = true;
            }
        }

        if (applied)
        {
            ++retired;
        }
        else
        {
            remaining.emplace(name, entry);
        }
    }

    return retired;
}

// Asynchronous: runs on the event loop after a snapshot is committed, and the
// host may be mid-POST waiting on KCS replies. Takes its own copies.
static void retirePendingFromSnapshot(std::vector<bios::knob::knob> knobs,
                                      asrock::varstore::Snapshot snap)
{
    ::getSdBus()->async_method_call(
        [knobs = std::move(knobs), snap = std::move(snap)](
            const boost::system::error_code& ec,
            const std::variant<PendingAttributes>& value) {
            if (ec)
            {
                return;
            }
            const auto& pending = std::get<PendingAttributes>(value);
            if (pending.empty())
            {
                return;
            }
            PendingAttributes remaining;
            const std::size_t retired =
                retireApplied(knobs, snap, pending, remaining);
            if (retired == 0)
            {
                return;
            }
            log<level::INFO>("biosconfig: staged attributes now live in the BIOS",
                             entry("RETIRED=%zu", retired),
                             entry("REMAINING=%zu", remaining.size()));
            ::getSdBus()->async_method_call(
                [](const boost::system::error_code& setEc) {
                    if (setEc)
                    {
                        log<level::ERR>(
                            "biosconfig: cannot update PendingAttributes",
                            entry("ERR=%s", setEc.message().c_str()));
                    }
                },
                biosMgrService, biosMgrPath, "org.freedesktop.DBus.Properties",
                "Set", std::string(biosMgrIface),
                std::string("PendingAttributes"),
                std::variant<PendingAttributes>(std::move(remaining)));
        },
        biosMgrService, biosMgrPath, "org.freedesktop.DBus.Properties", "Get",
        std::string(biosMgrIface), std::string("PendingAttributes"));
}

// ---------------------------------------------------------------------------
// Turn a type-2 snapshot into a BaseBIOSTable. Pure: runs on the commit worker.
// ---------------------------------------------------------------------------
static bool buildSnapshotTable(const std::vector<uint8_t>& blob,
                               const std::vector<bios::knob::knob>& knobs,
                               asrock::varstore::Snapshot& snap,
                               bios::BiosBaseTableType& table,
                               std::size_t& live)
{
    if (!asrock::varstore::parseSnapshot(blob, snap))
    {
        log<level::ERR>("biosconfig: malformed varstore snapshot",
                        entry("BYTES=%zu", blob.size()));
        return false;
    }

    live = 0;
    asrock::varstore::buildTable(knobs, snap, table, live);
    if (table.empty() || live == 0)
    {
        log<level::ERR>(
            "biosconfig: snapshot resolved no attribute; keeping old table",
            entry("VARSTORES=%zu", snap.records.size()),
            entry("COUNT=%zu", table.size()));
        return false;
    }
    return true;
}

// ===========================================================================
// Payload commit pipeline
//
// A completed transfer is ACKed as soon as it is integrity-checked and on
// disk; turning it into BaseBIOSTable happens afterwards, off the KCS path.
//
// The host waits for the reply to the final chunk, and its KCS transport gives
// every reply a fixed budget (roughly one to two seconds on this board's
// OpenOobPkg). This work used to run inside the handler -- LZMA inflate, a
// ~180 KB tinyxml2 parse, building the table and a blocking BaseBIOSTable
// write -- and one snapshot was measured at 2.48 s. The BIOS abandoned the
// reply, the interface wedged for the rest of POST, and everything after it
// was lost: BiosCfgOobDxe looped on 0x60/0x77 behind a black console and the
// boot override was never read.
//
// Merely deferring the same work onto the event loop would move the stall onto
// the host's next request, so the CPU half runs on a worker thread. ipmid is
// built with BOOST_ASIO_DISABLE_THREADS, so the worker never touches asio: it
// queues its completion under a mutex and signals an eventfd the event loop
// watches. The completion -- which owns all the state above, and makes its
// D-Bus calls asynchronously -- then runs back on the loop. Jobs run one at a
// time in arrival order, because the type-2 snapshot of a boot is resolved
// against the type-1 schema the same boot sends first.
// ===========================================================================
namespace commit
{

static int g_eventFd = -1;
static std::unique_ptr<boost::asio::posix::stream_descriptor> g_eventDesc;
static uint64_t g_eventCount = 0;
static std::mutex g_readyMutex;
static std::deque<std::function<void()>> g_ready; // filled by worker threads

static void armEvent();

static void onEvent(const boost::system::error_code& ec, std::size_t)
{
    if (ec == boost::asio::error::operation_aborted)
    {
        return;
    }
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        ready.swap(g_ready);
    }
    for (auto& fn : ready)
    {
        fn();
    }
    armEvent();
}

static void armEvent()
{
    g_eventDesc->async_read_some(
        boost::asio::buffer(&g_eventCount, sizeof(g_eventCount)), onEvent);
}

static bool init()
{
    if (g_eventDesc)
    {
        return true;
    }
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
    {
        log<level::ERR>("biosconfig: eventfd failed; committing inline");
        return false;
    }
    try
    {
        g_eventDesc = std::make_unique<boost::asio::posix::stream_descriptor>(
            *getIoContext(), fd);
    }
    catch (const std::exception& e)
    {
        ::close(fd);
        log<level::ERR>("biosconfig: cannot watch eventfd; committing inline",
                        entry("ERR=%s", e.what()));
        return false;
    }
    g_eventFd = fd;
    armEvent();
    return true;
}

// Run `work` on a worker thread and hand its result to `done` on the event
// loop. Falls back to running both inline if no thread can be started.
template <typename Result>
static void offload(std::function<Result()> work,
                    std::function<void(Result)> done)
{
    struct Task
    {
        std::function<Result()> work;
        std::function<void(Result)> done;
    };
    auto task = std::make_shared<Task>(Task{std::move(work), std::move(done)});

    if (init())
    {
        try
        {
            std::thread([task] {
                // An exception escaping a detached thread would terminate
                // ipmid; report it as an empty (failed) result instead.
                std::shared_ptr<Result> result;
                try
                {
                    result = std::make_shared<Result>(task->work());
                }
                catch (const std::exception& e)
                {
                    log<level::ERR>("biosconfig: commit worker failed",
                                    entry("ERR=%s", e.what()));
                    result = std::make_shared<Result>();
                }
                {
                    std::lock_guard<std::mutex> lock(g_readyMutex);
                    g_ready.emplace_back(
                        [task, result] { task->done(std::move(*result)); });
                }
                const uint64_t one = 1;
                // Nothing useful to do on failure: the counter only saturates.
                [[maybe_unused]] auto n = ::write(g_eventFd, &one, sizeof(one));
            }).detach();
            return;
        }
        catch (const std::system_error& e)
        {
            log<level::WARNING>("biosconfig: no worker thread; committing "
                                "inline",
                                entry("ERR=%s", e.what()));
        }
    }
    task->done(task->work());
}

struct Job
{
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    uint32_t checksum = 0;
};

static std::deque<Job> g_jobs;
static bool g_busy = false;

static void next();

static void finished()
{
    g_busy = false;
    next();
}

struct XmlResult
{
    bool ok = false;
    bios::BiosBaseTableType table;
    std::vector<bios::knob::knob> knobs;
};

static void parseAndCommitXml(Job job)
{
    const uint8_t type = job.type;
    const uint32_t checksum = job.checksum;
    const std::size_t bytes = job.payload.size();
    const std::string xmlPath = std::string(stateDir) + "/Payload" +
                                std::to_string(type) + ".xml";

    offload<XmlResult>(
        [payload = std::move(job.payload), xmlPath] {
            XmlResult r;
            r.ok = parseBiosXml(payload, xmlPath, r.table, r.knobs);
            return r;
        },
        [type, checksum, bytes](XmlResult r) {
            if (!r.ok)
            {
                log<level::WARNING>(
                    "biosconfig: BIOS XML captured but BaseBIOSTable not "
                    "updated (see prior errors)",
                    entry("TYPE=%u", static_cast<unsigned>(type)),
                    entry("BYTES=%zu", bytes));
                finished();
                return;
            }
            commitBaseBIOSTable(
                std::move(r.table), "BIOS XML",
                [type, checksum, bytes,
                 knobs = std::move(r.knobs)](bool ok) mutable {
                    if (ok)
                    {
                        g_committedChecksum[type] = checksum;
                        // Only the type-1 document is the knob schema
                        // (schemaXmlPath); adopt the list this parse produced.
                        if (type == static_cast<uint8_t>(
                                        PayloadType::xmlType1) &&
                            !knobs.empty())
                        {
                            g_knobs = std::move(knobs);
                            g_knobsValid = true;
                        }
                        storeCapability();
                        log<level::INFO>(
                            "biosconfig: BaseBIOSTable updated from BIOS XML",
                            entry("TYPE=%u", static_cast<unsigned>(type)),
                            entry("BYTES=%zu", bytes));
                    }
                    else
                    {
                        log<level::WARNING>(
                            "biosconfig: BIOS XML captured but BaseBIOSTable "
                            "not updated (see prior errors)",
                            entry("TYPE=%u", static_cast<unsigned>(type)),
                            entry("BYTES=%zu", bytes));
                    }
                    finished();
                });
        });
}

static void runXmlJob(Job job)
{
    // This BIOS re-pushes a byte-identical document on every POST, so
    // re-parsing it costs an LZMA inflate, a ~180 KB tinyxml2 parse and a full
    // BaseBIOSTable write that change nothing. Skip all of that when the
    // payload matches what we last committed and the table is still
    // published. Note this only saves BMC work: the BIOS ignores completion
    // codes (it sends cmd 0x26, gets 0xC1, and transfers anyway), so the KCS
    // chunks still have to be received and ACKed either way.
    loadCapability();
    if (job.checksum != g_committedChecksum[job.type])
    {
        parseAndCommitXml(std::move(job));
        return;
    }
    auto pending = std::make_shared<Job>(std::move(job));
    baseBiosTableSize([pending](std::optional<size_t> published) {
        if (published.value_or(0) > 0)
        {
            log<level::INFO>(
                "biosconfig: payload unchanged, keeping BaseBIOSTable",
                entry("TYPE=%u", static_cast<unsigned>(pending->type)),
                entry("CRC=0x%08X", pending->checksum),
                entry("COUNT=%zu", *published));
            finished();
            return;
        }
        parseAndCommitXml(std::move(*pending));
    });
}

struct SnapshotResult
{
    bool ok = false;
    bool knobsLoaded = false;
    std::vector<bios::knob::knob> knobs;
    asrock::varstore::Snapshot snap;
    bios::BiosBaseTableType table;
    std::size_t live = 0;
};

// The live varstore bytes. This is what makes the published values real: it
// always follows the schema (skipped or not) in the same boot, so a schema
// re-push can no longer leave Redfish showing defaults until the next power-off
// flash sync. No checksum bookkeeping here: recordPayload() already filled
// g_meta[2], which is what GetPayload(Info) answers, so the host's skip gate is
// driven by the last snapshot this ipmid instance actually saw. Losing that on
// restart is the right behaviour -- a fresh instance wants the values again.
static void runSnapshotJob(Job job)
{
    std::optional<std::vector<bios::knob::knob>> knobs;
    if (g_knobsValid && !g_knobs.empty())
    {
        knobs = g_knobs; // the worker gets its own copy
    }
    const std::size_t bytes = job.payload.size();

    offload<SnapshotResult>(
        [payload = std::move(job.payload), knobs = std::move(knobs)]() mutable {
            SnapshotResult r;
            if (knobs)
            {
                r.knobs = std::move(*knobs);
            }
            else
            {
                r.knobs = loadSchemaKnobs();
                r.knobsLoaded = true;
            }
            if (r.knobs.empty())
            {
                // The schema arrives as a separate payload; on a fresh BMC the
                // host sends it in the same boot, but ordering is the host's
                // choice.
                log<level::WARNING>(
                    "biosconfig: varstore snapshot received before any schema");
                return r;
            }
            r.ok = buildSnapshotTable(payload, r.knobs, r.snap, r.table,
                                      r.live);
            return r;
        },
        [bytes](SnapshotResult r) {
            if (r.knobsLoaded && !r.knobs.empty() && !g_knobsValid)
            {
                g_knobs = r.knobs;
                g_knobsValid = true;
            }
            if (!r.ok)
            {
                log<level::WARNING>(
                    "biosconfig: varstore snapshot captured but not "
                    "published (see prior errors)",
                    entry("BYTES=%zu", bytes));
                finished();
                return;
            }
            const std::size_t varstores = r.snap.records.size();
            const std::size_t count = r.table.size();
            const std::size_t live = r.live;
            commitBaseBIOSTable(
                std::move(r.table), "live varstores",
                [varstores, count, live, bytes, knobs = std::move(r.knobs),
                 snap = std::move(r.snap)](bool ok) mutable {
                    if (ok)
                    {
                        log<level::INFO>(
                            "biosconfig: BaseBIOSTable updated from live "
                            "varstores",
                            entry("VARSTORES=%zu", varstores),
                            entry("COUNT=%zu", count),
                            entry("LIVE=%zu", live));
                        retirePendingFromSnapshot(knobs, snap);
                        g_snapshot = std::move(snap);
                    }
                    else
                    {
                        log<level::WARNING>(
                            "biosconfig: varstore snapshot captured but not "
                            "published (see prior errors)",
                            entry("BYTES=%zu", bytes));
                    }
                    finished();
                });
        });
}

static void next()
{
    if (g_busy || g_jobs.empty())
    {
        return;
    }
    g_busy = true;
    Job job = std::move(g_jobs.front());
    g_jobs.pop_front();
    if (job.type == static_cast<uint8_t>(PayloadType::varstoreSnapshot))
    {
        runSnapshotJob(std::move(job));
    }
    else
    {
        runXmlJob(std::move(job));
    }
}

static void enqueue(uint8_t type, std::vector<uint8_t> payload,
                    uint32_t checksum)
{
    // A newer payload of a type still waiting supersedes it in place, which
    // keeps a boot loop from piling up work and preserves schema-first order.
    for (auto& queued : g_jobs)
    {
        if (queued.type == type)
        {
            queued = Job{type, std::move(payload), checksum};
            return;
        }
    }
    g_jobs.push_back(Job{type, std::move(payload), checksum});
    next();
}

// Have the knob schema in memory before the host first asks for staged
// settings, so GetPayload(type 3) never parses the XML on the event loop.
static void preloadSchema()
{
    std::error_code ec;
    if (!fs::exists(schemaXmlPath(), ec))
    {
        return; // fresh BMC: the host will send the schema
    }
    offload<std::vector<bios::knob::knob>>(
        [] { return loadSchemaKnobs(); },
        [](std::vector<bios::knob::knob> knobs) {
            if (!g_knobsValid && !knobs.empty())
            {
                g_knobs = std::move(knobs);
                g_knobsValid = true;
            }
        });
}

} // namespace commit

// ---------------------------------------------------------------------------
// Encode PendingAttributes into the type-3 payload slot so GetPayload can serve
// it. Rebuilt on every GetPayload so the host always sees the current staging
// state, not whatever was staged when the last one was built.
//
// The "generation" is the CRC32 of the records themselves rather than a
// counter: it needs no persistence, and it changes exactly when the staged set
// changes. The host stores the last generation it applied and resets at most
// once per change, so a value it has already written can never drive a boot
// loop.
// ---------------------------------------------------------------------------
static void refreshPendingPayload()
{
    constexpr auto type = static_cast<size_t>(PayloadType::pendingSettings);

    const auto* knobs = schemaKnobs();
    PendingAttributes pending;
    getPendingAttributes(pending);

    std::vector<uint8_t> blob;
    std::size_t count = 0;
    if (knobs != nullptr && !pending.empty())
    {
        // Two passes: the generation has to cover the encoded records, so build
        // once with a zero generation to hash, then re-encode with it.
        count = asrock::varstore::buildPendingBlob(*knobs, g_snapshot, pending,
                                                   0, blob);
        if (count != 0)
        {
            uint32_t gen = crc32(blob.data(), blob.size());
            if (gen == 0)
            {
                gen = 1; // 0 means "nothing staged" to the host
            }
            asrock::varstore::buildPendingBlob(*knobs, g_snapshot, pending, gen,
                                               blob);
        }
    }
    if (count == 0)
    {
        // Still publish a well-formed empty blob: the host asks every boot and
        // a valid "nothing staged" answer is cheaper than a failed fetch.
        asrock::varstore::buildPendingBlob({}, {}, {}, 0, blob);
    }

    g_payload[type] = blob;

    PayloadMeta& md = g_meta[type];
    md.version = 1;
    md.type = static_cast<uint8_t>(type);
    md.totalSize = static_cast<uint32_t>(blob.size());
    md.totalChecksum = crc32(blob.data(), blob.size());
    md.flag = 0;
    md.status = static_cast<uint8_t>(PayloadStatus::valid);
    md.timestamp = static_cast<uint32_t>(::time(nullptr));
}

// ---------------------------------------------------------------------------
// Persist a completed payload to disk (best effort) and record its metadata.
// ---------------------------------------------------------------------------
static void recordPayload(uint8_t type, const std::vector<uint8_t>& data,
                          uint32_t checksum, uint16_t version, uint8_t status)
{
    if (type >= maxPayloadTypes)
    {
        return;
    }
    g_payload[type] = data;

    PayloadMeta& md = g_meta[type];
    md.version = version;
    md.type = type;
    md.totalSize = static_cast<uint32_t>(data.size());
    md.totalChecksum = checksum;
    md.flag = 0;
    md.status = status;
    md.timestamp = static_cast<uint32_t>(::time(nullptr));

    ensureStateDir();
    std::ofstream f(std::string(stateDir) + "/Payload" + std::to_string(type),
                    std::ios::binary | std::ios::trunc);
    if (f)
    {
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
}

// ---------------------------------------------------------------------------
// Typed error helper: return just a completion code from a handler that
// otherwise returns payload types (RspType<T...> is a std::tuple alias).
// ---------------------------------------------------------------------------
template <typename... T>
static ipmi::RspType<T...> rsp(uint8_t cc)
{
    return {cc, std::nullopt};
}

// ===========================================================================
// 0xD3  SetBIOSCapabilities
// ===========================================================================
static ipmi::RspType<> ipmiSetBIOSCap(uint8_t capability, uint8_t reserved1,
                                      uint8_t reserved2, uint8_t reserved3)
{
    if (reserved1 || reserved2 || reserved3)
    {
        return ipmi::responseInvalidFieldRequest();
    }
    // Load before storing: the BIOS issues SetBIOSCap on every POST, ahead of
    // the SetPayload transfer. Without this, the store below would write back a
    // zeroed committedChecksum array and defeat the unchanged-payload skip.
    loadCapability();
    g_capability = capability;
    g_capabilityInit = true;
    storeCapability();
    return ipmi::responseSuccess();
}

// ===========================================================================
// 0xD4  GetBIOSCapabilities
// ===========================================================================
static ipmi::RspType<uint8_t, uint8_t, uint8_t, uint8_t> ipmiGetBIOSCap()
{
    loadCapability();
    return ipmi::responseSuccess(g_capability, 0, 0, 0);
}

// ===========================================================================
// 0xD5  SetPayload — chunked host->BMC transfer state machine
//   byte0: TransferState, byte1: PayloadType, rest: state-dependent
// ===========================================================================
static ipmi::RspType<uint32_t> ipmiSetPayload(uint8_t stateByte,
                                              uint8_t payloadType,
                                              std::vector<uint8_t> reqData)
{
    if (payloadType >= maxPayloadTypes)
    {
        return rsp<uint32_t>(ipmi::ccParmOutOfRange);
    }
    const auto state = static_cast<TransferState>(stateByte);

    switch (state)
    {
        case TransferState::startTransfer:
        {
            // Intel PayloadStartTransfer (packed, 11 bytes):
            //   uint16_t payloadVersion
            //   uint32_t payloadTotalSize
            //   uint32_t payloadTotalChecksum
            //   uint8_t  payloadFlag
            // Confirmed by KCS capture: [01 00 | 97 35 00 00 | CC CF BC 66 | 00]
            if (reqData.size() < 11)
            {
                return rsp<uint32_t>(ccPayloadLengthIllegal);
            }
            uint32_t totalSize = rd32(reqData, 2);
            if (totalSize == 0 || totalSize > maxPayloadSize)
            {
                return rsp<uint32_t>(ccPayloadLengthIllegal);
            }
            g_session = Session{};
            g_session.active = true;
            g_session.type = payloadType;
            g_session.version =
                static_cast<uint16_t>(reqData[0] | (reqData[1] << 8));
            g_session.totalSize = totalSize;
            g_session.totalChecksum = rd32(reqData, 6);
            // reqData[10] = payloadFlag (unused)
            g_session.reservationId = nextReservationId();
            g_session.buffer.reserve(g_session.totalSize);
            return ipmi::responseSuccess(g_session.reservationId);
        }

        case TransferState::inProgress:
        {
            // reqData: reservationId(4) offset(4) curSize(4) curChecksum(4) data
            if (reqData.size() < 16)
            {
                return rsp<uint32_t>(ccPayloadLengthIllegal);
            }
            if (!g_session.active || g_session.type != payloadType)
            {
                return rsp<uint32_t>(ccNotSupportedInCurrentState);
            }
            uint32_t resId = rd32(reqData, 0);
            uint32_t offset = rd32(reqData, 4);
            uint32_t curSize = rd32(reqData, 8);
            uint32_t curChecksum = rd32(reqData, 12);
            if (resId != g_session.reservationId)
            {
                return rsp<uint32_t>(ccNotSupportedInCurrentState);
            }
            const size_t dataLen = reqData.size() - 16;
            if (curSize != dataLen)
            {
                return rsp<uint32_t>(ccPayloadLengthIllegal);
            }
            if (offset != g_session.buffer.size())
            {
                // out-of-order / missed packet
                return rsp<uint32_t>(ccPayloadPacketMissed);
            }
            if (crc32(reqData.data() + 16, dataLen) != curChecksum)
            {
                return rsp<uint32_t>(ccPayloadChecksumFailed);
            }
            g_session.buffer.insert(g_session.buffer.end(),
                                    reqData.begin() + 16, reqData.end());
            return ipmi::responseSuccess(curSize);
        }

        case TransferState::endTransfer:
        {
            if (reqData.size() < 4)
            {
                return rsp<uint32_t>(ccPayloadLengthIllegal);
            }
            if (!g_session.active || g_session.type != payloadType ||
                rd32(reqData, 0) != g_session.reservationId)
            {
                return rsp<uint32_t>(ccNotSupportedInCurrentState);
            }
            if (g_session.buffer.size() != g_session.totalSize)
            {
                g_session.active = false;
                return rsp<uint32_t>(ccPayloadInComplete);
            }
            uint32_t whole =
                crc32(g_session.buffer.data(), g_session.buffer.size());
            if (whole != g_session.totalChecksum)
            {
                recordPayload(payloadType, g_session.buffer, whole,
                              g_session.version,
                              static_cast<uint8_t>(PayloadStatus::corrupted));
                g_session.active = false;
                return rsp<uint32_t>(ccPayloadChecksumFailed);
            }

            // Integrity OK — persist. Persisting first guarantees the raw
            // payload (currently BIOS-config XML) is always on disk at
            // /var/lib/asrock-bios-config/PayloadN for inspection.
            std::vector<uint8_t> buf = std::move(g_session.buffer);
            uint16_t version = g_session.version;
            g_session.active = false;
            recordPayload(payloadType, buf, whole, version,
                          static_cast<uint8_t>(PayloadStatus::valid));

            // Turning the payload into BaseBIOSTable happens after this reply
            // and off the event loop -- see the commit pipeline for why the
            // host must not wait for it. A parse failure is only logged: the
            // bytes are on disk either way and the handshake completes.
            const auto received = static_cast<uint32_t>(buf.size());
            if (payloadType == static_cast<uint8_t>(PayloadType::xmlType0) ||
                payloadType == static_cast<uint8_t>(PayloadType::xmlType1) ||
                payloadType ==
                    static_cast<uint8_t>(PayloadType::varstoreSnapshot))
            {
                commit::enqueue(payloadType, std::move(buf), whole);
            }
            return ipmi::responseSuccess(received);
        }

        case TransferState::userAbort:
        {
            g_session = Session{};
            return ipmi::responseSuccess(static_cast<uint32_t>(0));
        }
    }
    return rsp<uint32_t>(ipmi::ccInvalidFieldRequest);
}

// ===========================================================================
// 0xD6  GetPayload
//   byte0: GetParam (Info/Data/Status), byte1: PayloadType
//   Data: + offset(4) length(4)
// ===========================================================================
static ipmi::RspType<std::vector<uint8_t>>
    ipmiGetPayload(uint8_t paramByte, uint8_t payloadType,
                   std::vector<uint8_t> reqData)
{
    if (payloadType >= maxPayloadTypes)
    {
        return rsp<std::vector<uint8_t>>(ipmi::ccParmOutOfRange);
    }

    loadCapability();
    const auto param = static_cast<GetParam>(paramByte);

    // Type 3 is generated, not stored: it is PendingAttributes as of right now,
    // encoded as varstore writes. Rebuild it when the host asks for the info
    // header -- and only then. The host reads Info, then pulls the body in
    // chunks against the size and CRC32 that header quoted, so regenerating
    // mid-transfer (a Redfish PATCH can land between two chunks) would hand it
    // a body that fails its own integrity check. Every other type simply serves
    // whatever SetPayload last stored.
    if (payloadType == static_cast<uint8_t>(PayloadType::pendingSettings) &&
        param == GetParam::info)
    {
        refreshPendingPayload();
    }

    const PayloadMeta& md = g_meta[payloadType];
    const std::vector<uint8_t>& buf = g_payload[payloadType];
    std::vector<uint8_t> resp;

    switch (param)
    {
        case GetParam::info:
        {
            // version(2 LE) type(1) totalSize(4) totalChecksum(4) flag(1)
            // status(1) timestamp(4)
            resp.push_back(md.version & 0xFF);
            resp.push_back((md.version >> 8) & 0xFF);
            resp.push_back(md.type);
            appendLE32(resp, md.totalSize);
            appendLE32(resp, md.totalChecksum);
            resp.push_back(md.flag);
            resp.push_back(md.status);
            appendLE32(resp, md.timestamp);
            return ipmi::responseSuccess(resp);
        }

        case GetParam::data:
        {
            if (reqData.size() < 8)
            {
                return rsp<std::vector<uint8_t>>(ccPayloadLengthIllegal);
            }
            uint32_t offset = rd32(reqData, 0);
            uint32_t length = rd32(reqData, 4);
            if (offset > buf.size())
            {
                return rsp<std::vector<uint8_t>>(ccPayloadLengthIllegal);
            }
            uint32_t avail = static_cast<uint32_t>(buf.size()) - offset;
            uint32_t readCount = std::min(length, avail);

            // payloadType(1) readCount(4) checksum(4) data...
            resp.push_back(payloadType);
            appendLE32(resp, readCount);
            appendLE32(resp,
                       crc32(buf.data() + offset, readCount));
            resp.insert(resp.end(), buf.begin() + offset,
                        buf.begin() + offset + readCount);
            return ipmi::responseSuccess(resp);
        }

        case GetParam::status:
        {
            resp.push_back(payloadType);
            resp.push_back(md.status);
            return ipmi::responseSuccess(resp);
        }
    }
    return rsp<std::vector<uint8_t>>(ipmi::ccInvalidFieldRequest);
}

// ===========================================================================
// 0xD7  SetBIOSPwdHashInfo — host reports the admin password hash + seed.
//   seed(32) algoInfo(1) adminHash(<=64)
// Stored to the STANDARD bios-settings-manager seedData file.
// ===========================================================================
static ipmi::RspType<>
    ipmiSetBIOSPwdHashInfo(std::array<uint8_t, maxSeedSize> seed,
                           uint8_t algoInfo, std::vector<uint8_t> adminHash)
{
    if (adminHash.size() > maxHashSize)
    {
        return ipmi::responseReqDataLenInvalid();
    }

    Json j;
    j["HashAlgo"] = (algoInfo == 2) ? "SHA384" : "SHA256";
    j["Seed"] = std::vector<uint8_t>(seed.begin(), seed.end());
    j["IsAdminPwdChanged"] = true;
    j["AdminPwdHash"] = adminHash;
    j["IsUserPwdChanged"] = false;
    j["UserPwdHash"] = std::vector<uint8_t>{};
    j["StatusFlag"] = 1;

    try
    {
        std::error_code ec;
        fs::create_directories(fs::path(seedDataFile).parent_path(), ec);
        std::ofstream f(seedDataFile, std::ios::trunc);
        if (!f)
        {
            return ipmi::responseUnspecifiedError();
        }
        f << j.dump();
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("biosconfig: seedData write failed",
                        entry("ERR=%s", e.what()));
        return ipmi::responseUnspecifiedError();
    }
    return ipmi::responseSuccess();
}

// ===========================================================================
// 0xD8  GetBIOSPwdHash — return seed(32) flag(1) adminHash(<=64).
// If no seed exists yet, mint one from /dev/urandom so the host can proceed.
// ===========================================================================
static ipmi::RspType<std::vector<uint8_t>> ipmiGetBIOSPwdHash()
{
    std::array<uint8_t, maxSeedSize> seed{};
    std::vector<uint8_t> adminHash;
    uint8_t flag = 0;

    bool haveSeed = false;
    std::ifstream f(seedDataFile);
    if (f)
    {
        try
        {
            Json j;
            f >> j;
            if (j.contains("Seed"))
            {
                auto s = j["Seed"].get<std::vector<uint8_t>>();
                for (size_t i = 0; i < seed.size() && i < s.size(); ++i)
                {
                    seed[i] = s[i];
                }
                haveSeed = true;
            }
            if (j.contains("AdminPwdHash"))
            {
                adminHash = j["AdminPwdHash"].get<std::vector<uint8_t>>();
            }
            flag = j.value("StatusFlag", 0);
        }
        catch (const std::exception&)
        {
            haveSeed = false;
        }
    }

    if (!haveSeed)
    {
        std::ifstream r("/dev/urandom", std::ios::binary);
        if (r)
        {
            r.read(reinterpret_cast<char*>(seed.data()),
                   static_cast<std::streamsize>(seed.size()));
        }
    }
    if (adminHash.size() > maxHashSize)
    {
        adminHash.resize(maxHashSize);
    }

    std::vector<uint8_t> resp;
    resp.insert(resp.end(), seed.begin(), seed.end());
    resp.push_back(flag);
    resp.insert(resp.end(), adminHash.begin(), adminHash.end());
    return ipmi::responseSuccess(resp);
}

// ---------------------------------------------------------------------------
// Registration — runs when phosphor-ipmi-host loads the provider library.
// ---------------------------------------------------------------------------
static void registerBiosConfigCommands() __attribute__((constructor));

static void registerBiosConfigCommands()
{
    log<level::INFO>("asrock biosconfig: registering BIOS OOB config commands "
                     "(NetFn 0x30, cmd 0xD3-0xD8 — Intel path, used by AMI BIOS)");

    const auto netFn = static_cast<ipmi::NetFn>(netFnGeneral);

    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdSetBIOSCap,
                          ipmi::Privilege::Admin, ipmiSetBIOSCap);
    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdGetBIOSCap,
                          ipmi::Privilege::User, ipmiGetBIOSCap);
    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdSetPayload,
                          ipmi::Privilege::Admin, ipmiSetPayload);
    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdGetPayload,
                          ipmi::Privilege::User, ipmiGetPayload);
    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdSetBIOSPwdHashInfo,
                          ipmi::Privilege::Admin, ipmiSetBIOSPwdHashInfo);
    ipmi::registerHandler(ipmi::prioOemBase, netFn, cmdGetBIOSPwdHash,
                          ipmi::Privilege::User, ipmiGetBIOSPwdHash);

    // Runs once the event loop is up (this constructor runs at dlopen).
    post_work([] { commit::preloadSchema(); });
}

} // namespace biosconfig
} // namespace asrock
