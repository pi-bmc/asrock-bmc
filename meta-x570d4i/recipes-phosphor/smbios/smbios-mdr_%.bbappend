# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe; it
# provides the xyz.openbmc_project.Smbios.MDR_V2 D-Bus backend.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# Enable the STANDARD smbios-ipmi-blob handler (the "/smbios" phosphor-ipmi-blobs
# receiver, installed to ${libdir}/blob-ipmid).  This is the default OpenBMC path
# for a host to push its full SMBIOS table to the BMC over KCS, inline — used by
# our injected SmbiosBmcPushDxe (recipes-bsp/host-bios-image).  It replaces the
# old asrock-ipmi-oem AMI-MDR (0x5D) synth path.
#
# There is no MDRv2 IPMI alternative to weigh this against. smbios-mdr registers
# NO ipmid handlers at all -- it ships the xyz.openbmc_project.Smbios.MDR_V2
# D-Bus service plus this blob handler, and nothing else. The MDR II *IPMI*
# command set (NetFn 0x3E, cmds 0x30-0x3D) exists only in intel-ipmi-oem, which
# we do not install, and it could not work here if we did: those commands carry
# no payload (mdr2SendDataBlock takes only offset/length/checksum) because the
# bytes move through a shared-memory aperture the BMC reaches via
# /dev/vgasharedmem. That device does not exist on this board -- no aspeed VGA
# shared-memory driver, no device-tree node -- so the blob handler is not a
# preference over MDR II, it is the only inline-over-KCS receiver available.
# See OpenOobPkg/Include/Library/OobIntelOemLib.h for the host-side reasoning.
# Enabling this PACKAGECONFIG also pulls phosphor-ipmi-blobs into the build.
PACKAGECONFIG:append = " smbios-ipmi-blob"

# Install the DIMM socket/channel location table for this board. All four
# SO-DIMM slots are listed; values map to the Socket / MemoryController /
# Channel / Slot fields for Redfish/IPMI DIMM info.
#
# Keys are the SMBIOS Type 17 *Device Locator* alone, which is what upstream
# dimm.cpp looks up (data.find(deviceLocator)) -- no patch required.
#
# That works because the locators are made unique on the HOST side before the
# table is ever pushed. This BIOS numbers slots per bank, reporting "DIMM 0" /
# "DIMM 1" under BOTH "P0 CHANNEL A" and "P0 CHANNEL B", so all four slots
# would otherwise collapse onto two keys and channel B could not be given its
# own Channel value. SmbiosBmcPushDxe rewrites each Type 17 Device Locator to
# "DIMM_<channel><slot>" (DIMM_A0/A1/B0/B1) while building the blob it pushes;
# see the Type 17 normalisation block in
# recipes-bsp/host-bios-image/files/OpenOobPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.c.
# Only the pushed copy is rewritten -- the host OS still sees the vendor's
# original naming in dmidecode.
#
# Keep these keys in step with MakeDimmName() in that file. A key that matches
# nothing makes dimm.cpp log "Failed find the corresponding table for dimm ..."
# and zero all four fields.
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

# Stop the Inventory-anchor startup race being logged as three errors per boot.
# smbios-mdr starts before entity-manager has published the board object (~25s
# gap here), installs an interfacesAdded match rule for exactly that case, and
# recovers on its own -- the journal shows "Successful match on system
# interface" right after. Demoted to debug/info; see the patch header.
SRC_URI:append = " file://0004-mdrv2-don-t-report-the-inventory-startup-race-as-an-error.patch"
PATCHTOOL = "patch"

do_install:append() {
    install -d ${D}${datadir}/smbios-mdr
    install -m 0644 ${UNPACKDIR}/memoryLocationTable.json \
        ${D}${datadir}/smbios-mdr/memoryLocationTable.json
}

FILES:${PN}:append = " ${datadir}/smbios-mdr/memoryLocationTable.json"
