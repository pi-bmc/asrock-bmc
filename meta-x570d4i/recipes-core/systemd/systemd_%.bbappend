FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Stop systemd-networkd-wait-online.service failing on every BMC boot. See the
# drop-in's own comment for why --any is the right semantic on this board.
SRC_URI:append = " file://wait-online-any.conf"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d
    install -m 0644 ${UNPACKDIR}/wait-online-any.conf \
        ${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d/wait-online-any.conf
}

# No FILES override needed: the drop-in directory is named
# systemd-networkd-wait-online.service.d, and systemd_259.5.bb already ships
# "FILES:${PN}-networkd += ${systemd_system_unitdir}/systemd-networkd*", whose
# glob covers it. Adding it to ${PN} instead would split the drop-in away from
# the unit it patches.
