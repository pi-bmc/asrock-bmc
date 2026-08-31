// SPDX-License-Identifier: Apache-2.0
//
// p2a-inventory-monitor — publish host-supplied PCIe/NVMe inventory as phosphor
// Inventory.Item.* D-Bus objects, so bmcweb serves Redfish Storage / Drive /
// StorageController / PCIeDevice for hardware the BMC has no sideband to probe.
//
// The X570D4I-2T routes no NVMe-MI SMBus/MCTP path to the BMC (every i2c0
// PCA9545 channel is silent; SATA is invisible), so nvmesensor/phosphor-nvme can
// never populate these collections. Instead the host BIOS driver InventoryOobDxe
// (OpenOobPkg) enumerates the topology at ReadyToBoot and writes a compact,
// CRC'd blob into a fixed, DT-reserved window of BMC DRAM over the AST2500
// PCIe-to-AHB (P2A) bridge. This daemon owns the BMC end of that window: it
// unlocks the region for host access via the aspeed-p2a-ctrl driver, maps it,
// validates the blob, and republishes it as inventory the rest of OpenBMC
// already knows how to render.
//
// Freshness: the DRAM keeps its contents across host reboots, so a blob alone is
// not proof the data is current. We therefore gate publication on host power
// state — inventory is exposed only while the host is Running and cleared when it
// powers off — and additionally re-read whenever the blob's CRC changes.
//
// This daemon also publishes the host's onboard NIC MACs as
// Inventory.Item.NetworkInterface objects, read from the board FRU EEPROM (see
// the host-NIC section below). Those are BMC-local facts, independent of the
// P2A blob and of host power, and back bmcweb's Systems EthernetInterfaces
// routes (bmcweb layer patch 0004).
//
// Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.

#include "oob-inventory-blob.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// aspeed-p2a-ctrl UAPI. Declared inline rather than depending on the kernel's
// uapi/linux/aspeed-p2a-ctrl.h being staged in the sysroot.
// ---------------------------------------------------------------------------
namespace p2a
{
struct ctrl_mapping
{
    std::uint64_t addr;
    std::uint32_t length;
    std::uint32_t flags;
};
constexpr unsigned int kMagic = 0xb3;
constexpr unsigned int kReadWrite = 1;
// _IOW(0xb3, 0x00, struct) and _IOWR(0xb3, 0x01, struct), 16-byte struct.
inline constexpr unsigned long ioSetWindow()
{
    return (1UL << 30) | (sizeof(ctrl_mapping) << 16) | (kMagic << 8) | 0x00;
}
inline constexpr unsigned long ioGetConfig()
{
    return (3UL << 30) | (sizeof(ctrl_mapping) << 16) | (kMagic << 8) | 0x01;
}
constexpr const char* kDevice = "/dev/aspeed-p2a-ctrl";
} // namespace p2a

