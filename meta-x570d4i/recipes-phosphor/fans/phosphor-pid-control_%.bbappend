# FAN3 is driven open-loop from "GPU Temp" (see the GPU zone in
# x570d4i2t.json) and is listed in MissingIsAcceptable, so a fan whose tach
# reads nothing does not hold the zone in failsafe. Upstream never matched
# MissingIsAcceptable against fan inputs, which the zone knows by their
# paired fan+pwm name; see the patch header.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " file://0001-dbusconfiguration-honour-MissingIsAcceptable-for-fan-inputs.patch"
