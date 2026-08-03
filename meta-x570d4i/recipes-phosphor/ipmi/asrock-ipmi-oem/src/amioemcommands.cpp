// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ---------------------------------------------------------------------------
// ASRock Rack OEM IPMI commands (NetFn 0x3A) + AMI OEM stubs (NetFn 0x32)
// ---------------------------------------------------------------------------
// The stock 2.59F BIOS drives a fixed set of OEM commands over KCS during POST.
// Upstream phosphor-ipmi-host registers none of them, so every one returns 0xC1
// "Invalid Command"; the BIOS retries each TEN times per POST cycle and gives
// up. Critically, the same capture shows NetFn 0x30 is never reached -- that is
// the BIOS-OOB-config transfer in biosconfigcommands.cpp -- so the handshake
// failing here plausibly aborts the whole out-of-band configuration sequence.
//
// The three handlers below are NOT guesses. They are transcribed from the stock
// MegaRAC implementation, `libasrrcmds.so.1.0.0`, whose command table
// `g_asrrcmds_CmdHndlr` (63 entries of {cmd, priv, handler, reqlen, ...}) maps
// every command seen in the capture:
//
//   0xBD -> SetBIOSPOSTStatus   reqlen 0x01
//   0xBE -> GetBIOSPOSTStatus   reqlen 0x00
//   0xB2 -> SetBIOSFWInfo       reqlen 0xFF (variable)
//   0xA1 -> GetMacAddr          0xAB -> GetChassisID   (not implemented here)
//
// WHY THE EARLIER ACCEPT-AND-LOG STUB WAS NOT ENOUGH
//   The BIOS sends 0xBD [01] and immediately follows with 0xBE [] -- a
//   write-then-read-back verify. The vendor's 0xBE returns TWO bytes,
//   [CC, status], echoing what 0xBD stored. A stub returning only a completion
//   code gives the BIOS nothing to verify against, which is the most likely
//   reason it retried ten times and abandoned the sequence.