namespace
{
using Association = std::tuple<std::string, std::string, std::string>;

constexpr const char* kInventoryRoot = "/xyz/openbmc_project/inventory";
constexpr const char* kStoragePath =
    "/xyz/openbmc_project/inventory/system/storage/1";
constexpr const char* kPcieRoot =
    "/xyz/openbmc_project/inventory/system/pciedevice";
constexpr const char* kServiceName = "xyz.openbmc_project.P2AInventory";

// Redfish-facing chassis id of the mainboard; the daemon resolves its actual
// inventory object path (Inventory.Item.Board) at runtime and associates drives
// to it so /redfish/v1/Chassis/<id>/Drives is populated.
constexpr const char* kBoardName = "ASRock_Rack_X570D4I";

constexpr int kPollSeconds = 5;
constexpr int kResolveRetrySeconds = 10;

// Trimmed hwdata pci.ids, installed by this recipe. Absence is tolerated: names
// simply stay empty and the raw ids are published instead.
constexpr const char* kPciIdsPath =
    "/usr/share/p2a-inventory-monitor/pci.ids";

// -------- blob validation --------

// Validate a candidate blob living at `src` (at most `avail` bytes readable)
// and, if it is well-formed, copy exactly TotalSize bytes into `out`.
//
// Shared by the DRAM window and the on-disk cache so a cache file can never
// inject anything the live path would have rejected — it is regular data from
// a writable filesystem, not a trusted store.
bool extractBlob(const std::uint8_t* src, std::size_t avail,
                 std::vector<std::uint8_t>& out)
{
    if (src == nullptr || avail < sizeof(OOB_INV_HEADER))
    {
        return false;
    }
    OOB_INV_HEADER hdr{};
    std::memcpy(&hdr, src, sizeof(hdr));
    if (hdr.Magic != OOB_INV_MAGIC ||
        hdr.FormatVersion != OOB_INV_FORMAT_VERSION ||
        hdr.HeaderSize != sizeof(OOB_INV_HEADER))
    {
        return false;
    }
    if (hdr.TotalSize < sizeof(OOB_INV_HEADER) || hdr.TotalSize > avail ||
        hdr.PcieCount > OOB_INV_MAX_PCIE || hdr.DriveCount > OOB_INV_MAX_DRIVES)
    {
        return false;
    }
    if (hdr.PcieRecSize != sizeof(OOB_INV_PCIE_RECORD) ||
        hdr.DriveRecSize != sizeof(OOB_INV_DRIVE_RECORD))
    {
        return false;
    }
    const std::uint32_t expect =
        sizeof(OOB_INV_HEADER) +
        static_cast<std::uint32_t>(hdr.PcieCount) * sizeof(OOB_INV_PCIE_RECORD) +
        static_cast<std::uint32_t>(hdr.DriveCount) * sizeof(OOB_INV_DRIVE_RECORD);
    if (expect != hdr.TotalSize)
    {
        return false;
    }

    out.resize(hdr.TotalSize);
    std::memcpy(out.data(), src, hdr.TotalSize);

    // Verify CRC with the Crc32 field zeroed, exactly as the writer computed.
    auto* h = reinterpret_cast<OOB_INV_HEADER*>(out.data());
    const std::uint32_t got = h->Crc32;
    h->Crc32 = 0;
    const std::uint32_t calc = OobInvCrc32(out.data(), h->TotalSize);
    h->Crc32 = got;
    return calc == got;
}

// -------- on-disk cache --------
//
// Why persist at all: the host only pushes while it is running, but the drives
// and PCIe devices are still physically installed when it is off — and being
// able to answer "what is in this box" with the host powered down is the entire
// point of out-of-band management. smbios-mdr sets the precedent on this system
// (it keeps /var/lib/smbios/smbios2 and keeps serving it), so inventory here
// behaves the same way rather than blinking out at power-off.
//
// The tradeoff is explicit: cached inventory is last-known-good, so hardware
// swapped while the host was down reads stale until the next boot pushes a new
// blob. That is inherent to any OOB cache and is why the blob carries a CRC —
// a changed CRC is what triggers a refresh.
constexpr const char* kCachePath = "/var/lib/p2a-inventory/inventory.blob";

bool loadCache(std::vector<std::uint8_t>& out)
{
    int fd = ::open(kCachePath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    std::vector<std::uint8_t> raw(OOB_INV_REGION_LEN);
    ssize_t n = ::read(fd, raw.data(), raw.size());
    ::close(fd);
    if (n <= 0)
    {
        return false;
    }
    return extractBlob(raw.data(), static_cast<std::size_t>(n), out);
}

// Write-to-temp + rename, so a power loss mid-write cannot leave a torn file
// that would then fail validation on the next boot and lose the inventory.
void saveCache(const std::vector<std::uint8_t>& buf)
{
    // systemd's StateDirectory= normally creates this, but create it here too so
    // the daemon still caches when run by hand (which is how it gets debugged).
    ::mkdir("/var/lib/p2a-inventory", 0755);

    const std::string tmp = std::string(kCachePath) + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return;
    }
    const std::uint8_t* p = buf.data();
    std::size_t left = buf.size();
    while (left > 0)
    {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0)
        {
            ::close(fd);
            ::unlink(tmp.c_str());
            return;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    ::rename(tmp.c_str(), kCachePath);
}

// -------- blob access --------

// Maps the P2A DRAM window once; subsequent reads are plain memory copies.
class BlobWindow
{
  public:
    bool open()
    {
        fd = ::open(p2a::kDevice, O_RDWR | O_SYNC);
        if (fd < 0)
        {
            return false;
        }
        p2a::ctrl_mapping cfg{};
        if (::ioctl(fd, p2a::ioGetConfig(), &cfg) < 0 || cfg.length == 0)
        {
            // Fall back to the contract's fixed geometry if the driver did not
            // report a configured memory-region.
            cfg.addr = OOB_INV_BMC_PHYS_ADDR;
            cfg.length = OOB_INV_REGION_LEN;
        }
        length = cfg.length;

        // Unlock the region for host (PCIe) writes; without this the BIOS P2A
        // writes are dropped by the SCU region gating.
        p2a::ctrl_mapping win{cfg.addr, cfg.length, p2a::kReadWrite};
        ::ioctl(fd, p2a::ioSetWindow(), &win);

        base = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
        return base != MAP_FAILED;
    }

    // Copies a validated blob out of the window into buf. Returns false if no
    // well-formed blob is present.
    bool read(std::vector<std::uint8_t>& buf) const
    {
        if (base == MAP_FAILED || base == nullptr)
        {
            return false;
        }
        return extractBlob(static_cast<const std::uint8_t*>(base), length, buf);
    }

    ~BlobWindow()
    {
        if (base != MAP_FAILED && base != nullptr)
        {
            ::munmap(base, length);
        }
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

  private:
    int fd = -1;
    void* base = MAP_FAILED;
    std::size_t length = 0;
};

// Trim to a std::string, stopping at the first NUL (fields are NUL/space
// trimmed by the writer already, but guard anyway).
std::string field(const char* p, std::size_t n)
{
    std::size_t len = 0;
    while (len < n && p[len] != '\0')
    {
        len++;
    }
    std::string s(p, len);
    while (!s.empty() && s.back() == ' ')
    {
        s.pop_back();
    }
    return s;
}

std::string generationDbus(std::uint8_t gen)
{
    // Maps to bmcweb's pcie_util::redfishPcieGenerationFromDbus.
    return "xyz.openbmc_project.Inventory.Item.PCIeSlot.Generations.Gen" +
           std::to_string(gen);
}

std::string hex16(std::uint16_t v)
{
    char b[8];
    std::snprintf(b, sizeof(b), "0x%04x", v);
    return b;
}

std::string hex8(std::uint8_t v)
{
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02x", v);
    return b;
}

// PCI base class -> Redfish PCIeFunction DeviceClass enum value.
//
// bmcweb copies Function0DeviceClass into the Redfish payload VERBATIM -- it
// only checks the string is non-empty (redfish-core/lib/pcie.hpp, which still
// carries a TODO about mapping these properly) -- exactly as it already does
// for Function0FunctionType. So this must return the BARE Redfish enum value
// ("DisplayController"), not a fully-qualified D-Bus enum path. Returning the
// latter would put a non-schema string straight into the response.
//
// The mapping is 1:1 with the PCI-SIG base class byte, which is also what the
// "C xx" class section of pci.ids enumerates -- Redfish's enum was named from
// it, so every base class the host can report has an exact counterpart.
std::string deviceClassRedfish(std::uint8_t base, std::uint8_t sub)
{
    switch (base)
    {
        case 0x00: return "UnclassifiedDevice";
        case 0x01: return "MassStorageController";
        case 0x02: return "NetworkController";
        case 0x03: return "DisplayController";
        case 0x04: return "MultimediaController";
        case 0x05: return "MemoryController";
        case 0x06: return "Bridge";
        case 0x07: return "CommunicationController";
        case 0x08: return "GenericSystemPeripheral";
        case 0x09: return "InputDeviceController";
        case 0x0a: return "DockingStation";
        // The one place the subclass matters: PCI puts co-processors under
        // base class 0x0b subclass 0x40, and Redfish has a distinct
        // Coprocessor value for exactly that.
        case 0x0b: return sub == 0x40 ? "Coprocessor" : "Processor";
        case 0x0c: return "SerialBusController";
        case 0x0d: return "WirelessController";
        case 0x0e: return "IntelligentController";
        case 0x0f: return "SatelliteCommunicationsController";
        case 0x10: return "EncryptionController";
        case 0x11: return "SignalProcessingController";
        case 0x12: return "ProcessingAccelerators";
        case 0x13: return "NonEssentialInstrumentation";
        case 0x40: return "Coprocessor";
        case 0xff: return "UnassignedClass";
        default: return "Other";
    }
}

// Vendor/device name resolution against a pci.ids table.
//
// The installed file is stock hwdata pci.ids with the subsystem lines and the
// class section stripped at build time (see the recipe): ~900 KB of
// "vvvv  Vendor name" lines, each followed by tab-indented "dddd  Device name"
// lines. Full hwdata is deliberately NOT installed -- it also ships usb.ids and
// the ~5 MB oui.txt, and this board's 32 MiB rofs has no room to spare.
//
// Lookup is one linear scan per publish, filtered to the ids actually present,
// rather than a map parsed up front: a publish happens only when the host
// pushes a new blob, while a full table is ~40k entries that would sit resident
// for a daemon which otherwise idles.
class PciIdDb
{
  public:
    struct Names
    {
        std::string vendor;
        std::string device;
    };

    // Fold a (vendor, device) pair into one lookup word.
    static constexpr std::uint32_t key(std::uint16_t ven, std::uint16_t dev)
    {
        return (static_cast<std::uint32_t>(ven) << 16) | dev;
    }

    // Resolve every requested pair in a single pass. A missing file, an unknown
    // vendor and an unknown device are all non-errors: each just leaves the
    // corresponding string empty and the caller falls back to raw hex, which is
    // what this daemon published before names existed.
    static std::map<std::uint32_t, Names>
        resolve(const std::set<std::uint32_t>& wanted, const char* path)
    {
        std::map<std::uint32_t, Names> out;
        std::ifstream in(path);
        if (!in)
        {
            return out;
        }

        // Vendors owning at least one wanted pair, so a vendor name is still
        // captured when the device id itself is absent from the table.
        std::set<std::uint16_t> vendors;
        for (auto k : wanted)
        {
            vendors.insert(static_cast<std::uint16_t>(k >> 16));
        }

        std::string line;
        std::uint16_t curVendor = 0;
        bool curWanted = false;

        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            // The class section ("C 03  Display controller") reuses the same
            // one-tab indentation for its subclasses, so parsing past it would
            // file class names as device names. It is last in the file, and the
            // build-time filter drops it, but stop here regardless so this also
            // works against an untrimmed pci.ids.
            if (line[0] == 'C' && line.size() > 1 && line[1] == ' ')
            {
                break;
            }

            if (line[0] != '\t')
            {
                curVendor = parseHex16(line, 0);
                curWanted = vendors.count(curVendor) != 0;
                if (curWanted)
                {
                    // Seed every wanted pair belonging to this vendor, so a
                    // device the table does not list still gets a vendor name.
                    const std::string vname = nameAfterId(line, 0);
                    for (auto k : wanted)
                    {
                        if (static_cast<std::uint16_t>(k >> 16) == curVendor)
                        {
                            out[k].vendor = vname;
                        }
                    }
                }
                continue;
            }

            // Subsystem line, or a vendor with nothing wanted under it.
            if (!curWanted || line.size() < 2 || line[1] == '\t')
            {
                continue;
            }

            auto it = out.find(key(curVendor, parseHex16(line, 1)));
            if (it != out.end())
            {
                it->second.device = nameAfterId(line, 1);
            }
        }
        return out;
    }

  private:
    // Parse the hex id starting at `off`; 0 on anything malformed, which then
    // simply fails to match a wanted key.
    static std::uint16_t parseHex16(const std::string& s, std::size_t off)
    {
        std::uint32_t v = 0;
        std::size_t i = off;
        for (; i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]));
             i++)
        {
            const char c = s[i];
            const std::uint32_t d =
                (c <= '9') ? static_cast<std::uint32_t>(c - '0')
                           : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10);
            v = (v << 4) | d;
        }
        return (i == off) ? 0 : static_cast<std::uint16_t>(v);
    }

    // The name is whatever follows the id and its separating whitespace.
    static std::string nameAfterId(const std::string& s, std::size_t off)
    {
        std::size_t i = off;
        while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i])))
        {
            i++;
        }
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        {
            i++;
        }
        std::string n = s.substr(i);
        while (!n.empty() && (n.back() == ' ' || n.back() == '\r'))
        {
            n.pop_back();
        }
        return n;
    }
};

