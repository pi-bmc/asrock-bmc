# Enable dynamic sensor discovery so voltage/temperature/fan sensors published
# by adcsensor, hwmontempsensor, and externalsensor appear in the IPMI SDR
# without needing a static sensor.yaml map.
PACKAGECONFIG:append = " dynamic-sensors"

# Pre-seed /run/ipmi/channel_access_volatile.json at boot (tmpfiles, runs before
# ipmid) so phosphor-ipmi-host doesn't log "channel_access_volatile.json not
# found" on first read each boot. See the .conf for details.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " file://ipmi-channel-access-volatile.conf"

# Keep a Continuous BootSourceOverride alive across the BIOS's param-5 clear;
# see the patch header for why the BMC is the right place to decline it.
SRC_URI:append = " file://0001-chassis-keep-persistent-boot-override-on-BIOS-clear.patch"

# The BIOS's all-zero clear must not rewrite BootType to Legacy, and boot
# option reads must answer from a cache rather than twelve D-Bus round trips:
# a slow KCS reply wedges the interface mid-POST and drops the boot override.
# See the patch headers for the measurements.
SRC_URI:append = " file://0002-chassis-keep-boot-type-on-BIOS-clear.patch"
SRC_URI:append = " file://0003-chassis-answer-boot-option-reads-from-a-cache.patch"

do_install:append() {
    install -d ${D}${sysconfdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/ipmi-channel-access-volatile.conf \
        ${D}${sysconfdir}/tmpfiles.d/ipmi-channel-access-volatile.conf
}

FILES:${PN}:append = " ${sysconfdir}/tmpfiles.d/ipmi-channel-access-volatile.conf"
