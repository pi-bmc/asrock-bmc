#!/bin/sh
# NCT6779 -> ExternalSensor bridge for X570D4I-2T
#
# The board's fans are 3-wire and controlled by the host BIOS SmartFan through
# the Nuvoton NCT6779 SuperIO; the BMC's AST2500 PWM/tach is not in the fan
# path. The NCT6779 is reachable read-only from the BMC at i2c-1 0x2d
# (nct6775-i2c -> /sys/class/hwmon), but entity-manager rejects a "NCT6779"
# sensor type, so this small daemon polls the hwmon sysfs and pushes the
# readings into the ExternalSensor objects entity-manager creates from
# x570d4i2t.json:
#   - SuperIO temperatures (SYSTIN / CPUTIN / AUXTIN / PCH_*).
#   - Fan PWM duty (pwm1/pwm2, 0-255 -> 0-100 %), i.e. the SmartFan output the
#     host is driving the fans with. (fanN tach is unwired -> no RPM.)

set -u

I2C_BUS=1
I2C_ADDR=0x2d
DEV_DIR="/sys/bus/i2c/devices/1-002d"
DRV_DIR="/sys/bus/i2c/drivers/nct6775-i2c"
INTERVAL=5

# hwmon temp input -> ExternalSensor temperature object name (nct6775 labels for
# the NCT6779D family: temp1=SYSTIN temp2=CPUTIN temp3=AUXTIN0
# temp9=PCH_CPU_TEMP temp10=PCH_MCH_TEMP).
TEMP_PATHS="\
temp1_input:SYSTIN \
temp2_input:CPUTIN \
temp3_input:AUXTIN \
temp9_input:PCH_CPU_Temp \
temp10_input:PCH_MCH_Temp"

# hwmon pwm (0-255) -> ExternalSensor "percent" object name (Units "Percent"
# maps to /xyz/openbmc_project/sensors/percent/, not /utilization/).
PWM_PATHS="\
pwm1:Fan1_Duty \
pwm2:Fan2_Duty"

find_hwmon() {
    for h in /sys/class/hwmon/hwmon*; do
        if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nct6779" ]; then
            echo "$h"
            return 0
        fi
    done
    return 1
}

ensure_device() {
    # Bind the driver if the i2c client exists but is unbound (e.g. the chip
    # reappeared after a host power transition); otherwise instantiate it.
    if [ -e "$DEV_DIR" ]; then
        if [ ! -L "$DEV_DIR/driver" ]; then
            echo "1-002d" > "$DRV_DIR/bind" 2>/dev/null || true
        fi
        return 0
    fi
    echo "nct6779 $I2C_ADDR" > "/sys/bus/i2c/devices/i2c-$I2C_BUS/new_device" 2>/dev/null || true
    sleep 1
}

set_value() {
    busctl set-property xyz.openbmc_project.ExternalSensor \
        "$1" xyz.openbmc_project.Sensor.Value Value d "$2" 2>/dev/null || true
}

publish() {
    HW="$1"
    for entry in $TEMP_PATHS; do
        sysfs="${entry%%:*}"; name="${entry##*:}"
        raw=$(cat "$HW/$sysfs" 2>/dev/null) || continue
        [ -z "$raw" ] && continue
        # Open/unwired diodes saturate to +/-127000 — skip so the sensor
        # doesn't latch a bogus reading.
        if [ "$raw" -ge 120000 ] || [ "$raw" -le -120000 ]; then continue; fi
        val=$(awk -v r="$raw" 'BEGIN{printf "%.3f", r/1000}')
        set_value "/xyz/openbmc_project/sensors/temperature/$name" "$val"
    done
    for entry in $PWM_PATHS; do
        sysfs="${entry%%:*}"; name="${entry##*:}"
        raw=$(cat "$HW/$sysfs" 2>/dev/null) || continue
        [ -z "$raw" ] && continue
        val=$(awk -v r="$raw" 'BEGIN{printf "%.1f", r*100/255}')
        set_value "/xyz/openbmc_project/sensors/percent/$name" "$val"
    done
}

while true; do
    ensure_device
    HW=$(find_hwmon || true)
    [ -n "$HW" ] && publish "$HW"
    sleep "$INTERVAL"
done