// -------- host NIC inventory from the board EEPROM --------
//
// The host's two onboard X550-AT2 10GbE MACs are provisioned in the same FRU
// EEPROM the BMC's own MACs come from (i2c7/0x57; the DTS taps 0x3F80/0x3F88
// as nvmem cells for the BMC's eth0/eth1). Four entries sit at 8-byte stride
// from 0x3F80 — BMC dedicated, BMC NC-SI shared, host LAN1, host LAN2 — each
// 6 MAC bytes plus a checksum byte chosen so all 7 sum to 0 mod 256. Verified
// on hardware 2026-08-30: the 0x3F90/0x3F98 entries are the MACs this host's
// X550 ports PXE-boot with.
//
// Unlike everything else this daemon serves, these are BMC-local facts about
// installed hardware, so they are published unconditionally — host on, off, or
// never yet booted — and are never torn down. bmcweb's Systems
// EthernetInterfaces routes (layer patch 0004) render them at
// /redfish/v1/Systems/system/EthernetInterfaces, which is where
// Metal3/Ironic-style provisioning discovers a machine's boot MACs.
constexpr const char* kMacEeprom = "/sys/bus/i2c/devices/7-0057/eeprom";
constexpr const char* kNicRoot =
    "/xyz/openbmc_project/inventory/system/network";

