// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
// Adapted from openbmc/intel-ipmi-oem src/biosconfigcommands.cpp
// (Copyright 2020 Intel Corporation, Apache-2.0).
//
// AMI-firmware adaptation of the OpenBMC "BIOS OOB config" IPMI command set.
// See biosconfigcommands.cpp for the full rationale; in short:
//
//   * The Intel original transfers Intel's proprietary BIOS Setup *XML* and
//     parses it with biosxml.hpp + a spawned converter (boost::process). This
//     board's host is AMI, which never emits Intel XML, so all of that is
//     removed.
//   * Here the SetPayload transport carries a *JSON* document that maps 1:1
//     onto the xyz.openbmc_project.BIOSConfig.Manager BaseBIOSTable property.
//     The AMI host pushes it over KCS (from an injected DXE, mirroring the
//     existing SmbiosBmcPushDxe SMBIOS push); the BMC deserialises it straight
//     into BaseBIOSTable so the stock bmcweb /redfish/v1/Systems/system/Bios
//     endpoints are populated with no Intel-specific translation step.
//
// The IPMI wire protocol (NetFn/cmd numbers, the chunked transfer state
// machine, CRC32 integrity) is kept identical to the de-facto OpenBMC
// BIOS-config contract so an OpenBMC-aware host implementation can drive it
// unchanged. All tunables live at the top of this header.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace asrock
{
namespace biosconfig
{

// ---------------------------------------------------------------------------
// IPMI NetFn / command numbers — Intel BIOS-config path (NetFn 0x30).
//
// EMPIRICALLY CONFIRMED (live KCS capture): the stock AMI host BIOS on this
// board drives the Intel/OpenBMC BIOS-config protocol directly — it sends
// SetBIOSCap (cmd 0xD3) on NetFn 0x30 (Intel "OEM one" / netFnGeneral) during
// POST. So these are NOT our own DXE's numbers to choose; they are what the
// real BIOS uses, and the handlers MUST match them exactly. Kept identical to
// openbmc/intel-ipmi-oem (NetFn 0x30, cmds 0xD3-0xD8).
// ---------------------------------------------------------------------------
constexpr uint8_t netFnGeneral = 0x30; // Intel "OEM one"; the AMI BIOS uses it

constexpr uint8_t cmdSetBIOSCap = 0xD3;
constexpr uint8_t cmdGetBIOSCap = 0xD4;
constexpr uint8_t cmdSetPayload = 0xD5;
constexpr uint8_t cmdGetPayload = 0xD6;
constexpr uint8_t cmdSetBIOSPwdHashInfo = 0xD7;
constexpr uint8_t cmdGetBIOSPwdHash = 0xD8;

// ---------------------------------------------------------------------------
// OEM completion codes (returned in addition to the standard IPMI set).
// ---------------------------------------------------------------------------
constexpr uint8_t ccPayloadPacketMissed = 0x80;
constexpr uint8_t ccPayloadChecksumFailed = 0x81;
constexpr uint8_t ccNotSupportedInCurrentState = 0x82;
constexpr uint8_t ccPayloadInComplete = 0x83;
constexpr uint8_t ccPayloadLengthIllegal = 0x84;
constexpr uint8_t ccBIOSCapabilityInitNotDone = 0x85;

// ---------------------------------------------------------------------------
// Payload types carried by Set/GetPayload.
//
// CONFIRMED by KCS capture: the AMI BIOS pushes payloadType 1. In Intel's
// scheme 0/1 are "IntelXMLType0/1" — a BIOS-config XML document (Intel server
// BIOS is AMI Aptio, so this board's AMI BIOS emits the same XML). Type 5 is an
// opaque/OTA blob stored to disk only.
// ---------------------------------------------------------------------------
enum class PayloadType : uint8_t
{
    // host -> BMC: BIOS-config XML -> BaseBIOSTable (both 0 and 1 are XML).
    xmlType0 = 0,
    xmlType1 = 1,
    // opaque blob, persisted to disk only.
    ota = 5,
    maxType = 6,
};

// SetPayload transfer-state selector (request byte 0).
enum class TransferState : uint8_t
{
    startTransfer = 0,
    inProgress = 1,
    endTransfer = 2,
    userAbort = 3,
};

// GetPayload parameter selector (request byte 0).
enum class GetParam : uint8_t
{
    info = 0,
    data = 1,
    status = 2,
};

// Payload validity, reported by GetPayload(Info/Status).
enum class PayloadStatus : uint8_t
{
    unknown = 0,
    valid = 1,
    corrupted = 2,
};

// Size limits (mirror intel-ipmi-oem header).
constexpr size_t maxHashSize = 64;
constexpr size_t maxSeedSize = 32;
constexpr size_t maxPayloadTypes = static_cast<size_t>(PayloadType::maxType);
// Upper bound on a single SetPayload transfer. The captured BIOS-config XML is
// ~13.7 KB; 1 MiB is a generous ceiling that rejects a corrupt StartTransfer
// (bad totalSize) before it triggers a huge buffer reservation.
constexpr uint32_t maxPayloadSize = 1024 * 1024;

// ---------------------------------------------------------------------------
// BaseBIOSTable D-Bus type — must match xyz.openbmc_project.BIOSConfig.Manager
// exactly. Wire signature: a{s(sbsssvva(svs))}.
//
//   key                : attribute name
//   AttributeType      : full enum string (…AttributeType.Integer, …String, …)
//   ReadonlyStatus     : bool
//   DisplayName        : string
//   Description        : string
//   MenuPath           : string
//   CurrentValue       : variant<int64_t,string>
//   DefaultValue       : variant<int64_t,string>
//   BoundValues        : array of (BoundType-string, variant, BoundValueName)
// ---------------------------------------------------------------------------
using DbusVariant = std::variant<int64_t, std::string>;
using BiosOption = std::tuple<std::string, DbusVariant, std::string>;
using BiosOptions = std::vector<BiosOption>;
using BiosTableEntry =
    std::tuple<std::string, bool, std::string, std::string, std::string,
               DbusVariant, DbusVariant, BiosOptions>;
using BiosBaseTable = std::map<std::string, BiosTableEntry>;

// PendingAttributes D-Bus type. Wire signature: a{s(sv)}.
using PendingAttrEntry = std::tuple<std::string, DbusVariant>;
using PendingAttributes = std::map<std::string, PendingAttrEntry>;

// ---------------------------------------------------------------------------
// D-Bus coordinates of the BIOS config manager (phosphor-bios-config /
// bios-settings-mgr). Stable well-known name; isolated here for clarity.
// ---------------------------------------------------------------------------
constexpr const char* biosMgrService = "xyz.openbmc_project.BIOSConfigManager";
constexpr const char* biosMgrPath = "/xyz/openbmc_project/bios_config/manager";
constexpr const char* biosMgrIface = "xyz.openbmc_project.BIOSConfig.Manager";

// Enum string prefixes for BaseBIOSTable / PendingAttributes.
constexpr const char* attrTypePrefix =
    "xyz.openbmc_project.BIOSConfig.Manager.AttributeType.";
constexpr const char* boundTypePrefix =
    "xyz.openbmc_project.BIOSConfig.Manager.BoundType.";

// ---------------------------------------------------------------------------
// On-BMC persistence. Intel used /var/oob/nvoobdata.dat; this board keeps its
// staging + capability state under its own directory. The password seed uses
// the STANDARD bios-settings-manager path (created by the biosconfig-manager
// bbappend), so it interoperates with the running manager.
// ---------------------------------------------------------------------------
constexpr const char* stateDir = "/var/lib/asrock-bios-config";
constexpr const char* seedDataFile =
    "/var/lib/bios-settings-manager/seedData";

} // namespace biosconfig
} // namespace asrock
