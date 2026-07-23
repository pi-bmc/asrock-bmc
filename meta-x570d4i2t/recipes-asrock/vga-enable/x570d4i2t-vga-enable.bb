SUMMARY = "Enable the AST2500 host VGA PCIe function for KVM video capture"
DESCRIPTION = "On X570D4I-2T the aspeed XDMA engine is intentionally disabled \
(it would clear the SCU VGA_EN bit and present the host PCIe device as the \
non-VGA BMC/XDMA device 1a03:2402, blanking the KVM). Since nothing else \
programs SCU PCIE_CONF, this oneshot sets VGA_EN at every BMC boot so the host \
enumerates the onboard VGA controller (1a03:2000) and the aspeed-video engine \
has a signal to capture."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://x570d4i2t-vga-enable.sh \
           file://x570d4i2t-vga-enable.service \
           file://x570d4i2t-vga-enable-monitor.sh \
           file://x570d4i2t-vga-enable-monitor.service"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "x570d4i2t-vga-enable.service \
                         x570d4i2t-vga-enable-monitor.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/x570d4i2t-vga-enable.sh \
        ${D}${libexecdir}/x570d4i2t-vga-enable.sh
    install -m 0755 ${UNPACKDIR}/x570d4i2t-vga-enable-monitor.sh \
        ${D}${libexecdir}/x570d4i2t-vga-enable-monitor.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/x570d4i2t-vga-enable.service \
        ${D}${systemd_system_unitdir}/x570d4i2t-vga-enable.service
    install -m 0644 ${UNPACKDIR}/x570d4i2t-vga-enable-monitor.service \
        ${D}${systemd_system_unitdir}/x570d4i2t-vga-enable-monitor.service
}

FILES:${PN} += "${libexecdir}/x570d4i2t-vga-enable.sh \
                ${libexecdir}/x570d4i2t-vga-enable-monitor.sh \
                ${systemd_system_unitdir}/x570d4i2t-vga-enable.service \
                ${systemd_system_unitdir}/x570d4i2t-vga-enable-monitor.service"

# devmem + mknod are provided by busybox (always in the image)
RDEPENDS:${PN} += "busybox"

COMPATIBLE_MACHINE = "x570d4i2t"
