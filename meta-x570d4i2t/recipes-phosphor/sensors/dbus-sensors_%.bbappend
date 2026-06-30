# Trim sensor daemons with no working hardware on this board:
#   - intelcpusensor: Intel PECI-only; this is an AMD Ryzen board.
#   - psusensor: no PMBus PSU answers at the scanned 0x58/0x59 (the board's PSU
#     telemetry is a non-standard device at i2c2 0x38), so it only logs
#     "<hwmon> not found in sensor whitelist" for every hwmon on the box.
# fansensor is dropped: the AST2500 PWM/tach is not in the fan path (the fans
# are 3-wire, driven by the host NCT6779 SmartFan), so fansensor only logged
# "reading error!" / "failed to find match" / pwm_enable noise. NCT6779 fan-duty
# and SuperIO temps are surfaced instead via nct6779-bridge -> ExternalSensor,
# which needs the 'external' daemon enabled.
PACKAGECONFIG = " \
        adcsensor \
        external \
        hwmontempsensor \
        ipmbsensor \
        nvmesensor \
        "
