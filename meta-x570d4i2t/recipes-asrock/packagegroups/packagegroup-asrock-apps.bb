SUMMARY = "OpenBMC for ASRock - Applications"
PR = "r1"

inherit packagegroup

PROVIDES = "${PACKAGES}"
PACKAGES = " \
        ${PN}-chassis \
        ${PN}-fans \
        ${PN}-flash \
        ${PN}-system \
        "

PROVIDES += "virtual/obmc-chassis-mgmt"
PROVIDES += "virtual/obmc-fan-mgmt"
PROVIDES += "virtual/obmc-flash-mgmt"
PROVIDES += "virtual/obmc-system-mgmt"

RPROVIDES:${PN}-chassis += "virtual-obmc-chassis-mgmt"
RPROVIDES:${PN}-fans += "virtual-obmc-fan-mgmt"
RPROVIDES:${PN}-flash += "virtual-obmc-flash-mgmt"
RPROVIDES:${PN}-system += "virtual-obmc-system-mgmt"

SUMMARY:${PN}-chassis = "ASRock Chassis"
RDEPENDS:${PN}-chassis = " \
        obmc-phosphor-buttons-signals \
        obmc-phosphor-buttons-handler \
        phosphor-pid-control \
        phosphor-power-control \
        phosphor-power-regulators \
        phosphor-post-code-manager \
        phosphor-host-postd        \
        phosphor-skeleton-control-power \
        phosphor-ipmi-ipmb \
        "

SUMMARY:${PN}-fans = "ASRock Fans"
RDEPENDS:${PN}-fans = " \
        phosphor-pid-control \
        "

SUMMARY:${PN}-flash = "ASRock Flash"
RDEPENDS:${PN}-flash = " \
        phosphor-ipmi-flash \
        phosphor-ipmi-blobs \
        "

SUMMARY:${PN}-system = "ASRock System"
RDEPENDS:${PN}-system = " \
        entity-manager \
        phosphor-software-manager \
        phosphor-virtual-sensor \
        x570d4i2t-vga-enable \
        aspeed-video-watchdog \
        nct6779-bridge \
        smbios-mdr \
        phosphor-ipmi-blobs \
        biosconfig-manager \
        asrock-ipmi-oem \
        dbus-sensors \
        usb-network \
        phosphor-misc-usb-ctrl \
        phosphor-host-postd \
        phosphor-post-code-manager \
        phosphor-power-regulators \
        phosphor-power-control \
        "
