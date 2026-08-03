FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Publish xyz.openbmc_project.Control.Power.ACPIPowerState via phosphor-settings-
# manager (under xyz.openbmc_project.Settings) so the upstream IPMI "Set/Get ACPI
# Power State" command (App NetFn 0x06 Cmd 0x06/0x07) finds an object to set
# SysACPIStatus/DevACPIStatus on, instead of failing with "No Object has
# implemented the interface". Declarative + persistent; no source patch needed.
SRC_URI:append = " \
    file://host-acpi-power-state.yaml \
"
SETTINGS_HOST_TEMPLATES:append = " host-acpi-power-state.yaml"
