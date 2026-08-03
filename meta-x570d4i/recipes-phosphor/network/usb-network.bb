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
            file://10-rhi.conf"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/usb-network.service ${D}${systemd_system_unitdir}

    install -d ${D}${sysconfdir_native}/systemd/network/
    install -d ${D}${sysconfdir_native}/systemd/network/00-bmc-usb0.network.d
    install -m 0644 ${UNPACKDIR}/10-rhi.conf \
        ${D}${sysconfdir_native}/systemd/network/00-bmc-usb0.network.d/10-rhi.conf

    install -d ${D}/${sbindir}
    install -m 755 ${UNPACKDIR}/usb_network.sh ${D}/${sbindir}
}

NATIVE_SYSTEMD_SUPPORT = "1"
SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "usb-network.service"
# Enable at boot explicitly (don't rely on the default). This bakes the
# .wants symlink and a "98-usb-network.preset: enable" entry into the image so
# the gadget comes up without a manual `systemctl enable`. As of 2026-08-03 the
# unit's [Install] is WantedBy=basic.target, not multi-user.target, so the
# symlink lands in basic.target.wants -- the gadget has to exist before the
# host powers on, which happens well before multi-user is reached. See the
# ordering rationale in usb-network.service.
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
