FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://x570d4i2t.cfg \
    file://0001-append-x570d4i25-dtb-to-dts-makefile.patch \
    file://aspeed-bmc-asrock-x570d4i2t.dts \
"

# Deliver the machine device tree as a managed source file instead of a large
# "add the whole .dts" patch (which drifted out of sync with the real built DT).
# KERNEL_DEVICETREE (in conf/machine/x570d4i2t.conf) names the .dtb, and
# kernel-devicetree.bbclass builds it with `oe_runmake <dtb>`, which compiles the
# .dts directly from the kernel source tree -- no arch/arm/boot/dts/aspeed/Makefile
# entry is required. So we just drop the .dts into the kernel source tree before
# configure/compile. Copying an untracked file into the shared kernel tree would
# otherwise mark it "dirty" and append a +/-dirty suffix to the kernel version
# (which would desync /lib/modules/<version>); SCMVERSION = "0" suppresses that.
SCMVERSION = "0"

do_configure:prepend() {
    # The kernel recipe unpacks loose SRC_URI files under ${WORKDIR}/sources
    # (same place as the .cfg fragments), not ${WORKDIR} directly.
    install -D -m 0644 "${WORKDIR}/sources/aspeed-bmc-asrock-x570d4i2t.dts" \
        "${STAGING_KERNEL_DIR}/arch/arm/boot/dts/aspeed/aspeed-bmc-asrock-x570d4i2t.dts"
}
