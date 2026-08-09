# X570D4I-2T host BIOS firmware update support.
#
# The host BIOS SPI flash sits on the AST2500's SPI1 controller (1e630000.spi,
# spi1.0, DTS &spi1 flash@0 "bios") behind a hardware mux on GPIOJ1
# ("control-bios-spi-mux-n"): driving J1 LOW hands the flash to the BMC
# (discovered from stock MegaRAC: BIOS_SPI_GPIO_PIN="J1", LEVEL=0). This
# bypasses AMI Secure Flash and the AMD PSP SPI host-access lock, which only
# guard the host/FCH path, not the BMC's own SPI1 controller.
#
# Update mechanism: the entity-manager driven SPI updater
# (bios-software-update -> xyz.openbmc_project.Software.BIOS). It matches the
# "SPIFlash" record in meta-x570d4i/recipes-phosphor/configuration/
# entity-manager/x570d4i2t.json (SPIControllerIndex 1 / SPIDeviceIndex 0 /
# MuxOutputs control-bios-spi-mux-n Low), asserts the mux, binds the SMC +
# spi-nor drivers, writes, then unbinds and restores the mux. BIOS shows up as
# a Redfish UpdateService firmware inventory item; update images must be
# wrapped as PLDM firmware-update packages whose descriptors match the
# FirmwareInfo (VendorIANA / CompatibleHardware) in the EM record.
#
# This replaces the legacy meta-asrock/meta-common script flow (flash_bios +
# bios-update.sh + /etc/default/bios-update), which is disabled here. The
# phosphor-software-manager-bios-software-update package is pulled into the
# image by packagegroup-asrock-apps.

PACKAGECONFIG:remove = "flash_bios"
PACKAGECONFIG:append = " bios-software-update"

# meta-common's do_install:append installs bios-update.sh unconditionally,
# independent of the flash_bios PACKAGECONFIG. With flash_bios removed the
# script is dead weight (its /etc/default/bios-update env file and the
# obmc-flash-host-bios@.service that invoked it are both gone) - drop it.
# A postfunc (not do_install:append) because this layer's append runs before
# meta-common's install step; postfuncs run after the whole task.
remove_legacy_bios_update_script() {
    rm -f ${D}${sbindir}/bios-update.sh
    # the script was the only thing in sbindir; a leftover empty dir trips
    # the installed-vs-shipped QA check
    rmdir --ignore-fail-on-non-empty ${D}${sbindir}
}
do_install[postfuncs] += "remove_legacy_bios_update_script"

# ---------------------------------------------------------------------------
# Fix the startup coredump (2026-08-03)
# ---------------------------------------------------------------------------
# phosphor-bios-software-update queries the entity-manager SPIFlash config on
# startup and throws an UNCAUGHT sdbusplus exception if it is not on D-Bus yet:
#
#   terminate called after throwing an instance of 'sdbusplus::exception::SdBusError'
#     what():  xyz.openbmc_project.Common.Error.ResourceNotFound
#   Process 319 (phosphor-bios-s) of user 0 dumped core.
#   xyz.openbmc_project.Software.BIOS.service: Main process exited, code=dumped,
#                                              status=6/ABRT
#
# systemd restarts it and the second attempt succeeds (NRestarts=1, then
# Result=success), so the daemon does work -- but every boot leaves a core file
# and a "dumped core" in the journal. Plain unit ordering does not fix it:
# entity-manager reaches "started" well before it finishes publishing inventory
# objects. So poll for the object in ExecStartPre instead.
#
# The "Missing property Name/Polarity on ...MuxOutputs1" errors were a SEPARATE,
# harmless thing -- the daemon discovers how many mux outputs exist by walking
# MuxOutputs0, MuxOutputs1, ... until one is missing, and logged the absent one
# that ends the loop. Our config declares a single MuxOutputs entry, which is
# correct; 0001 makes the probe use a non-logging getter so a correct config
# stops looking broken.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://wait-for-spiflash-config.sh \
    file://10-wait-for-config.conf \
    file://0001-software-manager-don-t-log-the-MuxOutputs-probe-terminator.patch \
    "

install_bios_update_startup_guard() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/wait-for-spiflash-config.sh \
        ${D}${libexecdir}/wait-for-spiflash-config.sh

    install -d ${D}${systemd_system_unitdir}/xyz.openbmc_project.Software.BIOS.service.d
    install -m 0644 ${UNPACKDIR}/10-wait-for-config.conf \
        ${D}${systemd_system_unitdir}/xyz.openbmc_project.Software.BIOS.service.d/10-wait-for-config.conf
}
do_install[postfuncs] += "install_bios_update_startup_guard"

FILES:${PN}-bios-software-update:append = " \
    ${libexecdir}/wait-for-spiflash-config.sh \
    ${systemd_system_unitdir}/xyz.openbmc_project.Software.BIOS.service.d/10-wait-for-config.conf \
    "
