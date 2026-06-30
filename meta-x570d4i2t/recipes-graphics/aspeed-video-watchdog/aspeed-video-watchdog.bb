SUMMARY = "Watchdog that resets the ASPEED video engine on V4L2 link failure"
DESCRIPTION = "Monitors the obmc-ikvm journal for 'Link has been severed' \
(ENOLINK returned by the aspeed-video driver) and recovers by stopping \
obmc-ikvm, unbinding/rebinding the aspeed-video platform driver (or \
reloading the module if it is not compiled in), then restarting obmc-ikvm."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://aspeed-video-watchdog.sh \
           file://aspeed-video-watchdog.service"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "aspeed-video-watchdog.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/aspeed-video-watchdog.sh \
        ${D}${libexecdir}/aspeed-video-watchdog.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/aspeed-video-watchdog.service \
        ${D}${systemd_system_unitdir}/aspeed-video-watchdog.service
}

FILES:${PN} += " \
    ${libexecdir}/aspeed-video-watchdog.sh \
    ${systemd_system_unitdir}/aspeed-video-watchdog.service \
"

# journalctl, logger, lsmod, modprobe, sleep — all from busybox.
RDEPENDS:${PN} += "busybox"

# The reset path re-asserts the host VGA PCIe function via the vga-enable
# oneshot's script (/usr/libexec/x570d4i2t-vga-enable.sh).
RDEPENDS:${PN} += "x570d4i2t-vga-enable"
