# psusensor reads the PSU via the pws505p kernel hwmon driver (flat-register
# Supermicro PWS-505P-1H at i2c2/0x38, pmbus-style labels) through the
# entity-manager "pmbus" record in supermicro-pws-505p-1h.json.
PACKAGECONFIG = " \
        adcsensor \
        fansensor \
        hwmontempsensor \
        ipmbsensor \
        nvmesensor \
        psusensor \
        "
