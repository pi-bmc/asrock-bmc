# psusensor stays disabled: the PWS-505P-1H is not PMBus (flat register map);
# its sensors come from the supermicro-psu-monitor daemon (recipes-asrock).
#
# nvmesensor stays disabled too: nothing answers the NVMe-MI basic-management
# address 0x6a on ANY of the PCA9545 channels (i2cdetect on buses 40-43 shows
# only the mux itself at 0x70, and i2ctransfer to 0x6a returns ENXIO), so the
# M.2 SMBus sideband is not wired/populated on this board. With it enabled the
# daemon retried ~5x/sec forever and logged "Failed to read block data from
# device '0x6a' on bus '41'". The matching NVME1000 expose was removed from
# x570d4i2t.json.
PACKAGECONFIG = " \
        adcsensor \
        fansensor \
        hwmontempsensor \
        ipmbsensor \
        "
