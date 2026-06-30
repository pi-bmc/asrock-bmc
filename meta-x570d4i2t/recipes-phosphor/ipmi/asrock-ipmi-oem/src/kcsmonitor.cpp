// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Generic IPMI request monitor for the X570D4I-2T.
//
// Registers a pass-through IPMI *filter*. The filter hook is the only point in
// phosphor-ipmi-host that sees EVERY inbound message before command dispatch —
// crucially including NetFn/Cmd pairs that have no registered handler, which a
// normal ipmi::registerHandler() callback never observes. For each request the
// filter logs the full envelope (channel, NetFn/LUN/Cmd, requesting user and
// privilege, IPMB source address) plus a hex dump of the payload, then returns
// ccSuccess so the message proceeds untouched to its normal handler (or to the
// "invalid command" path if unhandled).
//
// Purpose: capture everything the host BIOS sends over the system interface
// (/dev/ipmi-kcs3 — channel 3 in channel_config.json) so undocumented or
// unhandled commands (e.g. SMM/POST platform events) become visible in the
// journal. All channels are logged; each line carries CHANNEL= so KCS3 traffic
// is trivially greppable:
//
//   journalctl -t asrock-ipmi-oem -f | grep 'IPMI-REQ.*CHANNEL=3'
//
// To restrict the monitor to a single channel at the source, gate logRequest()
// on ctx->channel (see kcsChannel below).

#include <ipmid/api.hpp>
#include <ipmid/filter.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <phosphor-logging/log.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace asrock
{

// KCS3 system interface — channel "3" per phosphor-ipmi-config/channel_config.json.
// Left here for optional channel gating; the monitor logs all channels by default.
static constexpr int kcsChannel = 3;

static void registerKcsMonitor() __attribute__((constructor));

// -----------------------------------------------------------------------
// Render the raw request payload as "AA BB CC ...".
// -----------------------------------------------------------------------
static std::string hexDump(const ipmi::SecureBuffer& raw)
{
    std::string out;
    out.reserve(raw.size() * 3);
    char byte[4];
    for (uint8_t b : raw)
    {
        std::snprintf(byte, sizeof(byte), "%02X ", b);
        out += byte;
    }
    if (!out.empty())
    {
        out.pop_back(); // drop trailing space
    }
    return out;
}

// -----------------------------------------------------------------------
// Filter callback — runs on every inbound IPMI message, pre-dispatch.
// Passive: it never rejects, always returning ccSuccess.
// -----------------------------------------------------------------------
static ipmi::Cc logRequest(ipmi::message::Request::ptr request)
{
    const auto& ctx = request->ctx;

    // Optional: restrict to the KCS3 host interface only.
    //   if (ctx->channel != kcsChannel) { return ipmi::ccSuccess; }
    (void)kcsChannel;

    const std::string data = hexDump(request->payload.raw);

    // Emit a single self-contained line so the whole request envelope is
    // greppable with plain journalctl (no -o json/verbose needed), e.g.:
    //   journalctl -t phosphor-ipmi-host -f | grep 'IPMI-REQ ch=3'
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "IPMI-REQ ch=%d netfn=0x%02X lun=0x%02X cmd=0x%02X user=%d "
                  "priv=0x%02X rqSA=0x%02X len=%zu data=[",
                  ctx->channel, static_cast<unsigned>(ctx->netFn),
                  static_cast<unsigned>(ctx->lun),
                  static_cast<unsigned>(ctx->cmd), ctx->userId,
                  static_cast<unsigned>(ctx->priv),
                  static_cast<unsigned>(ctx->rqSA),
                  request->payload.raw.size());
    const std::string line = std::string(hdr) + data + "]";

    phosphor::logging::log<phosphor::logging::level::INFO>(line.c_str());

    // Passive monitor — always allow the message through.
    return ipmi::ccSuccess;
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
static void registerKcsMonitor()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock IPMI request monitor registered (filter, all channels)");

    ipmi::registerFilter(
        ipmi::prioOpenBmcBase,
        [](ipmi::message::Request::ptr request) { return logRequest(request); });
}

} // namespace asrock
