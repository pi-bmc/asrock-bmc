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
Drives, StorageControllers and PCIeDevices. Publication is gated on host power."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://p2a-inventory-monitor.cpp \
           file://oob-inventory-blob.h \
           file://meson.build \
           file://p2a-inventory-monitor.service"

S = "${UNPACKDIR}"

DEPENDS = "sdbusplus boost systemd"

inherit meson pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "p2a-inventory-monitor.service"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/p2a-inventory-monitor.service \
        ${D}${systemd_system_unitdir}/p2a-inventory-monitor.service
}

FILES:${PN} += "${systemd_system_unitdir}/p2a-inventory-monitor.service"
