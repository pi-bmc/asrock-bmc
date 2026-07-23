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
// PAYLOAD FORMAT (confirmed by capture): the SetPayload StartTransfer header
// carries payloadType 1 and a ~13.7 KB body, i.e. Intel-Aptio BIOS-config XML
// (Intel server BIOS is AMI Aptio, so this board's AMI BIOS emits the same XML).
// This handler runs the full chunked transfer + CRC32 and persists the assembled
// payload to /var/lib/asrock-bios-config/PayloadN. Turning that XML into the
// BaseBIOSTable D-Bus property needs the concrete Aptio schema — capture PayloadN
// from a boot, then add an XML->BiosBaseTable parser (it can reuse the property
// commit in the [[maybe_unused]] setBaseBIOSTable() below). Transfers are ACKed
// as successful meanwhile so the BIOS handshake completes.
//
// The kcsmonitor filter (same library) logs every inbound request over KCS.

#include "biosconfigcommands.hpp"

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/message/types.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <boost/crc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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
// handlers below are never re-entered concurrently — no locking needed.
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
    }
    catch (const std::exception&)
    {
        // fall back to default 0
    }
}

static void storeCapability()
{
    ensureStateDir();
    Json j;
    j["capability"] = g_capability;
    std::ofstream f(std::string(stateDir) + "/nvdata.json", std::ios::trunc);
    if (f)
    {
        f << j.dump();
    }
}

// ---------------------------------------------------------------------------
// JSON <-> D-Bus value mapping
// ---------------------------------------------------------------------------
static std::string attrTypeToDbus(const std::string& s)
{
    if (s.rfind("xyz.openbmc_project", 0) == 0)
    {
        return s; // already a fully-qualified enum string
    }
    return std::string(attrTypePrefix) + s;
}

static std::string boundTypeToDbus(const std::string& s)
{
    if (s.rfind("xyz.openbmc_project", 0) == 0)
    {
        return s;
    }
    return std::string(boundTypePrefix) + s;
}

