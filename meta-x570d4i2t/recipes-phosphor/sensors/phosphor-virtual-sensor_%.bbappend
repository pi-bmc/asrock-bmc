# X570D4I-2T: no virtual sensors (empty config).
#
# This previously published two constant fan_tach values to keep swampd's fan
# PID out of failsafe. But the board's fans are 3-wire and driven by the host
# NCT6779 SmartFan (the AST2500 PWM/tach is not in the fan path), and swampd
# isn't installed on this image anyway, so the AST AspeedFan/PID loop was
# removed. Fan duty + SuperIO temps are now surfaced read-only by nct6779-bridge
# -> ExternalSensor instead. The empty config is kept so the recipe still owns
# the path; add virtual-sensor stanzas here if a real derived sensor is needed.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://config-virtual-sensor.json"

do_install:append() {
    install -d ${D}${datadir}/${PN}
    install -m 0644 ${UNPACKDIR}/config-virtual-sensor.json \
        ${D}${datadir}/${PN}/virtual_sensor_config.json
}

FILES:${PN}:append = " ${datadir}/${PN}/virtual_sensor_config.json"