#include <ipmid/api.hpp>
#include <ipmid/api-types.hpp>
#include <phosphor-logging/log.hpp>
#include <user_channel/channel_layer.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace asrock
{
namespace amioem
{

using phosphor::logging::level;
using phosphor::logging::log;

static constexpr ipmi::NetFn netFnAsrockOem = 0x3A; // ASRock Rack OEM
static constexpr ipmi::NetFn netFnAmiOem32 = 0x32;  // AMI OEM

// --- NetFn 0x3A, transcribed from libasrrcmds.so -------------------------
static constexpr ipmi::Cmd cmdSetBiosFwInfo = 0xB2;
static constexpr ipmi::Cmd cmdSetBiosPostStatus = 0xBD;
static constexpr ipmi::Cmd cmdGetBiosPostStatus = 0xBE;

// SetBIOSPOSTStatus rejects anything above 4 with 0xCC:
//     cmp r3, #4 ; mvnhi r3, #51 (= 0xCC) ; strbhi r3, [r6]
static constexpr uint8_t maxPostStatus = 4;

// SetBIOSFWInfo copies req[0..20] to AsrrGlobal+0x120..0x134 (21 bytes) and
// then calls SaveGlobalInfo(). The captured BIOS request is only 16 bytes and
// the table's reqlen is 0xFF (unchecked), so accept any length up to 21.
static constexpr size_t biosFwInfoMax = 21;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// The vendor keeps these in its persisted "AsrrGlobal" blob. Process lifetime
// is the right scope for us: POST status is meaningless across a BMC restart,
// and the BIOS re-sends its firmware info on every POST.
static uint8_t g_postStatus = 0;
static std::vector<uint8_t> g_biosFwInfo;

static std::string hex(const std::vector<uint8_t>& d)
{
    static constexpr char lut[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(d.size() * 3);
    for (uint8_t b : d)
    {
        s.push_back(lut[b >> 4]);
        s.push_back(lut[b & 0xF]);
        s.push_back(' ');
    }
    if (!s.empty())
    {
        s.pop_back();
    }
    return s;
}

// These commands come from the host firmware over KCS. A LAN client has no
// business driving the BIOS POST handshake, so gate on the system interface --
// same rule as the DSP0270 bootstrap handler.
static bool onSystemInterface(ipmi::Context::ptr ctx)
{
    ipmi::ChannelInfo chInfo;
    if (ipmi::getChannelInfo(ctx->channel, chInfo) != ipmi::ccSuccess)
    {
        return false;
    }
    return chInfo.mediumType ==
           static_cast<uint8_t>(ipmi::EChannelMediumType::systemInterface);
}

// ---------------------------------------------------------------------------
// 0xBD  SetBIOSPOSTStatus   request: 1 byte (0..4)   response: CC only
// ---------------------------------------------------------------------------
static ipmi::RspType<> setBiosPostStatus(ipmi::Context::ptr ctx,
                                         uint8_t status)
{
    if (!onSystemInterface(ctx))
    {
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }
    if (status > maxPostStatus)
    {
        // Vendor returns 0xCC for out-of-range.
        return ipmi::responseInvalidFieldRequest();
    }

    g_postStatus = status;

    // 1 = POST started, 4 = POST complete (the vendor latches a 32-bit counter
    // from AsrrGlobal+0x17B..0x17E into +0x17F..0x182 on 4). We keep the value;
    // nothing downstream consumes the counter yet.
    log<level::INFO>("ASRR-OEM SetBIOSPOSTStatus",
                     phosphor::logging::entry("STATUS=%u",
                                              static_cast<unsigned>(status)));
    return ipmi::responseSuccess();
}

// ---------------------------------------------------------------------------
// 0xBE  GetBIOSPOSTStatus   request: none   response: CC + 1 byte
// ---------------------------------------------------------------------------
// Vendor: resp[0]=CC, resp[1]=AsrrGlobal[0x172], returns length 2. The single
// data byte is what makes the BIOS's write-then-verify succeed.
static ipmi::RspType<uint8_t> getBiosPostStatus(ipmi::Context::ptr ctx)
{
    if (!onSystemInterface(ctx))
    {
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }
    return ipmi::responseSuccess(g_postStatus);
}

// ---------------------------------------------------------------------------
// 0xB2  SetBIOSFWInfo   request: up to 21 bytes   response: CC only
// ---------------------------------------------------------------------------
// Observed payload (16 bytes):
//     01 02 59 46 0A 20 12 13 00 00 14 00 00 00 44 00
// The 02 59 46 group lines up with BIOS 2.59F (major, BCD minor, ASCII suffix)
// and the following bytes look like a build date, but the field layout is not
// documented and is NOT decoded here -- we store the blob verbatim and log it
// so the encoding can be confirmed against a second BIOS version.
static ipmi::RspType<> setBiosFwInfo(ipmi::Context::ptr ctx,
                                     std::vector<uint8_t> info)
{
    if (!onSystemInterface(ctx))
    {
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }
    if (info.empty() || info.size() > biosFwInfoMax)
    {
        return ipmi::responseReqDataLenInvalid();
    }

    g_biosFwInfo = info;

    log<level::INFO>("ASRR-OEM SetBIOSFWInfo",
                     phosphor::logging::entry("LEN=%zu", info.size()),
                     phosphor::logging::entry("DATA=%s", hex(info).c_str()));
    return ipmi::responseSuccess();
}

// ---------------------------------------------------------------------------
// Everything else the BIOS asks for: accept and log
// ---------------------------------------------------------------------------
// 0xB5 (which carries the ASCII board name "X570D4I-2T") and 0xF3 are NOT in
// libasrrcmds.so's table -- they live in some other provider we have not
// located -- and the NetFn 0x32 commands are AMI's, not ASRock's. Without a
// reference implementation the honest response is the shortest legal one: a
// bare completion code, plus a log of the request so the payload is recoverable.
// If one of these turns out to need real data, the log gives us the bytes.
static ipmi::RspType<> oemAcceptAndLog(ipmi::Context::ptr ctx,
                                       std::vector<uint8_t> data)
{
    if (!onSystemInterface(ctx))
    {
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }
    std::string ascii;
    ascii.reserve(data.size());
    for (uint8_t b : data)
    {
        ascii.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
    }
    log<level::INFO>(
        "OEM handshake accepted (no reference implementation)",
        phosphor::logging::entry("NETFN=0x%02X", static_cast<int>(ctx->netFn)),
        phosphor::logging::entry("CMD=0x%02X", static_cast<int>(ctx->cmd)),
        phosphor::logging::entry("LEN=%zu", data.size()),
        phosphor::logging::entry("DATA=%s", hex(data).c_str()),
        phosphor::logging::entry("ASCII=%s", ascii.c_str()));
    return ipmi::responseSuccess();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static void registerAmiOemCommands() __attribute__((constructor));

static void registerAmiOemCommands()
{
    log<level::INFO>("asrock ami-oem: registering OEM handshake "
                     "(NetFn 0x3A: B2/BD/BE implemented from libasrrcmds; "
                     "B5 F3 A1 AB + NetFn 0x32 72 5D 3D accept-and-log)");

    ipmi::registerHandler(ipmi::prioOemBase, netFnAsrockOem,
                          cmdSetBiosPostStatus, ipmi::Privilege::Admin,
                          setBiosPostStatus);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAsrockOem,
                          cmdGetBiosPostStatus, ipmi::Privilege::Admin,
                          getBiosPostStatus);
    ipmi::registerHandler(ipmi::prioOemBase, netFnAsrockOem, cmdSetBiosFwInfo,
                          ipmi::Privilege::Admin, setBiosFwInfo);

    // Observed but unidentified -- deliberately not a catch-all, so anything
    // new still returns 0xC1 and shows up in the kcsmonitor log.
    for (ipmi::Cmd c : {0xB5, 0xF3, 0xA1, 0xAB})
    {
        ipmi::registerHandler(ipmi::prioOemBase, netFnAsrockOem,
                              static_cast<ipmi::Cmd>(c),
                              ipmi::Privilege::Admin, oemAcceptAndLog);
    }
    for (ipmi::Cmd c : {0x72, 0x5D, 0x3D})
    {
        ipmi::registerHandler(ipmi::prioOemBase, netFnAmiOem32,
                              static_cast<ipmi::Cmd>(c),
                              ipmi::Privilege::Admin, oemAcceptAndLog);
    }
}

} // namespace amioem
} // namespace asrock
