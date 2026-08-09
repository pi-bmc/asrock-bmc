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

# fansensor unconditionally opens pwmN_enable to take the channel out of
# automatic regulation, which only I2C fan controllers like the MAX31790 have.
# Our fans hang off the AST2500's own PWM/tach block (aspeed-pwm-tacho), which
# exposes just pwmN and fanN_input -- always manual, nothing to switch -- so all
# three headers logged "Error read/write '.../pwmN_enable'" on every config
# reload while working perfectly. The patch skips a missing file silently, the
# way enableFanInput() already handles a missing fanN_enable.
#
# 0002 silences one "error getting SpecialMode status: 'No route to host'" per
# sensor daemon: that service is Intel's manufacturing-mode manager and does not
# exist here, which the code already treats correctly -- only the log level was
# wrong. 0003 stops fansensor reporting its own startup race against
# entity-manager as a missing fan configuration.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += " \
    file://0001-fan-do-not-log-an-error-when-pwmN_enable-is-absent.patch \
    file://0002-Utils-an-absent-SpecialMode-service-is-not-an-error.patch \
    file://0003-fan-don-t-report-a-startup-race-as-a-missing-config.patch \
    "
