// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ---------------------------------------------------------------------------
// DMTF DSP0270 "Redfish Host Interface" credential bootstrapping over IPMI
// ---------------------------------------------------------------------------
// The host BIOS reaches the BMC's Redfish service over the USB-RNDIS host
// interface (usb0, 169.254.0.17 — see recipes-phosphor/network/usb-network).
// Before it can issue authenticated HTTPS requests it needs credentials, and
// DSP0270 forbids baking a fixed password into firmware. Instead the BIOS asks
// for a single-use account over the *system interface* (KCS), which is
// inherently host-only and therefore trusted:
//
//     NetFn 0x2C (Group Extension)
//     Group Extension Identification 0x52 ('R' — Redfish)
//     Command 0x02  Get Bootstrap Account Credentials
//
// Request  : [0] group (0x52)
//            [1] Disable Credential Bootstrapping control  (optional)
//                0xA5 = disable bootstrapping until the next BMC reset
// Response : [0] group (0x52)
//            [1..16]  username, 16 bytes ASCII, space padded
//            [17..32] password, 16 bytes ASCII, space padded
//
// Completion codes: 0x00 success, 0x80 "bootstrapping disabled".
//
// Upstream phosphor-ipmi-host implements no 0x2C group beyond DCMI (0xDC) and
// SBMR (0xAE), so without this provider the command returns 0xC1 and a
// standards-conformant BIOS has no way to authenticate.
//
// SECURITY MODEL
//   * Served ONLY on the system interface (KCS). A request arriving on any LAN
//     channel is refused with 0xD4 (insufficient privilege) — otherwise anyone
//     on the management network could mint an Administrator account.
//   * Each call replaces the account, so a leaked credential dies at the next
//     bootstrap. The account is deleted and recreated rather than reused.
//   * The account is a Redfish account only: groups {redfish}, no ipmi/ssh, so
//     the credential cannot be turned around into an IPMI or shell session.
//   * Password bytes are wiped from memory after packing the response.
//
// NOTE: this is belt-and-braces on this board. bmcweb also grants unauthenticated
// Administrator to connections landing on 169.254.0.17 (mirroring stock MegaRAC's
// AuthNoneRoleId), so a BIOS that skips bootstrapping still works. This exists so
// a BIOS that follows DSP0270 properly gets a conformant answer instead of 0xC1.

