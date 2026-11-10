SUMMARY = "OpenBMC for ASRock - Applications"
PR = "r1"

inherit packagegroup

# Only -system is used. The former -chassis / -fans / -flash packages were
# never installed (verified against the image manifest: nothing in the tree
# outside meta-ibm's own machine bbappends requires virtual-obmc-chassis-mgmt /
# -fan-mgmt / -flash-mgmt, so nothing ever pulled them in), which made their
# RDEPENDS misleading -- they advertised a button handler
# (obmc-phosphor-buttons), PID fan control (phosphor-pid-control), a second
# power implementation (phosphor-skeleton-control-power) and IPMB/flash
# providers that are absent from the image. Power control on this board is
# x86-power-control; buttons are handled by its own GPIO config
# (power-config-host0.json).
PROVIDES = "${PACKAGES}"
PACKAGES = " \
        ${PN}-system \
        "

PROVIDES += "virtual/obmc-system-mgmt"

RPROVIDES:${PN}-system += "virtual-obmc-system-mgmt"

SUMMARY:${PN}-system = "ASRock System"
# NOTE: phosphor-power-control and phosphor-power-regulators (from the
# phosphor-power repo) are deliberately NOT listed. Both are IBM POWER
# machine daemons -- the only configs they ship are Rainier / Everest / Fuji /
# BlueRidge / Bonnell / Balcones / Huygens -- and neither has anything to do on
# an AMD AM4 board, yet both installed a multi-user.target unit
# (phosphor-power-control.service is Restart=always). This board's power state
# machine is x86-power-control and its PSU telemetry is supermicro-psu-monitor.
#
# Fan control: phosphor-pid-control (swampd) runs exactly one zone, "GPU",
# from the entity-manager Pid / Stepwise / Pid.Zone records in x570d4i2t.json.
# FAN3 follows the higher of two curves:
#   - "GPU Temp" (gpu-smbpbi-sensor, the T4's SMBPBI post-box), and
#   - "X570 Temp" (W83773G remote diode 1); from its 85 C warning threshold
#     up, FAN3 runs at 100%.
# FAN3 is also 100% whenever the GPU is powered but unreadable. FAN1/FAN2 are
# in no zone and stay at their power-on 100%, so at that threshold every fan
# is at full speed.
RDEPENDS:${PN}-system = " \
        entity-manager \
        virtual-media \
        supermicro-psu-monitor \
        p2a-inventory-monitor \
        gpu-smbpbi-sensor \
        phosphor-pid-control \
        phosphor-software-manager \
        phosphor-software-manager-bios-software-update \
        smbios-mdr \
        phosphor-ipmi-blobs \
        biosconfig-manager \
        asrock-ipmi-oem \
        dbus-sensors \
        phosphor-host-postd \
        phosphor-post-code-manager \
        "
