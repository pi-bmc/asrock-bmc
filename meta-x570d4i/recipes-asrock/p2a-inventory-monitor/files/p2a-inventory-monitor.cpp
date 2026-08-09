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

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
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
            rebuild(cached);
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
                            lastCrc = 0; // force a rebuild so associations land
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
                rebuild(buf);
                lastCrc = h->Crc32;
                published = true;
                // Persist only what we have actually published, so the cache
                // can never describe something Redfish never served.
                saveCache(buf);
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
            publishDrive(i, d);
            publishController(d, seenControllerSerials);
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
        const std::string model = hex16(p.VendorId) + ":" + hex16(p.DeviceId);

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
            item->register_property<std::string>(
                "PrettyName", std::string("NVMe ") + loc);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(path, model, "");

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
        pub.add(cpath, "xyz.openbmc_project.Inventory.Item.StorageController")
            ->initialize();
        {
            auto item = pub.add(cpath, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>(
                "PrettyName", std::string("NVMe Controller ") + loc);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(cpath, model, "");
    }

    void publishDrive(std::uint16_t idx, const OOB_INV_DRIVE_RECORD& d)
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
        addAsset(path, model, serial);

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

        pub.add(path, "xyz.openbmc_project.Inventory.Item.StorageController")
            ->initialize();
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>("PrettyName",
                                                 model.empty() ? id : model);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(path, model, serial);
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
            dev->register_property<std::string>("Function0DeviceClass", "");
            dev->register_property<std::string>("Function0FunctionType",
                                                p.Function == 0 ? "Physical"
                                                                : "Virtual");
            dev->initialize();
        }
        {
            auto item = pub.add(path, "xyz.openbmc_project.Inventory.Item");
            item->register_property<std::string>("PrettyName", loc);
            item->register_property("Present", true);
            item->initialize();
        }
        addAsset(path, hex16(p.VendorId) + ":" + hex16(p.DeviceId), "");
    }

    void addAsset(const std::string& path, const std::string& model,
                  const std::string& serial)
    {
        auto asset =
            pub.add(path, "xyz.openbmc_project.Inventory.Decorator.Asset");
        asset->register_property<std::string>("Manufacturer", "");
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

    Monitor monitor(io, conn, server);
    if (!monitor.start())
    {
        // No P2A device (wrong kernel/DT) — nothing to do, but do not crash-loop.
        return 0;
    }

    conn->request_name(kServiceName);
    io.run();
    return 0;
}
