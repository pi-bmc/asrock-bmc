// supermicro-psu-monitor
//
// Consolidated monitor for the Supermicro PWS-505P-1H PSU on the ASRock
// X570D4I-2T (PSU_SMB1 = BMC i2c-2, addr 0x38).
//
// This PSU is NOT a real PMBus register device: every SMBus read (byte/word/
// block, with or without PEC) returns a flat IPMI-FRU memory image indexed by
// command-code-as-offset (proven: READ_VOUT@0x8B and READ_IOUT@0x8C share a
// byte; PMBUS_REVISION@0x98 reads garbage; PEC NAKs; the kernel pmbus driver
// fails capability ID). So neither the kernel pmbus/hwmon stack nor
// phosphor-psu-monitor (PMBus + IBM-CFF oriented) can drive it.
//
// Instead, live telemetry lives in the FRU internal-use area + Supermicro
// vendor registers, decoded here directly:
//   0x09  temperature  degC (direct)
//   0x0A  fan1         RPM = raw*30/0.262     0x0B fan2 (0 => absent)
//   0x0C  DC status    0x01 = good
//   0x14  AC current   A = raw/16
//   0xF4  AC voltage   V (direct)
//   0xF5/0xF6  AC power W (little-endian 16-bit)
// Identity (Manufacturer/Model/Serial/Version) is parsed from the standard
// IPMI FRU Product Info area.
//
// It publishes, from one daemon:
//   * six sensors (temperature/fan_tach/voltage/current/power) with thresholds
//     and a chassis association (so they list under the chassis in Redfish/IPMI)
//   * a PowerSupply inventory item (Item.PowerSupply + Asset + Revision +
//     OperationalStatus) that OWNS the object
//   * the chassis "powered_by" association, so the PSU appears in Redfish
//     PowerSubsystem/PowerSupplies (and the WebUI "Power supplies" card).

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// ---- inline Linux SMBus ioctl bits (avoid libi2c / header-version churn) ----
#define I2C_SLAVE 0x0703
#define I2C_SMBUS 0x0720
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_BYTE_DATA 2
union i2c_smbus_data
{
    uint8_t byte;
    uint16_t word;
    uint8_t block[34];
};
struct i2c_smbus_ioctl_data
{
    uint8_t read_write;
    uint8_t command;
    uint32_t size;
    union i2c_smbus_data* data;
};

