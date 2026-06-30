# usb-ctrl (phosphor-misc) generates the gadget serial number with
#   sha256sum | cut -b 0-12
# busybox cut is 1-indexed and rejects a 0-based range ("cut: invalid range
# 0-12"), which aborts gadget creation because usb-ctrl runs under 'set -e'.
# Rewrite to '1-12' — identical first-12-bytes result on both GNU and busybox.
# Needed so the meta-bytedance-style `usb-ctrl ecm usb0 on` path works on our
# busybox userland (see recipes-phosphor/usb-network).
do_install:append() {
    if [ -f "${D}${bindir}/usb-ctrl" ]; then
        sed -i 's/cut -b 0-12/cut -b 1-12/' "${D}${bindir}/usb-ctrl"
    fi
}
