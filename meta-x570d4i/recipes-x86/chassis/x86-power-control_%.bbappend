FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += " \
    file://power-config-host0.json \
    "

# Silence the "beep returned error ... No route to host" logged on every
# power-fail: xyz.openbmc_project.BeepCode has no implementation anywhere in
# the OpenBMC tree, so the call can only ever fail on this board.
SRC_URI += " file://0001-quiet-optional-beepcode-provider.patch"
PATCHTOOL = "patch"

# Instead of enabling GPIO passthrough via the SCU (SCU70 - Bit 21), we will
# enable button passthrough in x86-power-control, as any pin that's being
# passthroughed in hardware can not be used once the SCU is being locked.
EXTRA_OEMESON:append = " \
    -Dbutton-passthrough=enabled \
    "

do_install:append() {
    install -d  ${D}/${datadir}/${PN}
    install -m 0644 ${UNPACKDIR}/power-config-host0.json ${D}/${datadir}/${PN}
}
