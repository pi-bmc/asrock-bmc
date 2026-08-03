SUMMARY = "Enable USB ethernet"
PR = "r1"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS += "systemd"
RDEPENDS:${PN} += "libsystemd bash"

S = "${UNPACKDIR}"

inherit allarch systemd

SRC_URI += "file://usb-network.service \
            file://usb_network.sh \
            file://00-bmc-usb0.network"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/usb-network.service ${D}${systemd_system_unitdir}

    install -d ${D}${sysconfdir_native}/systemd/network/
    install -m 0644 ${UNPACKDIR}/00-bmc-usb0.network ${D}${sysconfdir_native}/systemd/network

    install -d ${D}/${sbindir}
    install -m 755 ${UNPACKDIR}/usb_network.sh ${D}/${sbindir}
}

NATIVE_SYSTEMD_SUPPORT = "1"
SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "usb-network.service"
# Enable at boot explicitly (don't rely on the default). This bakes the
# multi-user.target.wants symlink and a "98-usb-network.preset: enable" entry
# into the image so the gadget comes up without a manual `systemctl enable`.
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