// Read one 7-byte EEPROM MAC entry at `offset`. False (and no publication) on
// a short read, a checksum mismatch, an unprogrammed all-zero entry, or a
// non-unicast first byte — an erased-flash 0xFF entry fails the checksum.
bool readEepromMac(int fd, off_t offset, std::array<std::uint8_t, 6>& mac)
{
    std::array<std::uint8_t, 7> e{};
    if (::pread(fd, e.data(), e.size(), offset) !=
        static_cast<ssize_t>(e.size()))
    {
        return false;
    }
    std::uint8_t sum = 0;
    bool allZero = true;
    for (std::size_t i = 0; i < e.size(); i++)
    {
        sum = static_cast<std::uint8_t>(sum + e[i]);
        if (i < mac.size() && e[i] != 0)
        {
            allZero = false;
        }
    }
    if (sum != 0 || allZero || (e[0] & 1u) != 0)
    {
        return false;
    }
    std::copy_n(e.begin(), mac.size(), mac.begin());
    return true;
}

std::string macString(const std::array<std::uint8_t, 6>& m)
{
    char b[18];
    std::snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1],
                  m[2], m[3], m[4], m[5]);
    return b;
}

// Publish both host NICs; the returned interfaces just need to stay alive for
// the daemon's lifetime. Ids follow the host OS naming (eth0 = LAN1) so the
// Redfish ids line up with what the deployed OS and its provisioning config
// call the same ports. A missing EEPROM or invalid entry publishes nothing:
// bmcweb then serves an empty (still valid) collection.
std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>>
    publishHostNics(sdbusplus::asio::object_server& server)
{
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>> out;
    int fd = ::open(kMacEeprom, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return out;
    }
    constexpr struct
    {
        const char* id;
        const char* pretty;
        off_t offset;
    } nics[] = {
        {"eth0", "Onboard LAN1 (Intel X550-AT2 10GbE)", 0x3F90},
        {"eth1", "Onboard LAN2 (Intel X550-AT2 10GbE)", 0x3F98},
    };
    for (const auto& n : nics)
    {
        std::array<std::uint8_t, 6> mac{};
        if (!readEepromMac(fd, n.offset, mac))
        {
            continue;
        }
        const std::string path = std::string(kNicRoot) + "/" + n.id;
        auto item =
            server.add_interface(path, "xyz.openbmc_project.Inventory.Item");
        item->register_property<std::string>("PrettyName", n.pretty);
        item->register_property("Present", true);
        item->initialize();
        out.push_back(item);

        auto nic = server.add_interface(
            path, "xyz.openbmc_project.Inventory.Item.NetworkInterface");
        nic->register_property<std::string>("MACAddress", macString(mac));
        nic->initialize();
        out.push_back(nic);
    }
    ::close(fd);
    return out;
}

// Holds every interface published for the current blob so a refresh can tear
// them all down and rebuild.
class Publication
{
  public:
    explicit Publication(sdbusplus::asio::object_server& s) : server(s) {}

    void clear()
    {
        for (auto& iface : ifaces)
        {
            server.remove_interface(iface);
        }
        ifaces.clear();
    }

    std::shared_ptr<sdbusplus::asio::dbus_interface> add(const std::string& path,
                                                         const std::string& name)
    {
        auto i = server.add_interface(path, name);
        ifaces.push_back(i);
        return i;
    }

    ~Publication()
    {
        clear();
    }

  private:
    sdbusplus::asio::object_server& server;
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>> ifaces;
};

class Monitor
{
  public:
    Monitor(boost::asio::io_context& io,
            std::shared_ptr<sdbusplus::asio::connection> conn,
            sdbusplus::asio::object_server& server) :
        io(io), conn(std::move(conn)), server(server), pub(server),
        pollTimer(io)
    {}

    bool start()
    {
        if (!window.open())
        {
            return false;
        }

        // Publish last-known-good inventory immediately, before we know the
        // host's power state and without waiting for a push. This is what makes
        // /redfish/v1/Systems/system/Storage answer with the host powered down,
        // and it also covers the window between BMC boot and the host's first
        // push on a machine that is already running.
        std::vector<std::uint8_t> cached;
        if (loadCache(cached))
        {
            const auto* h =
                reinterpret_cast<const OOB_INV_HEADER*>(cached.data());
            lastCrc = h->Crc32;
            lastBlob = cached;
            rebuild(lastBlob);
            published = true;
        }

        watchHostState();
        resolveBoard();
        poll();
        return true;
    }