namespace
{
constexpr int psuBus = 2;
constexpr uint8_t psuAddr = 0x38;
constexpr int pollSeconds = 5;

const std::string chassisPath =
    "/xyz/openbmc_project/inventory/system/board/ASRock_Rack_X570D4I";
const std::string psuInvPath =
    "/xyz/openbmc_project/inventory/system/powersupply/psu0";
const std::string sensorRoot = "/xyz/openbmc_project/sensors/";

using Association = std::tuple<std::string, std::string, std::string>;
using DBusIface = std::shared_ptr<sdbusplus::asio::dbus_interface>;

// -------------------------------- I2C ---------------------------------------
int openPsu()
{
    std::string dev = "/dev/i2c-" + std::to_string(psuBus);
    int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    if (::ioctl(fd, I2C_SLAVE, psuAddr) < 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

int readByte(int fd, uint8_t cmd)
{
    i2c_smbus_data d{};
    i2c_smbus_ioctl_data args{I2C_SMBUS_READ, cmd, I2C_SMBUS_BYTE_DATA, &d};
    if (::ioctl(fd, I2C_SMBUS, &args) < 0)
    {
        return -1;
    }
    return d.byte;
}

// Read a standard IPMI FRU type/length string field, advancing 'off'.
std::string readFruField(int fd, int& off)
{
    int tl = readByte(fd, static_cast<uint8_t>(off++));
    if (tl < 0 || tl == 0xC1) // 0xC1 = end-of-fields marker
    {
        return {};
    }
    int len = tl & 0x3F;
    std::string s;
    for (int i = 0; i < len; ++i)
    {
        int b = readByte(fd, static_cast<uint8_t>(off++));
        if (b < 0)
        {
            break;
        }
        s.push_back(static_cast<char>(b));
    }
    return s;
}

struct Vpd
{
    std::string manufacturer = "Supermicro";
    std::string model = "PWS-505P-1H";
    std::string partNumber = "PWS-505P-1H";
    std::string version;
    std::string serial;
};

// Parse the FRU Product Info Area (offset from common header byte 0x04, x8).
Vpd readVpd(int fd)
{
    Vpd v;
    int prodOff = readByte(fd, 0x04);
    if (prodOff <= 0)
    {
        return v;
    }
    int off = prodOff * 8 + 3; // skip format-version, length, language bytes
    std::string mfr = readFruField(fd, off);
    std::string name = readFruField(fd, off);
    std::string part = readFruField(fd, off);
    std::string ver = readFruField(fd, off);
    std::string ser = readFruField(fd, off);
    if (!mfr.empty())
    {
        v.manufacturer = mfr;
    }
    if (!name.empty())
    {
        v.model = name;
    }
    if (!part.empty())
    {
        v.partNumber = part;
    }
    v.version = ver;
    v.serial = ser;
    return v;
}

// ------------------------------- Sensor -------------------------------------
struct Sensor
{
    DBusIface value;
    DBusIface opStatus;
    DBusIface avail;
    DBusIface warn; // may be null
    DBusIface crit; // may be null
    double warnHigh = std::numeric_limits<double>::quiet_NaN();
    double critHigh = std::numeric_limits<double>::quiet_NaN();
    double critLow = std::numeric_limits<double>::quiet_NaN();

    void update(double v, bool functional)
    {
        value->set_property("Value", v);
        avail->set_property("Available", functional);
        opStatus->set_property("Functional", functional);
        if (warn && !std::isnan(warnHigh))
        {
            warn->set_property("WarningAlarmHigh", functional && v > warnHigh);
        }
        if (crit)
        {
            if (!std::isnan(critHigh))
            {
                crit->set_property("CriticalAlarmHigh",
                                   functional && v > critHigh);
            }
            if (!std::isnan(critLow))
            {
                crit->set_property("CriticalAlarmLow",
                                   functional && v < critLow);
            }
        }
    }

    void markUnavailable()
    {
        value->set_property("Value",
                            std::numeric_limits<double>::quiet_NaN());
        avail->set_property("Available", false);
        opStatus->set_property("Functional", false);
    }
};

std::shared_ptr<Sensor>
    makeSensor(sdbusplus::asio::object_server& obj, const std::string& ns,
               const std::string& name, const std::string& unit, double minV,
               double maxV, double warnHigh, double critHigh, double critLow)
{
    auto s = std::make_shared<Sensor>();
    std::string path = sensorRoot + ns + "/" + name;

    s->value = obj.add_interface(path, "xyz.openbmc_project.Sensor.Value");
    s->value->register_property(
        "Value", std::numeric_limits<double>::quiet_NaN());
    s->value->register_property("MaxValue", maxV);
    s->value->register_property("MinValue", minV);
    s->value->register_property(
        "Unit", std::string("xyz.openbmc_project.Sensor.Value.Unit.") + unit);
    s->value->initialize();

    s->avail =
        obj.add_interface(path, "xyz.openbmc_project.State.Decorator.Availability");
    s->avail->register_property("Available", true);
    s->avail->initialize();

    s->opStatus = obj.add_interface(
        path, "xyz.openbmc_project.State.Decorator.OperationalStatus");
    s->opStatus->register_property("Functional", true);
    s->opStatus->initialize();

    if (!std::isnan(warnHigh))
    {
        s->warn = obj.add_interface(
            path, "xyz.openbmc_project.Sensor.Threshold.Warning");
        s->warn->register_property("WarningHigh", warnHigh);
        s->warn->register_property("WarningAlarmHigh", false);
        s->warn->initialize();
        s->warnHigh = warnHigh;
    }
    if (!std::isnan(critHigh) || !std::isnan(critLow))
    {
        s->crit = obj.add_interface(
            path, "xyz.openbmc_project.Sensor.Threshold.Critical");
        if (!std::isnan(critHigh))
        {
            s->crit->register_property("CriticalHigh", critHigh);
            s->crit->register_property("CriticalAlarmHigh", false);
            s->critHigh = critHigh;
        }
        if (!std::isnan(critLow))
        {
            s->crit->register_property("CriticalLow", critLow);
            s->crit->register_property("CriticalAlarmLow", false);
            s->critLow = critLow;
        }
        s->crit->initialize();
    }

    // Associate the sensor with the chassis (so it lists under the chassis
    // Sensors collection) and with the PSU inventory (so it groups under it).
    auto assoc =
        obj.add_interface(path, "xyz.openbmc_project.Association.Definitions");
    assoc->register_property(
        "Associations",
        std::vector<Association>{{"chassis", "all_sensors", chassisPath},
                                 {"inventory", "sensors", psuInvPath}});
    assoc->initialize();

    return s;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

int main()
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);
    // bmcweb reads both sensor data and inventory-item interfaces via
    // GetManagedObjects called at /xyz/openbmc_project/sensors and
    // /xyz/openbmc_project/inventory respectively. Without an ObjectManager at
    // each path those calls fail: individual sensor reads return InternalError
    // and the PSU inventory (Item.PowerSupply/Asset) is invisible to bmcweb.
    // Every dbus-sensors daemon registers these; do the same.
    server.add_manager("/xyz/openbmc_project/sensors");
    server.add_manager("/xyz/openbmc_project/inventory");

    // ---- one-shot: read identity from the FRU product area ----
    Vpd vpd;
    if (int fd = openPsu(); fd >= 0)
    {
        vpd = readVpd(fd);
        ::close(fd);
    }

    // ---- PowerSupply inventory (this daemon owns the object) ----
    auto item =
        server.add_interface(psuInvPath, "xyz.openbmc_project.Inventory.Item");
    item->register_property("Present", true);
    item->register_property("PrettyName", std::string("Supermicro PWS-505P-1H"));
    item->initialize();

    server.add_interface(psuInvPath,
                         "xyz.openbmc_project.Inventory.Item.PowerSupply")
        ->initialize();

    auto asset = server.add_interface(
        psuInvPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    asset->register_property("Manufacturer", vpd.manufacturer);
    asset->register_property("Model", vpd.model);
    asset->register_property("PartNumber", vpd.partNumber);
    asset->register_property("SerialNumber", vpd.serial);
    asset->register_property("BuildDate", std::string(""));
    asset->register_property("SparePartNumber", std::string(""));
    asset->initialize();

    auto rev = server.add_interface(
        psuInvPath, "xyz.openbmc_project.Inventory.Decorator.Revision");
    rev->register_property("Version", vpd.version);
    rev->initialize();

    auto psuOp = server.add_interface(
        psuInvPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");
    psuOp->register_property("Functional", true);
    psuOp->initialize();

    // chassis <- powered_by -> this PSU (drives Redfish PowerSubsystem)
    auto psuAssoc = server.add_interface(
        psuInvPath, "xyz.openbmc_project.Association.Definitions");
    psuAssoc->register_property(
        "Associations",
        std::vector<Association>{{"powering", "powered_by", chassisPath}});
    psuAssoc->initialize();

    // ---- sensors ----
    auto temp = makeSensor(server, "temperature", "PSU_Temp", "DegreesC", 0, 127,
                           85, 94, kNaN);
    auto fan1 = makeSensor(server, "fan_tach", "PSU_Fan_1", "RPMS", 0, 30000,
                           kNaN, kNaN, kNaN);
    auto fan2 = makeSensor(server, "fan_tach", "PSU_Fan_2", "RPMS", 0, 30000,
                           kNaN, kNaN, kNaN);
    auto vin = makeSensor(server, "voltage", "PSU_Input_Voltage", "Volts", 0, 300,
                          kNaN, 264, 90);
    auto iin = makeSensor(server, "current", "PSU_Input_Current", "Amperes", 0, 16,
                          kNaN, kNaN, kNaN);
    auto pin = makeSensor(server, "power", "PSU_Input_Power", "Watts", 0, 600,
                          kNaN, 525, kNaN);

    // Tag PSU AC-input power as the chassis TotalPower. bmcweb sources
    // Redfish Chassis/<id>/EnvironmentMetrics -> PowerWatts.Reading from the
    // single chassis-associated power sensor that implements
    // xyz.openbmc_project.Sensor.Purpose with Purpose == TotalPower (see
    // bmcweb environment_metrics.hpp getPowerWatts). The web UI's power page
    // (usePowerControl -> PowerWatts.Reading) reads exactly that, so without
    // this interface "Power consumption" renders as Not available. On this
    // single-PSU box the PSU AC input power is the whole-chassis draw. The
    // required Sensor.Value + all_sensors/chassis association are already set
    // by makeSensor above; this only adds the Purpose tag.
    auto pinPurpose = server.add_interface(
        sensorRoot + "power/PSU_Input_Power",
        "xyz.openbmc_project.Sensor.Purpose");
    pinPurpose->register_property(
        "Purpose",
        std::vector<std::string>{
            "xyz.openbmc_project.Sensor.Purpose.SensorPurpose.TotalPower"});
    pinPurpose->initialize();

    // ---- poll loop ----
    auto timer = std::make_shared<boost::asio::steady_timer>(io);
    std::function<void()> poll = [&, timer]() {
        int fd = openPsu();
        bool good = false;
        if (fd >= 0)
        {
            int status = readByte(fd, 0x0C);
            good = (status == 0x01);
            bool present = (status >= 0);

            psuOp->set_property("Functional", good);

            int t = readByte(fd, 0x09);
            if (t > 0 && t < 125)
            {
                temp->update(t, present);
            }

            int f1 = readByte(fd, 0x0A);
            if (f1 > 0)
            {
                fan1->update(f1 * 30.0 / 0.262, present);
            }
            else if (f1 == 0)
            {
                fan1->markUnavailable();
            }
            int f2 = readByte(fd, 0x0B);
            if (f2 > 0)
            {
                fan2->update(f2 * 30.0 / 0.262, present);
            }
            else
            {
                fan2->markUnavailable(); // second fan absent on this unit
            }

            int cur = readByte(fd, 0x14);
            if (cur >= 0)
            {
                iin->update(cur / 16.0, present);
            }

            int v = readByte(fd, 0xF4);
            if (v >= 0)
            {
                vin->update(v, present);
            }

            int lo = readByte(fd, 0xF5);
            int hi = readByte(fd, 0xF6);
            if (lo >= 0 && hi >= 0)
            {
                pin->update(lo + hi * 256, present);
            }
            ::close(fd);
        }

        if (fd < 0)
        {
            // PSU not answering: mark everything unavailable.
            psuOp->set_property("Functional", false);
            for (auto* s : {&temp, &fan1, &fan2, &vin, &iin, &pin})
            {
                (*s)->markUnavailable();
            }
        }

        timer->expires_after(std::chrono::seconds(pollSeconds));
        timer->async_wait([&](const boost::system::error_code& ec) {
            if (!ec)
            {
                poll();
            }
        });
    };
    poll();

    // Claim the well-known name only after the objects exist and have their
    // first readings, so clients never see a half-populated service.
    conn->request_name("xyz.openbmc_project.SupermicroPSU");

    io.run();
    return 0;
}
