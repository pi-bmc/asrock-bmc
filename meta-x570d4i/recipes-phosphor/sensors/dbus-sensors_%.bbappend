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
#
# The board's Super-I/O hwmon part is an NCT6796D, not an NCT6779, and
# nct6775_i2c_probe() trusts the i2c_device_id without checking the chip ID --
# so the type has to be selectable for the right temp mask/labels to be used.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://0001-hwmontemp-add-NCT6796-sensor-type.patch"

PACKAGECONFIG = " \
        adcsensor \
        fansensor \
        hwmontempsensor \
        ipmbsensor \
        "
