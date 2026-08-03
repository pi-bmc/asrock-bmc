// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Minimal shim for biosxml.hpp, which was lifted from intel-ipmi-oem and
// expects an ipmi::DbusVariant type. Intel's full types.hpp defines a very wide
// variant, but bios::Xml only ever stores std::string values into it, and the
// target property xyz.openbmc_project.BIOSConfig.Manager.BaseBIOSTable uses
// variant<int64_t, string>. Matching that exactly keeps the D-Bus wire type
// correct (a{s(sbsssvva(svs))}) and avoids pulling std::monostate (which
// sdbusplus cannot serialise) into the variant.

#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace ipmi
{
using DbusVariant = std::variant<int64_t, std::string>;
} // namespace ipmi
