FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://wait-online-any.conf \
    file://50-no-watchdog-pretimeout.conf \
"

# Drop-in for the unit itself: /lib/systemd/system/<unit>.service.d/
install_networkd_wait_online_dropin() {
    install -d ${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d
    install -m 0644 ${UNPACKDIR}/wait-online-any.conf \
        ${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d/
}

# Manager config, a different directory: /lib/systemd/system.conf.d/
# Sorts after meta-phosphor's 40-hardware-watchdog.conf so it wins.
install_no_watchdog_pretimeout() {
    install -d ${D}${systemd_unitdir}/system.conf.d
    install -m 0644 ${UNPACKDIR}/50-no-watchdog-pretimeout.conf \
        ${D}${systemd_unitdir}/system.conf.d/
}

do_install[postfuncs] += "install_networkd_wait_online_dropin install_no_watchdog_pretimeout"

# Only the watchdog drop-in needs a FILES entry. The wait-online one is already
# shipped by systemd's own FILES:${PN}-networkd, which globs
# "${systemd_system_unitdir}/systemd-networkd*" -- that matches the
# systemd-networkd-wait-online.service.d directory. Adding it to ${PN} instead
# would split the drop-in into a different package from the unit it patches.
FILES:${PN}:append = " ${systemd_unitdir}/system.conf.d/50-no-watchdog-pretimeout.conf"
