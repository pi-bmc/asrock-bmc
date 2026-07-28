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
    # It scans 0x03-0x77 on every bus and logs each address that either answers
    # without a valid FRU header ("failed to read bus N address M") or is held
    # by a bound driver ("device at bus N address M busy"). None of those are
    # faults, but they repeat on every rescan, so the known-not-a-FRU addresses
    # are blocked per-bus (the parser accepts either a bare bus number or a
    # {"bus", "addresses"} object; JSON comments are NOT accepted, hence this
    # note living here):
    #   0/40-43 0x70  PCA9545 mux, mirrored onto its own channel buses
    #   1 0x2d/0x4c   NCT6779 + W83773G, instantiated by entity-manager
    #   2 0x3c        AMD SB-RMI (APML), answers 0x20 but NAKs everything else
    #   4 0x4e/0x4f   NCT75 aux temp, instantiated by entity-manager
    #   6 0x60        unidentified responder on the host-shared SMBus
    # 2/0x38 is deliberately NOT blocked: that is the PWS-505P-1H FRU that the
    # supermicro-pws-505p-1h.json probe matches on. Bus 7 needs no entries --
    # the SPD/FRU EEPROMs there are kernel-bound, and FruDevice's
    # findI2CEeproms() reads those over sysfs and skips them automatically.
    # (The previous "buses": [9, 10, 11] was inert: the DTS declares no
    # &i2c9/&i2c10/&i2c11, so those adapters never exist.)
    install -m 0644 ${UNPACKDIR}/blacklist.json \
        ${D}${datadir}/entity-manager/blacklist.json
}

FILES:${PN}:append = " \
    ${datadir}/entity-manager/configurations/x570d4i2t.json \
    ${datadir}/entity-manager/configurations/supermicro-pws-505p-1h.json \
    ${datadir}/entity-manager/blacklist.json \
    "
