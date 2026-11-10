// SPDX-License-Identifier: Apache-2.0
//
// gpu-smbpbi-sensor: publish an NVIDIA data-centre GPU's temperature and board
// power as phosphor sensors. The BMC reads them itself, over the GPU's SMBus
// Post-Box Interface (SMBPBI), so no host agent is involved.
//
// On the X570D4I-2T the PCIe slot's SMBus is wired to AST2500 I2C4, and a
// Tesla T4 there answers SMBPBI at 7-bit address 0x4F. That address was
// previously misconfigured as an onboard "Aux Temp" TMP75, which read
// -0.06 C. SMBPBI is a mailbox, not a register file:
//
//   0x5C  command   32-bit, SMBus block (count byte 4, then little-endian)
//                   [7:0] opcode  [15:8] arg1  [23:16] arg2
//                   [28:24] status  [31] INTR (request pending)
//   0x5D  data      32-bit result, same block framing
//
// To issue a request, write the command with status NULL and INTR set. Then
// poll the command register until the GPU clears INTR and posts a status;
// 0x1F means success, and the result is in the data register. Field layout,
// opcodes and status codes match NVIDIA's open-gpu-kernel-modules smbpbi.h.
//
// On a Turing T4 the post-box is serviced only while the host's NVIDIA driver
// is loaded. During POST, in an OS without the driver, or with the host hung,
// requests fail; ERR_I2C_ACCESS (0x07) was observed. The sensors' D-Bus state
// encodes that distinction, so phosphor-pid-control can tell "nothing to cool"
// from "can't see the GPU":
//
//   chassis off              -> Available=false   (swampd: not a failure)
//   on, GPU answering        -> value, Available=true, Functional=true
//   on, GPU not answering    -> NaN, Functional=false (swampd: failsafe)
//
// The objects are always published, never removed: swampd will not build a
// zone whose input sensor is missing from D-Bus.
//
// Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace
{

constexpr const char* kServiceName = "xyz.openbmc_project.GpuSmbpbiSensor";
constexpr const char* kSensorRoot = "/xyz/openbmc_project/sensors";

// ---- SMBPBI protocol ------------------------------------------------------
constexpr uint8_t kRegCommand = 0x5c;
constexpr uint8_t kRegData = 0x5d;
constexpr uint8_t kBlockLength = 4;

constexpr uint8_t kOpGetTemp = 0x02;    // signed 24.8 fixed point, deg C
constexpr uint8_t kOpGetExtTemp = 0x03; // same format, with the fraction
constexpr uint8_t kOpGetPower = 0x04;   // milliwatts
constexpr uint8_t kArgTempGpu = 0x00;   // ARG1_TEMP_GPU_0
constexpr uint8_t kArgPowerTotal = 0x00; // ARG1_POWER_TOTAL (board power)

constexpr uint8_t kIntrPending = 0x80; // command bit 31, in byte 3
constexpr uint8_t kStatusMask = 0x1f;  // command bits 28:24, in byte 3
constexpr uint8_t kStatusNull = 0x00;
constexpr uint8_t kStatusErrOpcode = 0x02;
constexpr uint8_t kStatusErrNotSupported = 0x08;
constexpr uint8_t kStatusAccepted = 0x1c;
constexpr uint8_t kStatusSuccess = 0x1f;
// Not SMBPBI codes: local outcomes, kept outside the 5-bit status space.
constexpr int kOutcomeIoError = 0x100;
constexpr int kOutcomeTimeout = 0x101;

// A request normally completes in well under 10 ms; give the GPU 200 ms.
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr int kMaxPolls = 40;

// Failed reads in a row (while powered) before a sensor is declared
// non-functional. One cycle per second, so about three seconds.
constexpr unsigned kFailuresBeforeNonFunctional = 3;

// Alarm de-assert hysteresis.
constexpr double kHysteresis = 1.0;

struct Options
{
    int bus = 4;
    uint8_t address = 0x4f;
    std::string name = "GPU";
    std::string chassis =
        "/xyz/openbmc_project/inventory/system/board/ASRock_Rack_X570D4I";
    std::chrono::milliseconds interval{1000};
    // Tesla T4 (nvidia-smi -q): max operating 85 C, slowdown 93 C.
    double warningHigh = 85.0;
    double criticalHigh = 93.0;
};

class I2cDevice
{
  public:
    I2cDevice(int bus, uint8_t address) : address(address)
    {
        const std::string path = "/dev/i2c-" + std::to_string(bus);
        fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
        {
            std::cerr << "cannot open " << path << ": " << std::strerror(errno)
                      << "\n";
        }
    }
    ~I2cDevice()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
    I2cDevice(const I2cDevice&) = delete;
    I2cDevice& operator=(const I2cDevice&) = delete;

    bool ok() const
    {
        return fd >= 0;
    }

    // SMBus block write of one 32-bit command word to 0x5C.
    bool writeCommand(uint8_t opcode, uint8_t arg1, uint8_t arg2)
    {
        std::array<uint8_t, 6> buf = {kRegCommand, kBlockLength, opcode,
                                      arg1,        arg2,         kIntrPending};
        std::array<i2c_msg, 1> msgs = {
            {{address, 0, static_cast<uint16_t>(buf.size()), buf.data()}}};
        return transfer(msgs.data(), msgs.size());
    }

    // SMBus block read of one 32-bit register (count byte + 4 data bytes).
    std::optional<std::array<uint8_t, 4>> readBlock(uint8_t reg)
    {
        std::array<uint8_t, 1 + kBlockLength> buf{};
        std::array<i2c_msg, 2> msgs = {{
            {address, 0, 1, &reg},
            {address, I2C_M_RD, static_cast<uint16_t>(buf.size()), buf.data()},
        }};
        if (!transfer(msgs.data(), msgs.size()) || buf[0] != kBlockLength)
        {
            return std::nullopt;
        }
        return std::array<uint8_t, 4>{buf[1], buf[2], buf[3], buf[4]};
    }

  private:
    bool transfer(i2c_msg* msgs, size_t count)
    {
        if (fd < 0)
        {
            return false;
        }
        i2c_rdwr_ioctl_data data{msgs, static_cast<uint32_t>(count)};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        return ::ioctl(fd, I2C_RDWR, &data) >= 0;
    }

    int fd = -1;
    uint16_t address;
};

uint32_t le32(const std::array<uint8_t, 4>& b)
{
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

// ---- D-Bus sensor ---------------------------------------------------------
using Association = std::tuple<std::string, std::string, std::string>;

class Sensor
{
  public:
    Sensor(sdbusplus::asio::object_server& server, std::string displayName,
           const std::string& type, const std::string& unit, double minValue,
           double maxValue, std::optional<double> warningHigh,
           std::optional<double> criticalHigh, const std::string& chassis) :
        server(server), displayName(std::move(displayName)),
        warningHighValue(warningHigh), criticalHighValue(criticalHigh)
    {
        std::string leaf = this->displayName;
        for (char& c : leaf)
        {
            if (c == ' ')
            {
                c = '_';
            }
        }
        const std::string path = std::string(kSensorRoot) + "/" + type + "/" +
                                 leaf;

        value = server.add_interface(path, "xyz.openbmc_project.Sensor.Value");
        value->register_property("Value", nan());
        value->register_property("MaxValue", maxValue);
        value->register_property("MinValue", minValue);
        value->register_property("Unit", unit);
        value->initialize();

        if (warningHigh)
        {
            warning = server.add_interface(
                path, "xyz.openbmc_project.Sensor.Threshold.Warning");
            warning->register_property("WarningHigh", *warningHigh);
            warning->register_property("WarningAlarmHigh", false);
            warning->initialize();
        }
        if (criticalHigh)
        {
            critical = server.add_interface(
                path, "xyz.openbmc_project.Sensor.Threshold.Critical");
            critical->register_property("CriticalHigh", *criticalHigh);
            critical->register_property("CriticalAlarmHigh", false);
            critical->initialize();
        }

        availability = server.add_interface(
            path, "xyz.openbmc_project.State.Decorator.Availability");
        availability->register_property("Available", false);
        availability->initialize();

        operational = server.add_interface(
            path, "xyz.openbmc_project.State.Decorator.OperationalStatus");
        operational->register_property("Functional", true);
        operational->initialize();

        association = server.add_interface(
            path, "xyz.openbmc_project.Association.Definitions");
        association->register_property(
            "Associations",
            std::vector<Association>{{"chassis", "all_sensors", chassis}});
        association->initialize();
    }

    ~Sensor()
    {
        for (auto* iface :
             {&value, &warning, &critical, &availability, &operational,
              &association})
        {
            if (*iface)
            {
                server.remove_interface(*iface);
            }
        }
    }
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;

    // Chassis is off: the GPU is unpowered, so there is nothing to read.
    void powerOff()
    {
        failures = 0;
        setValue(nan());
        setAlarms(nan());
        availability->set_property("Available", false);
        isFunctional = true;
        operational->set_property("Functional", true);
    }

    void update(std::optional<double> reading, const std::string& why)
    {
        availability->set_property("Available", true);
        if (reading)
        {
            failures = 0;
            setValue(*reading);
            setAlarms(*reading);
            setFunctional(true, "");
            return;
        }
        if (++failures >= kFailuresBeforeNonFunctional)
        {
            setValue(nan());
            setAlarms(nan());
            setFunctional(false, why);
        }
    }

  private:
    static double nan()
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // set_property() emits PropertiesChanged when values compare unequal, and
    // NaN never equals itself, so skip NaN -> NaN explicitly.
    void setValue(double v)
    {
        if (std::isnan(v) && std::isnan(lastValue))
        {
            return;
        }
        lastValue = v;
        value->set_property("Value", v);
    }

    void setAlarms(double v)
    {
        auto evaluate = [&](bool& state, std::optional<double> limit) {
            if (!limit || std::isnan(v))
            {
                state = false;
            }
            else if (v >= *limit)
            {
                state = true;
            }
            else if (v < *limit - kHysteresis)
            {
                state = false;
            }
            return state;
        };
        if (warning)
        {
            warning->set_property("WarningAlarmHigh",
                                  evaluate(warningAsserted, warningHighValue));
        }
        if (critical)
        {
            critical->set_property(
                "CriticalAlarmHigh",
                evaluate(criticalAsserted, criticalHighValue));
        }
    }

    void setFunctional(bool functional, const std::string& why)
    {
        if (functional != isFunctional)
        {
            if (functional)
            {
                std::cerr << displayName << ": reading again\n";
            }
            else
            {
                std::cerr << displayName << ": no reading (" << why
                          << "); marked non-functional\n";
            }
        }
        isFunctional = functional;
        operational->set_property("Functional", functional);
    }

    sdbusplus::asio::object_server& server;
    std::string displayName;
    std::optional<double> warningHighValue;
    std::optional<double> criticalHighValue;
    std::shared_ptr<sdbusplus::asio::dbus_interface> value;
    std::shared_ptr<sdbusplus::asio::dbus_interface> warning;
    std::shared_ptr<sdbusplus::asio::dbus_interface> critical;
    std::shared_ptr<sdbusplus::asio::dbus_interface> availability;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operational;
    std::shared_ptr<sdbusplus::asio::dbus_interface> association;
    unsigned failures = 0;
    bool isFunctional = true;
    bool warningAsserted = false;
    bool criticalAsserted = false;
    double lastValue = std::numeric_limits<double>::quiet_NaN();
};

std::string describe(int outcome)
{
    if (outcome == kOutcomeIoError)
    {
        return "no I2C response";
    }
    if (outcome == kOutcomeTimeout)
    {
        return "request timed out";
    }
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "SMBPBI status 0x%02x", outcome);
    return buf.data();
}

// ---- poll loop ------------------------------------------------------------
class Monitor
{
  public:
    using Done = std::function<void(std::optional<uint32_t>, int outcome)>;

    Monitor(boost::asio::io_context& io,
            std::shared_ptr<sdbusplus::asio::connection> conn,
            sdbusplus::asio::object_server& server, const Options& opts) :
        io(io), conn(std::move(conn)), opts(opts),
        device(opts.bus, opts.address), cycleTimer(io), pollTimer(io),
        temperature(server, opts.name + " Temp", "temperature",
                    "xyz.openbmc_project.Sensor.Value.Unit.DegreesC", -128.0,
                    127.0, opts.warningHigh, opts.criticalHigh, opts.chassis),
        power(server, opts.name + " Power", "power",
              "xyz.openbmc_project.Sensor.Value.Unit.Watts", 0.0, 100.0,
              std::nullopt, std::nullopt, opts.chassis)
    {}

    void start()
    {
        watchChassisPower();
        cycle();
    }

  private:
    void watchChassisPower()
    {
        powerMatch = std::make_unique<sdbusplus::bus::match_t>(
            *conn,
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',"
            "arg0='xyz.openbmc_project.State.Chassis',"
            "path='/xyz/openbmc_project/state/chassis0'",
            [this](sdbusplus::message_t& m) {
                std::string iface;
                std::vector<std::pair<std::string, std::variant<std::string>>>
                    props;
                try
                {
                    m.read(iface, props);
                }
                catch (const std::exception&)
                {
                    return;
                }
                for (const auto& [name, val] : props)
                {
                    if (name == "CurrentPowerState")
                    {
                        if (const auto* s = std::get_if<std::string>(&val))
                        {
                            setPower(*s);
                        }
                    }
                }
            });
        queryChassisPower();
    }

    // A chassis already on when we start never transitions again, so read the
    // state once, retrying until power control answers.
    void queryChassisPower()
    {
        conn->async_method_call(
            [this](const boost::system::error_code& ec,
                   const std::variant<std::string>& v) {
                const auto* s = ec ? nullptr : std::get_if<std::string>(&v);
                if (s != nullptr)
                {
                    setPower(*s);
                    return;
                }
                auto t = std::make_shared<boost::asio::steady_timer>(io);
                t->expires_after(std::chrono::seconds(5));
                t->async_wait([this, t](const boost::system::error_code& e) {
                    if (!e)
                    {
                        queryChassisPower();
                    }
                });
            },
            "xyz.openbmc_project.State.Chassis",
            "/xyz/openbmc_project/state/chassis0",
            "org.freedesktop.DBus.Properties", "Get",
            "xyz.openbmc_project.State.Chassis", "CurrentPowerState");
    }

    void setPower(std::string_view state)
    {
        const bool on = state.ends_with(".On");
        if (!powerKnown || on != powerOn)
        {
            std::cerr << "chassis power " << (on ? "on" : "off") << "\n";
        }
        powerKnown = true;
        powerOn = on;
        if (!on)
        {
            temperature.powerOff();
            power.powerOff();
        }
    }

    void cycle()
    {
        if (!powerOn)
        {
            temperature.powerOff();
            power.powerOff();
            schedule();
            return;
        }
        if (!device.ok())
        {
            // Powered but blind: report a failure, so the fan zone fails safe
            // instead of treating the GPU as absent.
            temperature.update(std::nullopt, "I2C bus unavailable");
            power.update(std::nullopt, "I2C bus unavailable");
            schedule();
            return;
        }
        request(tempOpcode, kArgTempGpu,
                [this](std::optional<uint32_t> raw, int outcome) {
                    if (!raw && tempOpcode == kOpGetExtTemp &&
                        (outcome == kStatusErrOpcode ||
                         outcome == kStatusErrNotSupported))
                    {
                        std::cerr << "extended temperature unsupported; "
                                     "using GET_TEMP\n";
                        tempOpcode = kOpGetTemp;
                    }
                    std::optional<double> celsius;
                    if (raw)
                    {
                        // Signed 24.8 fixed point.
                        celsius = static_cast<int32_t>(*raw) / 256.0;
                    }
                    if (powerOn)
                    {
                        temperature.update(celsius, describe(outcome));
                    }
                    request(kOpGetPower, kArgPowerTotal,
                            [this](std::optional<uint32_t> mw, int why) {
                                std::optional<double> watts;
                                if (mw)
                                {
                                    watts = *mw / 1000.0;
                                }
                                if (powerOn)
                                {
                                    power.update(watts, describe(why));
                                }
                                schedule();
                            });
                });
    }

    void schedule()
    {
        cycleTimer.expires_after(opts.interval);
        cycleTimer.async_wait([this](const boost::system::error_code& ec) {
            if (!ec)
            {
                cycle();
            }
        });
    }

    void request(uint8_t opcode, uint8_t arg1, Done done)
    {
        if (!device.writeCommand(opcode, arg1, 0))
        {
            done(std::nullopt, kOutcomeIoError);
            return;
        }
        poll(opcode, kMaxPolls, std::move(done));
    }

    void poll(uint8_t opcode, int remaining, Done done)
    {
        pollTimer.expires_after(kPollInterval);
        pollTimer.async_wait([this, opcode, remaining, done = std::move(done)](
                                 const boost::system::error_code& ec) mutable {
            if (ec)
            {
                return;
            }
            const auto cmd = device.readBlock(kRegCommand);
            if (!cmd)
            {
                done(std::nullopt, kOutcomeIoError);
                return;
            }
            const uint8_t status = (*cmd)[3] & kStatusMask;
            const bool pending = ((*cmd)[3] & kIntrPending) != 0;
            const bool ours = (*cmd)[0] == opcode;
            // ERR_BUSY is a rejection, not progress: it fails this request and
            // the next cycle retries.
            const bool inFlight = pending || status == kStatusNull ||
                                  status == kStatusAccepted;
            if (ours && !inFlight)
            {
                if (status != kStatusSuccess)
                {
                    done(std::nullopt, status);
                    return;
                }
                const auto data = device.readBlock(kRegData);
                if (!data)
                {
                    done(std::nullopt, kOutcomeIoError);
                    return;
                }
                done(le32(*data), status);
                return;
            }
            if (remaining <= 1)
            {
                done(std::nullopt, ours ? kOutcomeTimeout : status);
                return;
            }
            poll(opcode, remaining - 1, std::move(done));
        });
    }

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    Options opts;
    I2cDevice device;
    boost::asio::steady_timer cycleTimer;
    boost::asio::steady_timer pollTimer;
    Sensor temperature;
    Sensor power;
    std::unique_ptr<sdbusplus::bus::match_t> powerMatch;
    uint8_t tempOpcode = kOpGetExtTemp;
    bool powerKnown = false;
    // Assume on until power control says otherwise. If it never answers, we
    // keep polling: a silent GPU then reads as a failure (FAN3 failsafe),
    // never as "chassis off" (FAN3 at its minimum).
    bool powerOn = true;
};

[[noreturn]] void usage(const char* argv0)
{
    std::cerr << "usage: " << argv0
              << " [--bus N] [--address 0xNN] [--name PREFIX]"
                 " [--chassis PATH] [--interval-ms N]"
                 " [--warning-high C] [--critical-high C]\n";
    std::exit(2);
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc)
        {
            usage(argv[0]);
        }
        const char* v = argv[++i];
        if (arg == "--bus")
        {
            o.bus = static_cast<int>(std::strtol(v, nullptr, 0));
        }
        else if (arg == "--address")
        {
            o.address = static_cast<uint8_t>(std::strtol(v, nullptr, 0));
        }
        else if (arg == "--name")
        {
            o.name = v;
        }
        else if (arg == "--chassis")
        {
            o.chassis = v;
        }
        else if (arg == "--interval-ms")
        {
            o.interval = std::chrono::milliseconds(std::strtol(v, nullptr, 0));
        }
        else if (arg == "--warning-high")
        {
            o.warningHigh = std::strtod(v, nullptr);
        }
        else if (arg == "--critical-high")
        {
            o.criticalHigh = std::strtod(v, nullptr);
        }
        else
        {
            usage(argv[0]);
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    const Options opts = parse(argc, argv);

    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);
    // bmcweb and the mapper enumerate sensors via GetManagedObjects.
    server.add_manager(kSensorRoot);

    Monitor monitor(io, conn, server, opts);
    conn->request_name(kServiceName);
    monitor.start();
    io.run();
    return 0;
}
