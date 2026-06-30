# X570D4I-2T host BIOS firmware update support.
#
# The generic flasher (bios-update.sh), obmc-flash-host-bios@.service and the
# "flash_bios" PACKAGECONFIG all come from meta-asrock/meta-common; this layer
# only supplies the board-specific env config (bios-update), which meta-common
# installs to /etc/default/bios-update.  See that file for the GPIOJ1 mux + the
# "bios" MTD details.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += "file://bios-update"
