# Enable dynamic sensor discovery so voltage/temperature/fan sensors published
# by adcsensor, hwmontempsensor, and externalsensor appear in the IPMI SDR
# without needing a static sensor.yaml map.
PACKAGECONFIG:append = " dynamic-sensors"

# Pre-seed /run/ipmi/channel_access_volatile.json at boot (tmpfiles, runs before
# ipmid) so phosphor-ipmi-host doesn't log "channel_access_volatile.json not
# found" on first read each boot. See the .conf for details.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " file://ipmi-channel-access-volatile.conf"

do_install:append() {
    install -d ${D}${sysconfdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/ipmi-channel-access-volatile.conf \
        ${D}${sysconfdir}/tmpfiles.d/ipmi-channel-access-volatile.conf
}

FILES:${PN}:append = " ${sysconfdir}/tmpfiles.d/ipmi-channel-access-volatile.conf"
