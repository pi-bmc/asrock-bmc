# Enable biosconfig-manager for the X570D4I-2T.
#
# This service provides the xyz.openbmc_project.BIOSConfig.Manager D-Bus
# interface that backs the stock bmcweb Redfish BIOS endpoints:
#   /redfish/v1/Systems/system/Bios
#   /redfish/v1/Systems/system/Bios/Settings
#
# NOTE: there is currently no host-push path populating this store. The AMI
# host BIOS's in-band BIOS-config push used the USB Redfish Host Interface,
# which was removed from this layer; the IPMI BIOS-OOB path was likewise never
# used by this firmware. The manager is kept so the standard Redfish /Bios
# endpoints exist (empty until a populator is added).
#
# No board-specific overrides are needed; inheriting the recipe as-is
# is sufficient.

# Ensure the state directory exists at build time
do_install:append() {
    install -d ${D}${localstatedir}/lib/bios-settings-manager
}

FILES:${PN}:append = " ${localstatedir}/lib/bios-settings-manager"