#include <ipmid/api.hpp>
#include <ipmid/api-types.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <user_channel/user_layer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace asrock
{
namespace redfishhi
{

using phosphor::logging::level;
using phosphor::logging::log;

// DSP0270 group extension id. 0x52 is 'R'; it is NOT 0x04 (a common mis-read of
// the spec — 0x04 is unassigned and returns 0xC1).
static constexpr ipmi::Group groupRedfish = 0x52;
static constexpr ipmi::Cmd cmdGetBootstrapAccountCredentials = 0x02;

// DSP0270: 0xA5 in the optional control byte disables further bootstrapping.
static constexpr uint8_t disableBootstrapMagic = 0xA5;
// DSP0270 completion code: bootstrapping is disabled.
static constexpr uint8_t ccBootstrapDisabled = 0x80;

static constexpr size_t credentialLen = 16;
static constexpr auto bootstrapUser = "bootstrap";

// Set by a request carrying 0xA5; cleared only by a BMC reset (this provider is
// reloaded with ipmid, so process lifetime == the required scope).
static bool bootstrapDisabled = false;

// ---------------------------------------------------------------------------
// Credential generation
// ---------------------------------------------------------------------------
// std::random_device on this kernel is backed by getrandom(2); it is the same
// entropy source bmcweb uses for session tokens.
static std::string randomCredential(size_t len, bool passwordCharset)
{
    // Deliberately no ambiguous glyphs (0/O, 1/l/I) — these credentials get
    // typed by hand during bring-up more often than anyone admits.
    static constexpr char alnum[] = "abcdefghijkmnopqrstuvwxyz"
                                    "ABCDEFGHJKLMNPQRSTUVWXYZ"
                                    "23456789";
    // phosphor-user-manager enforces a complexity policy via pam_cracklib; mix
    // in punctuation for passwords so a generated one is never rejected.
    static constexpr char punct[] = "!@#%^*-_=+";

    std::random_device rd;
    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; ++i)
    {
        // Every 5th character of a password is punctuation.
        if (passwordCharset && i > 0 && (i % 5) == 0)
        {
            out.push_back(punct[rd() % (sizeof(punct) - 1)]);
        }
        else
        {
            out.push_back(alnum[rd() % (sizeof(alnum) - 1)]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Account management via xyz.openbmc_project.User.Manager
// ---------------------------------------------------------------------------
// ipmid's user_layer is deliberately NOT used to create the account: it creates
// IPMI users (group "ipmi", occupying one of the 15 IPMI user slots). We want a
// Redfish-only account. Password setting does go through user_layer, because
// ipmiSetSpecialUserPassword() is the supported way to set a password on a
// non-IPMI account from inside ipmid.
static bool recreateBootstrapAccount(ipmi::Context::ptr ctx,
                                     const std::string& user,
                                     const std::string& password)
{
    constexpr auto userMgrService = "xyz.openbmc_project.User.Manager";
    constexpr auto userMgrObj = "/xyz/openbmc_project/user";
    constexpr auto userMgrIface = "xyz.openbmc_project.User.Manager";
    constexpr auto deleteIface = "xyz.openbmc_project.Object.Delete";

    boost::system::error_code ec;

    // Delete any prior bootstrap account so each call yields a fresh secret.
    // A missing account is the normal first-boot case, so the error is ignored.
    std::string userObj = std::string(userMgrObj) + "/" + user;
    ctx->bus->yield_method_call<>(ctx->yield, ec, userMgrService, userObj,
                                  deleteIface, "Delete");
    ec.clear();

    // groups {redfish} only — no ipmi, no ssh, no hostconsole.
    std::vector<std::string> groups{"redfish"};
    ctx->bus->yield_method_call<>(ctx->yield, ec, userMgrService, userMgrObj,
                                  userMgrIface, "CreateUser", user, groups,
                                  std::string("priv-admin"), true);
    if (ec)
    {
        log<level::ERR>("RHI bootstrap: CreateUser failed",
                        phosphor::logging::entry("ERROR=%s",
                                                 ec.message().c_str()));
        return false;
    }

    ipmi::SecureString secure(password.c_str());
    if (ipmi::ipmiSetSpecialUserPassword(user, secure) != ipmi::ccSuccess)
    {
        log<level::ERR>("RHI bootstrap: setting account password failed");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// NetFn 0x2C / group 0x52 / cmd 0x02 — Get Bootstrap Account Credentials
// ---------------------------------------------------------------------------
static ipmi::RspType<std::array<uint8_t, credentialLen>, // username
                     std::array<uint8_t, credentialLen>> // password
    getBootstrapAccountCredentials(ipmi::Context::ptr ctx,
                                   std::optional<uint8_t> disableControl)
{
    // DSP0270 delivers credentials over the host interface only. ipmid reports
    // the system interface as channel type "system-interface"; on this board
    // that is channel 3 (ipmi_kcs3, see channel_config.json). Refuse anything
    // arriving over LAN so a management-network client cannot mint an admin.
    ipmi::ChannelInfo chInfo;
    if (ipmi::getChannelInfo(ctx->channel, chInfo) != ipmi::ccSuccess)
    {
        return ipmi::responseUnspecifiedError();
    }
    if (chInfo.mediumType !=
        static_cast<uint8_t>(ipmi::EChannelMediumType::systemInterface))
    {
        log<level::WARNING>(
            "RHI bootstrap: refused off the system interface",
            phosphor::logging::entry("CHANNEL=%d", ctx->channel));
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }

    if (bootstrapDisabled)
    {
        return ipmi::response(ccBootstrapDisabled);
    }

    // The control byte is optional; only 0xA5 means anything.
    bool disableAfter =
        disableControl.has_value() && *disableControl == disableBootstrapMagic;

    std::string user = bootstrapUser;
    std::string password = randomCredential(credentialLen, true);

    if (!recreateBootstrapAccount(ctx, user, password))
    {
        return ipmi::responseUnspecifiedError();
    }

    // Pack space-padded fixed-width fields (DSP0270 uses fixed 16-byte fields;
    // it does not require NUL termination).
    std::array<uint8_t, credentialLen> userOut{};
    std::array<uint8_t, credentialLen> passOut{};
    userOut.fill(' ');
    passOut.fill(' ');
    std::copy_n(user.begin(), std::min(user.size(), credentialLen),
                userOut.begin());
    std::copy_n(password.begin(), std::min(password.size(), credentialLen),
                passOut.begin());

    // Do not leave the plaintext lying in the heap.
    std::fill(password.begin(), password.end(), '\0');

    if (disableAfter)
    {
        bootstrapDisabled = true;
        log<level::INFO>("RHI bootstrap: further bootstrapping disabled by host "
                         "until BMC reset");
    }

    log<level::INFO>("RHI bootstrap: issued Redfish bootstrap credentials to "
                     "the host interface");

    return ipmi::responseSuccess(userOut, passOut);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static void registerRedfishHiCommands() __attribute__((constructor));

static void registerRedfishHiCommands()
{
    log<level::INFO>("asrock redfish-hi: registering DSP0270 credential "
                     "bootstrap (NetFn 0x2C, group 0x52, cmd 0x02)");

    // Privilege::Admin: the command hands out an administrator account, and the
    // system-interface check above is the real gate.
    ipmi::registerGroupHandler(ipmi::prioOemBase, groupRedfish,
                               cmdGetBootstrapAccountCredentials,
                               ipmi::Privilege::Admin,
                               getBootstrapAccountCredentials);
}

} // namespace redfishhi
} // namespace asrock
