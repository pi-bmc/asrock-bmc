FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://x570d4i2t.json \
    file://blacklist.json \
    "

do_install:append() {
    # EntityManager reads board configs from .../entity-manager/configurations/
    install -d ${D}${datadir}/entity-manager/configurations
    install -m 0644 ${UNPACKDIR}/x570d4i2t.json \
        ${D}${datadir}/entity-manager/configurations/x570d4i2t.json
    # blacklist.json is read by FruDevice from the parent entity-manager dir.
    install -m 0644 ${UNPACKDIR}/blacklist.json \
        ${D}${datadir}/entity-manager/blacklist.json
}

FILES:${PN}:append = " \
    ${datadir}/entity-manager/configurations/x570d4i2t.json \
    ${datadir}/entity-manager/blacklist.json \
    "
