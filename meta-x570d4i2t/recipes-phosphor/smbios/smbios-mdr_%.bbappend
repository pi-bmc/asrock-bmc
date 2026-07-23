# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe; it
# provides the xyz.openbmc_project.Smbios.MDR_V2 D-Bus backend.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# Enable the STANDARD smbios-ipmi-blob handler (the "/smbios" phosphor-ipmi-blobs
# receiver, installed to ${libdir}/blob-ipmid).  This is the default OpenBMC path
# for a host to push its full SMBIOS table to the BMC over KCS, inline — used by
# our injected SmbiosBmcPushDxe (recipes-bsp/host-bios-image).  It replaces the
# old asrock-ipmi-oem AMI-MDR (0x5D) synth path.  We use the blob handler (not
# the MDRv2 agentSynchronizeData IPMI command set) because it is the standard
# OpenBMC inline-over-KCS receiver our injected DXE targets. (This smbios-mdr's
# MDRv2 is IPMI-message based; it has no mmap//dev/mem shared-memory path
# (verified by grep), so there is no VGA-aperture requirement either way.)
# Enabling this PACKAGECONFIG also pulls phosphor-ipmi-blobs into the build.
PACKAGECONFIG:append = " smbios-ipmi-blob"

# Install the DIMM socket/channel location table for this board. All four
# SO-DIMM slots are listed; values map to the Socket / MemoryController /
# Channel / Slot fields for Redfish/IPMI DIMM info.
#
# Keys are the fully-qualified "<bank locator> <device locator>" strings, which
# requires 0003 below. Upstream keys this table on the SMBIOS Type 17 *Device
# Locator* alone, but this board's BIOS numbers slots per bank and reports
# device locators "DIMM 0" / "DIMM 1" under BOTH banks ("P0 CHANNEL A" and
# "P0 CHANNEL B"), so all four slots collapse onto two keys and channel B
# cannot be given its own Channel value. Confirmed on the live board: the four
# dimm objects publish MemoryDeviceLocator "P0 CHANNEL A DIMM 0/1" and
# "P0 CHANNEL B DIMM 0/1". A key that matches nothing makes dimm.cpp log
# "Failed find the corresponding table for dimm ..." and zero all four fields.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://memoryLocationTable.json"

# Make the /smbios blob handler's path-stat report the SMBIOS table already
# persisted at /var/lib/smbios/smbios2 when no blob session is open (upstream
# returns failure in that case).  This lets our injected SmbiosBmcPushDxe do a
# single BmcBlobStat (sub 0x08) first and skip the full table push over KCS when
# the BMC already has the data -- faster host boot.  rm the file + reboot to
# force a refresh.
SRC_URI:append = " file://0001-smbios-blob-stat-persisted-file.patch"

# Silence the benign "bios_active not found" mapper error. smbios-mdr best-effort
# propagates the SMBIOS BIOS version to /xyz/openbmc_project/software/bios_active,
# which this board doesn't have (its BIOS is a HostSPIFlash code-update object).
SRC_URI:append = " file://0002-quiet-optional-bios-active-lookup.patch"

# Look the memoryLocationTable up by the full "<bank> <device>" locator, falling
# back to the bare device locator. Required for this board's per-bank DIMM
# numbering — see the memoryLocationTable comment above.
SRC_URI:append = " file://0003-dimm-match-full-bank-device-locator.patch"
PATCHTOOL = "patch"

do_install:append() {
    install -d ${D}${datadir}/smbios-mdr
    install -m 0644 ${UNPACKDIR}/memoryLocationTable.json \
        ${D}${datadir}/smbios-mdr/memoryLocationTable.json
}

FILES:${PN}:append = " ${datadir}/smbios-mdr/memoryLocationTable.json"
