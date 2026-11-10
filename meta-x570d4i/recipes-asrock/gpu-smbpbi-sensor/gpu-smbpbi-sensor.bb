SUMMARY = "NVIDIA GPU temperature and power over SMBPBI for X570D4I-2T"
DESCRIPTION = "Reads the PCIe-slot GPU's temperature and board power over the \
SMBus Post-Box Interface (slot SMBus = AST2500 I2C4, SMBPBI at 0x4F) and \
publishes them as phosphor sensors 'GPU Temp' and 'GPU Power', associated with \
the board chassis so bmcweb, the web UI and IPMI dynamic sensors pick them up. \
'GPU Temp' is the input of the FAN3 'GPU' zone in phosphor-pid-control."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://gpu-smbpbi-sensor.cpp \
           file://meson.build \
           file://gpu-smbpbi-sensor.service"

S = "${UNPACKDIR}"

DEPENDS = "sdbusplus boost systemd"

inherit meson pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "gpu-smbpbi-sensor.service"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/gpu-smbpbi-sensor.service \
        ${D}${systemd_system_unitdir}/gpu-smbpbi-sensor.service
}

FILES:${PN} += "${systemd_system_unitdir}/gpu-smbpbi-sensor.service"
