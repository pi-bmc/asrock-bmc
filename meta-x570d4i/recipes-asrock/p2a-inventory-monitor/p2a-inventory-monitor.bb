SUMMARY = "Host-supplied PCIe/NVMe inventory over the AST2500 P2A bridge for X570D4I-2T"
DESCRIPTION = "The X570D4I-2T routes no NVMe-MI SMBus/MCTP sideband to the BMC, \
so nvmesensor and phosphor-nvme can never populate Redfish Storage/Drive. \
Instead the host BIOS driver InventoryOobDxe (OpenOobPkg) enumerates PCIe \
functions and NVMe drives at ReadyToBoot and writes a CRC'd blob into the \
DT-reserved pci_memory window of BMC DRAM over the PCIe-to-AHB (P2A) bridge. \
This daemon unlocks that window via /dev/aspeed-p2a-ctrl, validates the blob, \
and republishes it as phosphor Inventory.Item.Storage / Item.Drive / \
Item.StorageController / Item.PCIeDevice objects (with Asset, presence, SMART \
life and a chassis 'drive' association) that bmcweb renders as Redfish Storage, \
Drives, StorageControllers and PCIeDevices. Publication is gated on host power. \
It also publishes the host's onboard NIC MACs (Item.NetworkInterface, read from \
the board FRU EEPROM, NOT gated on host power) backing bmcweb's Systems \
EthernetInterfaces routes."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://p2a-inventory-monitor.cpp \
           file://oob-inventory-blob.h \
           file://meson.build \
           file://p2a-inventory-monitor.service"

S = "${UNPACKDIR}"

# hwdata-native supplies pci.ids for the build-time trim below. It is a native
# dependency only -- nothing from hwdata is installed on the target.
DEPENDS = "sdbusplus boost systemd hwdata-native"

inherit meson pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "p2a-inventory-monitor.service"

# Vendor/device name table, so Redfish shows "NVIDIA Corporation" /
# "GK210GL [Tesla K80]" instead of 0x10de:0x102d.
#
# The stock hwdata PACKAGE is deliberately not pulled into the image: it is a
# single package covering usb.ids and the ~5 MiB oui.txt as well, which this
# board's 32 MiB rofs cannot absorb. Instead take just pci.ids and strip it to
# the lines the daemon reads:
#
#   - comments and blank lines           (noise)
#   - two-tab subsystem lines            (~18k lines; never looked up)
#   - the trailing "C xx" class section  (base classes are mapped in code to
#                                         fixed Redfish enum values instead)
#
# That is ~1.6 MiB down to ~900 KiB, which squashfs-xz compresses well. The
# daemon tolerates the file being absent or unparseable and falls back to
# publishing the raw ids, so this is an enrichment, never a dependency.
do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/p2a-inventory-monitor.service \
        ${D}${systemd_system_unitdir}/p2a-inventory-monitor.service

    install -d ${D}${datadir}/${BPN}
    awk '/^#/ || /^$/ { next }
         /^C / { exit }
         /^\t\t/ { next }
         { print }' \
        ${STAGING_DATADIR_NATIVE}/hwdata/pci.ids \
        > ${D}${datadir}/${BPN}/pci.ids
    [ -s ${D}${datadir}/${BPN}/pci.ids ] || \
        bbfatal "pci.ids trim produced an empty table; check hwdata-native layout"
}

FILES:${PN} += "${systemd_system_unitdir}/p2a-inventory-monitor.service \
                ${datadir}/${BPN}/pci.ids"
