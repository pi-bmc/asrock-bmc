FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://x570d4i2t.json \
    file://blacklist.json \
    "

do_install:append() {
    # EntityManager reads board configs from .../entity-manager/configurations/,
    # NOT the parent dir. Installing x570d4i2t.json to the parent (the old path)
    # meant EM kept using a stale cached copy in configurations/ and never picked
    # up our config changes (e.g. NCT6779-bridge removal, NCT75 Aux Temp).
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
