# Drop the closed-loop fan-control stack on this board.
#
# The X570D4I-2T does not do BMC fan control: the NCT6779 Super-I/O owns
# FAN1/2/3 PWM through its built-in Smart Fan curve, driven by the host BIOS
# over LPC (see the &i2c1 note in aspeed-bmc-asrock-x570d4i2t.dts). The BMC
# binds nct6775-i2c read-only.
#
# Upstream packagegroup-obmc-apps-fan-control pulls
# ${VIRTUAL-RUNTIME_obmc-fan-control} (= phosphor-fan-control) plus a
# hard-coded phosphor-fan-monitor, and their init units are wanted by
# obmc-chassis-poweron@.target -- so both started on every host power-on with
# no configuration at all: the image ships no /usr/share/phosphor-fan-presence
# directory, because no fan-control/monitor JSON was ever written for this
# machine. Clearing the RDEPENDS removes both daemons rather than leaving them
# to start and fail.
#
# Fan TACH readings are unaffected: those come from dbus-sensors' fansensor
# (the AspeedFan exposes in x570d4i2t.json), which is a separate package.
RDEPENDS:${PN}-fan-control = ""
