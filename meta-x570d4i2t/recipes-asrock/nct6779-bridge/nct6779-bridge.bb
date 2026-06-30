SUMMARY = "Bridge NCT6779 SuperIO hwmon readings to OpenBMC ExternalSensor objects"
DESCRIPTION = "On X570D4I-2T, the host-side Nuvoton NCT6779 hardware monitor \
is reachable from the BMC at i2c-1 0x2d but entity-manager's NCT6779 sensor \
flow doesn't work on this stack. This recipe installs a small shell daemon \
that polls the nct6775-i2c hwmon sysfs and pushes the values into the \
ExternalSensor objects declared in the board's entity-manager config."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://nct6779-bridge.sh \
           file://nct6779-bridge.service"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "nct6779-bridge.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/nct6779-bridge.sh ${D}${libexecdir}/nct6779-bridge.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/nct6779-bridge.service \
        ${D}${systemd_system_unitdir}/nct6779-bridge.service
}

FILES:${PN} += "${libexecdir}/nct6779-bridge.sh \
                ${systemd_system_unitdir}/nct6779-bridge.service"

RDEPENDS:${PN} += "bash i2c-tools systemd"