  private:
    // ---- host power gating ----
    void watchHostState()
    {
        // Prompt reaction to power transitions; the poll loop is the backstop.
        hostMatch = std::make_unique<sdbusplus::bus::match_t>(
            *conn,
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',arg0='xyz.openbmc_project.State.Host',"
            "path='/xyz/openbmc_project/state/host0'",
            [this](sdbusplus::message_t& m) {
                std::string iface;
                std::vector<std::pair<std::string,
                                      std::variant<std::string>>>
                    props;
                try
                {
                    m.read(iface, props);
                }
                catch (const std::exception&)
                {
                    return;
                }
                for (auto& [name, val] : props)
                {
                    if (name == "CurrentHostState")
                    {
                        if (auto* s = std::get_if<std::string>(&val))
                        {
                            hostRunning = (s->find(".Running") != std::string::npos);
                        }
                    }
                }
            });

        queryHostState();
    }

    // One-shot read of the current host power state, retried until the State.Host
    // service answers — otherwise a host already Running when this daemon starts
    // (and never transitioning again) would never be observed and nothing would
    // publish.
    void queryHostState()
    {
        conn->async_method_call(
            [this](const boost::system::error_code& ec,
                   const std::variant<std::string>& v) {
                const std::string* s =
                    ec ? nullptr : std::get_if<std::string>(&v);
                if (s != nullptr)
                {
                    hostRunning = (s->find(".Running") != std::string::npos);
                    return;
                }
                auto t = std::make_shared<boost::asio::steady_timer>(io);
                t->expires_after(std::chrono::seconds(kResolveRetrySeconds));
                t->async_wait([this, t](const boost::system::error_code& e) {
                    if (!e)
                    {
                        queryHostState();
                    }
                });
            },
            "xyz.openbmc_project.State.Host",
            "/xyz/openbmc_project/state/host0",
            "org.freedesktop.DBus.Properties", "Get",
            "xyz.openbmc_project.State.Host", "CurrentHostState");
    }

