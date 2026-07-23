FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://x570d4i2t.json \
    file://supermicro-pws-505p-1h.json \
    file://blacklist.json \
    "

do_install:append() {
    # EntityManager reads board configs from .../entity-manager/configurations/
    install -d ${D}${datadir}/entity-manager/configurations
    install -m 0644 ${UNPACKDIR}/x570d4i2t.json \
        ${D}${datadir}/entity-manager/configurations/x570d4i2t.json
    # Supermicro PSU as its own vendor config (upstream-style split): probes the
    # PSU's own FRU (i2c2/0x38) and owns the PowerSupply inventory object at
    # /xyz/openbmc_project/inventory/system/powersupply/psu0 with FRU-templated
    # Asset/Revision. Sensors + the chassis powered_by association stay in
    # supermicro-psu-monitor (the flat register map is not PMBus, so the
    # upstream pmbus/psusensor exposes cannot be used).
    install -m 0644 ${UNPACKDIR}/supermicro-pws-505p-1h.json \
        ${D}${datadir}/entity-manager/configurations/supermicro-pws-505p-1h.json
    # blacklist.json is read by FruDevice from the parent entity-manager dir.
    install -m 0644 ${UNPACKDIR}/blacklist.json \
        ${D}${datadir}/entity-manager/blacklist.json
}

FILES:${PN}:append = " \
    ${datadir}/entity-manager/configurations/x570d4i2t.json \
    ${datadir}/entity-manager/configurations/supermicro-pws-505p-1h.json \
    ${datadir}/entity-manager/blacklist.json \
    "
