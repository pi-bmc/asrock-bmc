# psusensor stays disabled: the PWS-505P-1H is not PMBus (flat register map);
# its sensors come from the supermicro-psu-monitor daemon (recipes-asrock).
PACKAGECONFIG = " \
        adcsensor \
        fansensor \
        hwmontempsensor \
        ipmbsensor \
        nvmesensor \
        "
