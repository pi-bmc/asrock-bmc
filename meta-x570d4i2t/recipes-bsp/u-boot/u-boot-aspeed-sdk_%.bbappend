FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Keep the AST2500 LPC Super-I/O decode + iLPC2AHB bridge enabled so the host
# BIOS Redfish Host Interface probe (Super-I/O 0x4E/0x4F -> LDN 0x0D iLPC2AHB ->
# read GPIO E4) can succeed and the host brings up Redfish-over-USB on usb0.
SRC_URI:append:x570d4i2t = " \
    file://enable-superio.cfg \
    file://0001-ast2500-keep-ilpc2ahb-enabled-with-superio.patch \
"
