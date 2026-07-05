FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://config-virtual-sensor.json"

do_install:append() {
    install -d ${D}${datadir}/${PN}
    install -m 0644 ${UNPACKDIR}/config-virtual-sensor.json \
        ${D}${datadir}/${PN}/virtual_sensor_config.json
}

FILES:${PN}:append = " ${datadir}/${PN}/virtual_sensor_config.json"
