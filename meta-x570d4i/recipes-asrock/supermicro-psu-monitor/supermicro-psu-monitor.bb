SUMMARY = "Consolidated Supermicro PWS-505P-1H PSU monitor for X570D4I-2T"
DESCRIPTION = "The PSU on PSU_SMB1 (i2c-2 0x38) is not a real PMBus register \
device (every SMBus read returns a flat IPMI-FRU image; the kernel pmbus \
driver fails to bind and phosphor-psu-monitor is PMBus/IBM-CFF oriented), so \
its telemetry is decoded here directly. This single sdbusplus daemon replaces \
the psu-bridge shell script and the entity-manager PSU config: it publishes \
the six PSU sensors (with thresholds + chassis association), the PowerSupply \
inventory item (Item.PowerSupply + Asset + OperationalStatus, VPD parsed from \
the FRU product area), and the chassis 'powered_by' association that makes the \
PSU appear in Redfish PowerSubsystem/PowerSupplies and the WebUI."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://supermicro-psu-monitor.cpp \
           file://meson.build \
           file://supermicro-psu-monitor.service"

S = "${UNPACKDIR}"

DEPENDS = "sdbusplus boost systemd"

inherit meson pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "supermicro-psu-monitor.service"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/supermicro-psu-monitor.service \
        ${D}${systemd_system_unitdir}/supermicro-psu-monitor.service
}

FILES:${PN} += "${systemd_system_unitdir}/supermicro-psu-monitor.service"