static std::string stripEnumPrefix(const std::string& s)
{
    auto pos = s.rfind('.');
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

static DbusVariant jsonToVariant(const Json& j)
{
    if (j.is_boolean())
    {
        return static_cast<int64_t>(j.get<bool>() ? 1 : 0);
    }
    if (j.is_number_integer() || j.is_number_unsigned())
    {
        return static_cast<int64_t>(j.get<int64_t>());
    }
    if (j.is_number_float())
    {
        return static_cast<int64_t>(llround(j.get<double>()));
    }
    if (j.is_string())
    {
        return j.get<std::string>();
    }
    return j.dump(); // objects/arrays: keep something rather than throw
}

// ---------------------------------------------------------------------------
// Populate xyz.openbmc_project.BIOSConfig.Manager.BaseBIOSTable from a JSON
// document. Accepts either {"attributes": {name: {...}}} or a bare {name: {...}}
// top-level map. Returns true on success.
//
// Currently unused: the AMI BIOS was found to push Aptio XML, not JSON. Kept as
// the reference D-Bus commit path — the forthcoming XML parser builds the same
// BiosBaseTable and reuses the property-Set block at the bottom of this fn.
// ---------------------------------------------------------------------------
[[maybe_unused]] static bool
    setBaseBIOSTable(const std::vector<uint8_t>& payload)
{
    Json root;
    try
    {
        root = Json::parse(payload.begin(), payload.end());
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("biosconfig: BaseBIOSTable JSON parse failed",
                        entry("ERR=%s", e.what()));
        return false;
    }

    const Json& attrs = root.contains("attributes") ? root["attributes"] : root;
    if (!attrs.is_object())
    {
        log<level::ERR>("biosconfig: payload has no attribute object");
        return false;
    }

    BiosBaseTable table;
    for (auto it = attrs.begin(); it != attrs.end(); ++it)
    {
        const std::string& name = it.key();
        const Json& a = it.value();
        if (!a.is_object())
        {
            continue;
        }

        std::string atype =
            attrTypeToDbus(a.value("attributeType", std::string("String")));
        bool readOnly = a.value("readOnly", false);
        std::string display = a.value("displayName", name);
        std::string desc = a.value("description", std::string(""));
        std::string menu = a.value("menuPath", std::string(""));

        DbusVariant cur = a.contains("currentValue")
                              ? jsonToVariant(a["currentValue"])
                              : DbusVariant(std::string(""));
        DbusVariant def =
            a.contains("defaultValue") ? jsonToVariant(a["defaultValue"]) : cur;

        BiosOptions options;
        if (a.contains("options") && a["options"].is_array())
        {
            for (const Json& o : a["options"])
            {
                std::string bt =
                    boundTypeToDbus(o.value("boundType", std::string("OneOf")));
                DbusVariant bv = o.contains("value")
                                     ? jsonToVariant(o["value"])
                                     : DbusVariant(std::string(""));
                std::string bn = o.value("valueName", std::string(""));
                options.emplace_back(std::move(bt), std::move(bv),
                                     std::move(bn));
            }
        }

        table.emplace(name, BiosTableEntry{std::move(atype), readOnly,
                                           std::move(display), std::move(desc),
                                           std::move(menu), std::move(cur),
                                           std::move(def), std::move(options)});
    }

    try
    {
        auto bus = ::getSdBus();
        auto m = bus->new_method_call(biosMgrService, biosMgrPath,
                                      "org.freedesktop.DBus.Properties", "Set");
        m.append(std::string(biosMgrIface), std::string("BaseBIOSTable"),
                 std::variant<BiosBaseTable>(table));
        bus->call(m);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("biosconfig: failed to set BaseBIOSTable",
                        entry("ERR=%s", e.what()));
        return false;
    }

    log<level::INFO>("biosconfig: BaseBIOSTable populated from host payload",
                     entry("COUNT=%zu", table.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Serialise the manager's PendingAttributes to JSON for GetPayload readback.
// Currently unused (readback of staged changes will serve XML, matching the
// BIOS's push format); kept for reference/debug tooling.
// ---------------------------------------------------------------------------
[[maybe_unused]] static std::vector<uint8_t> pendingAttributesJson()
{
    Json out;
    Json attrs = Json::object();
    try
    {
        auto bus = ::getSdBus();
        auto m = bus->new_method_call(biosMgrService, biosMgrPath,
                                      "org.freedesktop.DBus.Properties", "Get");
        m.append(std::string(biosMgrIface), std::string("PendingAttributes"));
        auto reply = bus->call(m);
        std::variant<PendingAttributes> v;
        reply.read(v);
        const auto& pending = std::get<PendingAttributes>(v);
        for (const auto& [name, entryTuple] : pending)
        {
            const auto& atype = std::get<0>(entryTuple);
            const auto& value = std::get<1>(entryTuple);
            Json a;
            a["attributeType"] = stripEnumPrefix(atype);
            std::visit([&a](const auto& x) { a["value"] = x; }, value);
            attrs[name] = a;
        }
    }
    catch (const std::exception& e)
    {
        log<level::WARNING>("biosconfig: PendingAttributes read failed",
                            entry("ERR=%s", e.what()));
    }
    out["pendingAttributes"] = attrs;
    std::string s = out.dump();
    return std::vector<uint8_t>(s.begin(), s.end());
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

            // The AMI BIOS sends BIOS-config XML (payloadType 0/1). Parsing
            // that Aptio XML into BaseBIOSTable requires the concrete schema —
            // capture Payload0/Payload1 from a live boot, then wire an
            // XML->BiosBaseTable parser here (it can reuse the D-Bus commit in
            // setBaseBIOSTable()). Until then we still ACK the transfer as
            // successful so the BIOS handshake completes and the bytes land.
            if (payloadType == static_cast<uint8_t>(PayloadType::xmlType0) ||
                payloadType == static_cast<uint8_t>(PayloadType::xmlType1))
            {
                log<level::INFO>(
                    "biosconfig: BIOS-config XML payload captured "
                    "(BaseBIOSTable parser pending)",
                    entry("TYPE=%u", static_cast<unsigned>(payloadType)),
                    entry("BYTES=%u",
                          static_cast<unsigned>(buf.size())));
            }
            return ipmi::responseSuccess(static_cast<uint32_t>(buf.size()));
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

    // GetPayload serves whatever SetPayload last stored for this type. The
    // BMC->host readback of Redfish-staged changes (Intel serves that back as
    // XML) depends on a BaseBIOSTable->XML writer, which is future work.
    const PayloadMeta& md = g_meta[payloadType];
    const std::vector<uint8_t>& buf = g_payload[payloadType];
    const auto param = static_cast<GetParam>(paramByte);
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
}

} // namespace biosconfig
} // namespace asrock
