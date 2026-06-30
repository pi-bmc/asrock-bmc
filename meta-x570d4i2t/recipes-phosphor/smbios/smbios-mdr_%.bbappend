# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe; it
# provides the xyz.openbmc_project.Smbios.MDR_V2 D-Bus backend.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# Enable the STANDARD smbios-ipmi-blob handler (the "/smbios" phosphor-ipmi-blobs
# receiver, installed to ${libdir}/blob-ipmid).  This is the default OpenBMC path
# for a host to push its full SMBIOS table to the BMC over KCS, inline — used by
# our injected SmbiosBmcPushDxe (recipes-bsp/host-bios-image).  It replaces the
# old asrock-ipmi-oem AMI-MDR (0x5D) synth path.  NOTE: the Intel MDR V2 IPMI
# push (NetFn 0x3E) is NOT viable here — it transfers via host<->BMC VGA shared
# memory (smbios-mdr mmaps /dev/mem at the agent's xferAddress), which this board
# has no aperture for; the blob handler carries the data inline over KCS.
# Enabling this PACKAGECONFIG also pulls phosphor-ipmi-blobs into the build.
PACKAGECONFIG:append = " smbios-ipmi-blob"

# Install the DIMM socket/channel location table for this board.
# Keys are SMBIOS Type 17 Device Locator strings as reported by the ASRock
# BIOS (CPU1_DIMM_A1 / CPU1_DIMM_B1); values map each slot to its
# Socket / MemoryController / Channel / Slot index for Redfish/IPMI DIMM info.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://memoryLocationTable.json"

# Make the /smbios blob handler's path-stat report the SMBIOS table already
# persisted at /var/lib/smbios/smbios2 when no blob session is open (upstream
# returns failure in that case).  This lets our injected SmbiosBmcPushDxe do a
# single BmcBlobStat (sub 0x08) first and skip the full table push over KCS when
# the BMC already has the data -- faster host boot.  rm the file + reboot to
# force a refresh.
SRC_URI:append = " file://0001-smbios-blob-stat-persisted-file.patch"
PATCHTOOL = "patch"

do_install:append() {
    install -d ${D}${datadir}/smbios-mdr
    install -m 0644 ${UNPACKDIR}/memoryLocationTable.json \
        ${D}${datadir}/smbios-mdr/memoryLocationTable.json
}

FILES:${PN}:append = " ${datadir}/smbios-mdr/memoryLocationTable.json"
