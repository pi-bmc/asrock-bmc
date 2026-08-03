FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# channel_config.json declares channel 8 = usb0, the Redfish Host Interface link
# (DSP0270). It exists so the host BIOS can learn the BMC's HI address without
# any out-of-band knowledge: RedfishHi issues "Get LAN Configuration Parameters,
# channel 8" over KCS and reads parameter 3 (IP) / 5 (MAC) / 6 (subnet).
# Verified on hardware: param 3 -> a9 fe 00 11 = 169.254.0.17, param 4 -> 01
# (Static, which is why 00-bmc-usb0.network must set Scope=global), param 5 ->
# 02:00:16:92:54:17, param 6 -> 255.255.0.0.
#
# channel_access.json must carry a matching "8" entry. A channel present in
# channel_config.json but absent from channel_access.json defaults to
# access_mode "disabled", so "Get Channel Access 8" reports the host interface
# as disabled even though its LAN parameters read back correctly.

SRC_URI:append = " \
    file://channel_access.json \
    file://channel_config.json \
    file://master_write_read_white_list.json \
    "

do_install:append() {
    install -m 0644 ${UNPACKDIR}/channel_access.json \
        ${D}${datadir}/ipmi-providers/channel_access.json
    install -m 0644 ${UNPACKDIR}/channel_config.json \
        ${D}${datadir}/ipmi-providers/channel_config.json
    install -m 0644 ${UNPACKDIR}/master_write_read_white_list.json \
        ${D}${datadir}/ipmi-providers/master_write_read_white_list.json
}

FILES:${PN}:append = " \
    ${datadir}/ipmi-providers/channel_access.json \
    ${datadir}/ipmi-providers/channel_config.json \
    ${datadir}/ipmi-providers/master_write_read_white_list.json \
    "
    