    // ---- resolve the mainboard chassis object for drive associations ----
    void resolveBoard()
    {
        std::vector<std::string> boardIf = {
            "xyz.openbmc_project.Inventory.Item.Board"};
        conn->async_method_call(
            [this](const boost::system::error_code& ec,
                   const std::vector<std::string>& paths) {
                if (!ec)
                {
                    for (const auto& p : paths)
                    {
                        auto slash = p.rfind('/');
                        if (slash != std::string::npos &&
                            p.substr(slash + 1) == kBoardName)
                        {
                            boardPath = p;
                            // Re-publish immediately so the chassis->drive
                            // association lands on inventory that was already
                            // built without it. This matters most on the cache
                            // path: startup publishes before the mapper has
                            // answered, and with the host powered off no DRAM
                            // refresh will ever come along to fix it up — which
                            // would leave /Chassis/<id>/Drives empty while
                            // /Systems/system/Storage looked fine.
                            if (!lastBlob.empty())
                            {
                                rebuild(lastBlob);
                            }
                            return;
                        }
                    }
                }
                auto t = std::make_shared<boost::asio::steady_timer>(io);
                t->expires_after(std::chrono::seconds(kResolveRetrySeconds));
                t->async_wait([this, t](const boost::system::error_code& e) {
                    if (!e)
                    {
                        resolveBoard();
                    }
                });
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
            std::string(kInventoryRoot), 0, boardIf);
    }

    // ---- poll loop ----
    void poll()
    {
        // Only the LIVE window is gated on host power: DRAM keeps its contents
        // across host reboots, so reading it while the host is down could
        // re-publish a blob from a machine that has since been re-configured.
        // Already-published inventory is deliberately NOT withdrawn at
        // power-off — see the cache rationale above. It is refreshed, not
        // revoked, and only ever by a newer blob from a running host.
        std::vector<std::uint8_t> buf;
        if (hostRunning && window.read(buf))
        {
            auto* h = reinterpret_cast<const OOB_INV_HEADER*>(buf.data());
            if (h->Crc32 != lastCrc)
            {
                lastBlob = buf;
                rebuild(lastBlob);
                lastCrc = h->Crc32;
                published = true;
                // Persist only what we have actually published, so the cache
                // can never describe something Redfish never served.
                saveCache(lastBlob);
            }
        }

        pollTimer.expires_after(std::chrono::seconds(kPollSeconds));
        pollTimer.async_wait([this](const boost::system::error_code& ec) {
            if (!ec)
            {
                poll();
            }
        });
    }

    // ---- publish ----
    void rebuild(const std::vector<std::uint8_t>& buf)
    {
        pub.clear();

        const auto* h = reinterpret_cast<const OOB_INV_HEADER*>(buf.data());
        const auto* pcie = reinterpret_cast<const OOB_INV_PCIE_RECORD*>(
            buf.data() + sizeof(OOB_INV_HEADER));
        const auto* drives = reinterpret_cast<const OOB_INV_DRIVE_RECORD*>(
            buf.data() + sizeof(OOB_INV_HEADER) +
            static_cast<std::size_t>(h->PcieCount) * sizeof(OOB_INV_PCIE_RECORD));

        // Resolve every vendor/device name the blob refers to in one pass, up
        // front: both the PCIe objects and the drives synthesized from PCIe
        // functions read the result, and doing it per-record would re-scan the
        // ~900 KB table 49 times.
        {
            std::set<std::uint32_t> wanted;
            for (std::uint16_t i = 0; i < h->PcieCount; i++)
            {
                wanted.insert(PciIdDb::key(pcie[i].VendorId, pcie[i].DeviceId));
            }
            pciNames = PciIdDb::resolve(wanted, kPciIdsPath);
        }

        // One Storage subsystem object so the collection is non-empty.
        pub.add(kStoragePath, "xyz.openbmc_project.Inventory.Item.Storage")
            ->initialize();
        {
            auto item =
                pub.add(kStoragePath, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>("PrettyName", "NVMe Storage");
            item->register_property("Present", true);
            item->initialize();
        }

        std::vector<std::string> seenControllerSerials;

        for (std::uint16_t i = 0; i < h->DriveCount; i++)
        {
            const auto& d = drives[i];
            // The PCIe function owning this drive's controller (when the BIOS
            // resolved it) supplies the vendor name for the Asset Manufacturer
            // — NVMe Identify carries no manufacturer string of its own.
            const OOB_INV_PCIE_RECORD* owner = pcieForDrive(d, pcie,
                                                            h->PcieCount);
            const std::string vendor = owner ? namesFor(*owner).vendor : "";
            publishDrive(i, d, vendor);
            publishController(d, vendor, seenControllerSerials);
        }

        // Fallback when the BIOS reported no drives. This is the NORMAL case on
        // this board, not an error path: the stock AMI firmware ships its own
        // NVMe stack (Nvme / NvmeSmm / NvmeInt13 / NvmeRaidDxe) rather than
        // EDK2's NvmExpressDxe, and it never publishes the standard
        // EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL, so InventoryOobDxe finds nothing
        // to Identify and DriveCount comes back 0 on every boot.
        //
        // The controller is still visible as a PCIe function, so synthesize a
        // Drive from that: presence, protocol and location are real, and the
        // vendor/device IDs identify the part. Capacity stays 0 and life stays
        // 255, which bmcweb treats as "unknown" and omits from the Redfish
        // resource rather than reporting a wrong number. Retire this once a
        // real NVMe path (BAR0 admin queues, or BlockIo+device-path) lands.
        if (h->DriveCount == 0)
        {
            std::uint16_t synth = 0;
            for (std::uint16_t i = 0; i < h->PcieCount; i++)
            {
                if (pcie[i].ClassBase == 0x01 && pcie[i].ClassSub == 0x08)
                {
                    publishDriveFromPcie(synth, pcie[i]);
                    synth++;
                }
            }
        }

        for (std::uint16_t i = 0; i < h->PcieCount; i++)
        {
            publishPcie(pcie[i]);
        }
    }

    // Drive + StorageController synthesized from a PCIe mass-storage/NVMe
    // function, for when the firmware could not enumerate namespaces.
    void publishDriveFromPcie(std::uint16_t idx, const OOB_INV_PCIE_RECORD& p)
    {
        char loc[16];
        std::snprintf(loc, sizeof(loc), "%02x:%02x.%x", p.Bus, p.Device,
                      p.Function);
        const std::string id = "nvme" + std::to_string(idx);
        const std::string path = std::string(kStoragePath) + "/" + id;
        // The controller's PCI ids are all we have here (no Identify data), so
        // name it from the same table the PCIe objects use.
        const auto n = namesFor(p);
        const std::string model =
            n.device.empty() ? hex16(p.VendorId) + ":" + hex16(p.DeviceId)
                             : n.device;

        {
            auto drive =
                pub.add(path, "xyz.openbmc_project.Inventory.Item.Drive");
            drive->register_property<std::string>(
                "Type",
                "xyz.openbmc_project.Inventory.Item.Drive.DriveType.SSD");
            drive->register_property<std::string>(
                "Protocol",
                "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.NVMe");
            // 0 capacity and 255 life are both "unknown" to bmcweb; it omits
            // CapacityBytes and PredictedMediaLifeLeftPercent entirely.
            drive->register_property<std::uint64_t>("Capacity", 0);
            drive->register_property<std::uint8_t>(
                "PredictedMediaLifeLeftPercent", 255);
            drive->initialize();
        }
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            // The resolved name already says what this is ("NVMe SSD
            // Controller PM9C1a"), so prefixing it would stutter; only the
            // bare-ids fallback needs the "NVMe" qualifier and the location.
            item->register_property<std::string>(
                "PrettyName",
                n.device.empty() ? (std::string("NVMe ") + loc) : model);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(path, model, "", n.vendor);

        if (!boardPath.empty())
        {
            auto assoc =
                pub.add(path, "xyz.openbmc_project.Association.Definitions");
            assoc->register_property(
                "Associations",
                std::vector<Association>{{"chassis", "drive", boardPath}});
            assoc->initialize();
        }

        // Matching StorageController, so /Storage/1/Controllers is populated
        // alongside the drive rather than being an empty collection.
        const std::string cpath =
            std::string(kStoragePath) + "/controller_nvme" + std::to_string(idx);
        addStorageController(cpath);
        {
            auto item = pub.add(cpath, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>(
                "PrettyName", std::string("NVMe Controller ") + loc);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(cpath, model, "", n.vendor);
    }

    // The PCIe record for a drive's owning controller. Exact BDF match first —
    // but the BDF the NVMe pass-thru path records does not always agree with
    // the enumeration list (measured on HW 2026-08-30: the 990 EVO's drive
    // record says 0000:06:00.0 while enumeration has no bus 06 at all, only
    // the 0x0108 function at 0000:2b:00.0), so when the exact match fails
    // fall back to the class-0108 (NVMe) function IF there is exactly one:
    // with a single NVMe controller in the box the attribution is certain,
    // and with several it stays honest by returning nothing.
    const OOB_INV_PCIE_RECORD* pcieForDrive(const OOB_INV_DRIVE_RECORD& d,
                                            const OOB_INV_PCIE_RECORD* pcie,
                                            std::uint16_t count) const
    {
        if (d.Bus != OOB_INV_LOC_UNKNOWN)
        {
            for (std::uint16_t i = 0; i < count; i++)
            {
                if (pcie[i].Segment == d.Segment && pcie[i].Bus == d.Bus &&
                    pcie[i].Device == d.Device &&
                    pcie[i].Function == d.Function)
                {
                    return &pcie[i];
                }
            }
        }
        const OOB_INV_PCIE_RECORD* nvme = nullptr;
        for (std::uint16_t i = 0; i < count; i++)
        {
            if (pcie[i].ClassBase == 0x01 && pcie[i].ClassSub == 0x08)
            {
                if (nvme != nullptr)
                {
                    return nullptr; // ambiguous: more than one NVMe function
                }
                nvme = &pcie[i];
            }
        }
        return nvme;
    }

    // Marker interface plus the Protocol property bmcweb patch 0005 renders as
    // SupportedDeviceProtocols / NVMeControllerProperties. Everything this
    // daemon can describe is NVMe by construction (the blob's drive records
    // are NVMe-only and the synthesized path filters on class 0x0108); the
    // value reuses the Item.Drive DriveProtocol vocabulary because phosphor
    // defines no controller-side protocol enum.
    void addStorageController(const std::string& path)
    {
        auto ctrl = pub.add(
            path, "xyz.openbmc_project.Inventory.Item.StorageController");
        ctrl->register_property<std::string>(
            "Protocol",
            "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.NVMe");
        ctrl->initialize();
    }

    void publishDrive(std::uint16_t idx, const OOB_INV_DRIVE_RECORD& d,
                      const std::string& vendor)
    {
        const std::string id = "nvme" + std::to_string(idx);
        const std::string path = std::string(kStoragePath) + "/" + id;
        const std::string model = field(d.Model, sizeof(d.Model));
        const std::string serial = field(d.Serial, sizeof(d.Serial));

        {
            auto drive =
                pub.add(path, "xyz.openbmc_project.Inventory.Item.Drive");
            drive->register_property<std::string>(
                "Type",
                "xyz.openbmc_project.Inventory.Item.Drive.DriveType.SSD");
            drive->register_property<std::string>(
                "Protocol",
                "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.NVMe");
            drive->register_property<std::uint64_t>("Capacity",
                                                    d.CapacityBytes);
            std::uint8_t life = (d.PercentageUsed <= 100)
                                    ? static_cast<std::uint8_t>(100 - d.PercentageUsed)
                                    : 255;
            drive->register_property<std::uint8_t>(
                "PredictedMediaLifeLeftPercent", life);
            drive->initialize();
        }
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>(
                "PrettyName", model.empty() ? id : model);
            item->register_property("Present", true);
            item->initialize();
        }
        {
            auto state = pub.add(path, "xyz.openbmc_project.State.Drive");
            // Healthy unless SMART flagged a critical warning; not a RAID rebuild.
            state->register_property("Rebuilding", false);
            state->initialize();
        }
        addAsset(path, model, serial, vendor);

        if (!boardPath.empty())
        {
            auto assoc =
                pub.add(path, "xyz.openbmc_project.Association.Definitions");
            assoc->register_property(
                "Associations",
                std::vector<Association>{{"chassis", "drive", boardPath}});
            assoc->initialize();
        }
    }

    void publishController(const OOB_INV_DRIVE_RECORD& d,
                           const std::string& vendor,
                           std::vector<std::string>& seenSerials)
    {
        const std::string serial = field(d.Serial, sizeof(d.Serial));
        // One StorageController per physical controller (dedupe by serial).
        for (const auto& s : seenSerials)
        {
            if (s == serial)
            {
                return;
            }
        }
        seenSerials.push_back(serial);

        const std::string id =
            "controller_nvme" + std::to_string(seenSerials.size() - 1);
        const std::string path = std::string(kStoragePath) + "/" + id;
        const std::string model = field(d.Model, sizeof(d.Model));

        addStorageController(path);
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>("PrettyName",
                                                 model.empty() ? id : model);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(path, model, serial, vendor);
    }

    void publishPcie(const OOB_INV_PCIE_RECORD& p)
    {
        char loc[32];
        std::snprintf(loc, sizeof(loc), "pcie_%04x_%02x_%02x_%x", p.Segment,
                      p.Bus, p.Device, p.Function);
        const std::string path = std::string(kPcieRoot) + "/" + loc;

        {
            auto dev =
                pub.add(path, "xyz.openbmc_project.Inventory.Item.PCIeDevice");
            if (p.GenerationInUse >= 1 && p.GenerationInUse <= 5)
            {
                dev->register_property<std::string>(
                    "GenerationInUse", generationDbus(p.GenerationInUse));
            }
            if (p.GenerationMax >= 1 && p.GenerationMax <= 5)
            {
                dev->register_property<std::string>(
                    "GenerationSupported", generationDbus(p.GenerationMax));
            }
            // MUST be size_t, not a fixed-width type: bmcweb unpacks these as
            // `const size_t*`, which is 32-bit on this ARM BMC. Publishing
            // uint64 makes sdbusplus::unpackPropertiesNoThrow fail on the whole
            // property set, and the PCIeDevice GET then returns InternalError
            // with only the asset fields filled in. Using size_t here keeps the
            // wire type identical to whatever bmcweb was built against.
            //
            // bmcweb treats SIZE_MAX LanesInUse as "unknown" -> null, and 0
            // MaxLanes as "omit the field".
            dev->register_property<std::size_t>(
                "LanesInUse", p.LanesInUse
                                  ? static_cast<std::size_t>(p.LanesInUse)
                                  : std::numeric_limits<std::size_t>::max());
            dev->register_property<std::size_t>(
                "MaxLanes", static_cast<std::size_t>(p.LanesMax));
            dev->register_property<std::string>(
                "DeviceType",
                p.DeviceType == OOB_INV_PCIE_MULTI_FN
                    ? "xyz.openbmc_project.Inventory.Item.PCIeDevice.DeviceTypes."
                      "MultiFunction"
                    : "xyz.openbmc_project.Inventory.Item.PCIeDevice.DeviceTypes."
                      "SingleFunction");

            // Each enumerated function is its own object; expose it as Function0.
            char classCode[16];
            std::snprintf(classCode, sizeof(classCode), "0x%02x%02x%02x",
                          p.ClassBase, p.ClassSub, p.ClassProgIf);
            dev->register_property<std::string>("Function0DeviceId",
                                                hex16(p.DeviceId));
            dev->register_property<std::string>("Function0VendorId",
                                                hex16(p.VendorId));
            dev->register_property<std::string>("Function0SubsystemId",
                                                hex16(p.SubsysId));
            dev->register_property<std::string>("Function0SubsystemVendorId",
                                                hex16(p.SubsysVendorId));
            dev->register_property<std::string>("Function0ClassCode", classCode);
            dev->register_property<std::string>("Function0RevisionId",
                                                hex8(p.RevisionId));
            dev->register_property<std::string>(
                "Function0DeviceClass",
                deviceClassRedfish(p.ClassBase, p.ClassSub));
            dev->register_property<std::string>("Function0FunctionType",
                                                p.Function == 0 ? "Physical"
                                                                : "Virtual");
            dev->initialize();
        }
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>("PrettyName",
                                                 prettyNameFor(p, loc));
            item->register_property("Present", true);
            item->initialize();
        }
        // Model carries the device name when the table knows it; the numeric
        // ids are not lost, they remain as Function0VendorId/Function0DeviceId
        // on this object and on the Redfish PCIeFunction.
        const auto n = namesFor(p);
        addAsset(path,
                 n.device.empty() ? hex16(p.VendorId) + ":" + hex16(p.DeviceId)
                                  : n.device,
                 "", n.vendor);
    }

    // Names for a PCIe record, empty-filled when the table had no match.
    PciIdDb::Names namesFor(const OOB_INV_PCIE_RECORD& p) const
    {
        auto it = pciNames.find(PciIdDb::key(p.VendorId, p.DeviceId));
        return (it == pciNames.end()) ? PciIdDb::Names{} : it->second;
    }

    // "NVIDIA Corporation GK210GL [Tesla K80]" when both halves resolved,
    // degrading to whichever half did, and finally to the caller's fallback
    // (the BDF location) so this is never empty.
    std::string prettyNameFor(const OOB_INV_PCIE_RECORD& p,
                              const std::string& fallback) const
    {
        const auto n = namesFor(p);
        if (!n.vendor.empty() && !n.device.empty())
        {
            return n.vendor + " " + n.device;
        }
        if (!n.device.empty())
        {
            return n.device;
        }
        if (!n.vendor.empty())
        {
            return n.vendor + " " + hex16(p.DeviceId);
        }
        return fallback;
    }

    void addAsset(const std::string& path, const std::string& model,
                  const std::string& serial,
                  const std::string& manufacturer = "")
    {
        auto asset =
            pub.add(path, "xyz.openbmc_project.Inventory.Decorator.Asset");
        asset->register_property<std::string>("Manufacturer", manufacturer);
        asset->register_property<std::string>("Model", model);
        asset->register_property<std::string>("SerialNumber", serial);
        asset->register_property<std::string>("PartNumber", "");
        asset->register_property<std::string>("SparePartNumber", "");
        asset->register_property<std::string>("BuildDate", "");
        asset->initialize();
    }

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server& server;
    Publication pub;
    BlobWindow window;
    boost::asio::steady_timer pollTimer;
    std::unique_ptr<sdbusplus::bus::match_t> hostMatch;

    std::string boardPath;
    // Vendor/device names for the ids in `lastBlob`, resolved once per rebuild.
    std::map<std::uint32_t, PciIdDb::Names> pciNames;
    // The blob backing what is currently published, kept so late-arriving
    // context (the chassis path from the mapper) can be folded in by
    // re-publishing without waiting for the host to push again.
    std::vector<std::uint8_t> lastBlob;
    bool hostRunning = false;
    bool published = false;
    std::uint32_t lastCrc = 0;
};

} // namespace

int main()
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);

    // The mapper (and thus bmcweb) discovers our objects through InterfacesAdded
    // plus GetManagedObjects on this manager. Every inventory publisher owns one.
    server.add_manager(kInventoryRoot);

    // Host NIC MACs come from the board EEPROM, not the P2A blob: published
    // before (and regardless of) the P2A path coming up.
    auto nics = publishHostNics(server);

    Monitor monitor(io, conn, server);
    const bool haveP2a = monitor.start();
    if (!haveP2a && nics.empty())
    {
        // No P2A device (wrong kernel/DT) and no NICs — nothing to serve, but
        // do not crash-loop.
        return 0;
    }

    conn->request_name(kServiceName);
    io.run();
    return 0;
}
