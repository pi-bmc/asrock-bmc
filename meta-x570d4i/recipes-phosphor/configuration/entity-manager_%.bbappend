FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://x570d4i2t.json \
    file://supermicro-pws-505p-1h.json \
    file://blacklist.json \
    "

do_install:append() {
    # EntityManager reads board configs from .../entity-manager/configurations/
    #
    # NO JC42 DIMM THERMAL SENSORS -- do not add them back. The config used to
    # declare four (DDR4_{A1,A2,B1,B2}_Temp at bus 7 / 0x18-0x1B). The DIMMs in
    # this board have no on-DIMM thermal sensor, so hwmontempsensor
    # instantiated and immediately deleted each one on every scan:
    #   "Failed to instantiate 'jc42' at address '26' on bus '7'"
    # ~156 journal lines per boot. Measured 2026-08-04 on the live board:
    # i2cget -y 7 0x18..0x1B all fail, while the SPD EEPROMs at 0x50..0x53 all
    # answer 0x0c (DDR4) -- i.e. four DIMMs are present and none carries a TS.
    # DIMM temperature is therefore not available out-of-band on this board; the
    # host reports it in-band instead.
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
    #   1 0x1c/0x1d     intermittent non-FRU responders. fru-device probes
    #                 0x1c, gets an ACK, then fails the FRU header read
    #                 ("failed to read bus 1 address 28 base offset 0").
    #   6 0x28/0x37/0x60  responders on the host-shared SMBus. These only
    #                 answer while the HOST IS POWERED ON, which is why 0x28
    #                 and 0x37 were missed originally -- every earlier survey
    #                 ran with the host off. None carries a FRU header.
    # NOTE the log prints the address in DECIMAL ("failed to read bus 6 address
    # 55" is 0x37, "bus 1 address 28" is 0x1c) while this file is hex.
    #
    # The motherboard FRU is NOT on bus 1. It is bus 7 / 0x57 -- the eeprom@57
    # declared in the DTS, which the kernel binds and FruDevice reads over sysfs
    # via findI2CEeproms(); that call returns a skipList, so a DT-declared
    # EEPROM is never raw-probed and never needs a blacklist entry. Verified on
    # the live board: /xyz/openbmc_project/FruDevice/X570D4I_2T has BUS=7
    # ADDRESS=87 and carries BOARD_PRODUCT_NAME, while .../FruDevice/1_28 is a
    # bare Inventory.Item.I2CDevice probe stub that makeProbeInterface() creates
    # for any address that ACKs, with no FRU properties at all.
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
