# Enable biosconfig-manager for the X570D4I-2T.
#
# This service provides the xyz.openbmc_project.BIOSConfig.Manager D-Bus
# interface that backs the stock bmcweb Redfish BIOS endpoints:
#   /redfish/v1/Systems/system/Bios
#   /redfish/v1/Systems/system/Bios/Settings
#
# HOST-PUSH PATH: this store is populated in-band over KCS by the BIOS OOB
# config IPMI commands in asrock-ipmi-oem (biosconfigcommands.cpp, NetFn 0x30
# cmd 0xD3-0xD8 — the Intel path the AMI BIOS actually drives). The host pushes its
# attribute registry as JSON via SetPayload and the BMC writes BaseBIOSTable
# here. This replaces the AMI USB Redfish Host Interface config push, which was
# removed from this layer. (The seedData file below is also written by that
# provider's Set/GetBIOSPwdHash handlers.)
#
# No board-specific overrides are needed; inheriting the recipe as-is
# is sufficient.

# Ensure the state directory exists at build time
do_install:append() {
    install -d ${D}${localstatedir}/lib/bios-settings-manager
}

FILES:${PN}:append = " ${localstatedir}/lib/bios-settings-manager"
