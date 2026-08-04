SUMMARY = "OpenBMC Virtual Media daemon"
DESCRIPTION = "\
Serves an image to the host as a USB mass-storage device. Legacy mode has the \
BMC fetch the image itself from an HTTPS or CIFS URL; proxy mode streams it \
from the browser over a websocket. Either way the image is exported through \
nbd and attached to a configfs USB gadget on the AST2500 vhub, so the host \
BIOS sees an ordinary USB device it can boot from -- a removable disk, or a \
CD/DVD for a slot with Cdrom set. \
\
This backs Redfish /redfish/v1/Managers/bmc/VirtualMedia, and it is what makes \
the Cd and Usb boot-source overrides resolvable: with no media attached the \
BIOS builds no CD or removable boot entry, so those overrides silently fall \
through to the next device. \
"

# Upstream lives in Intel's OpenBMC fork, not openbmc/. There is no
# openbmc/virtual-media repository -- bmcweb carries the Redfish client for
# this D-Bus interface but no daemon implementing it was ever upstreamed,
# which is also why BMCWEB_VM_NBDPROXY is hardcoded false and needs the bmcweb
# bbappend in this layer to turn back on. The fork is archived, so these
# patches are permanent rather than pending an uprev. README.md in this
# directory is upstream's design document, carrying a note on where it has
# gone stale relative to the code.
HOMEPAGE = "https://github.com/Intel-BMC/virtual-media"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e3fc50a88d0a364313df4b21ef20c29e"

SRC_URI = " \
    git://github.com/Intel-BMC/virtual-media.git;branch=main;protocol=https \
    file://0001-build-against-modern-boost-and-sdbusplus.patch \
    file://0002-allow-mount-point-to-present-as-cdrom.patch \
    file://0003-legacy-mount-take-bare-unix-fd.patch \
    file://0004-securecleanup-guard-empty-container.patch \
    file://0005-skip-credential-args-when-unauthenticated.patch \
    file://0006-cache-and-readahead-for-curl-plugin.patch \
    file://0007-install-daemon-to-sbindir.patch \
"
SRCREV = "1306c2132bdb9d5ad6deb00cd8a1d920753f7ea7"
PV = "0.1+git"

# systemd PROVIDES udev; naming both would be the same recipe twice.
DEPENDS = "boost nlohmann-json sdbusplus systemd"

inherit meson pkgconfig systemd

# The tree predates several warnings this toolchain emits by default, and
# werror=true would turn every one of them into a build failure for code we are
# not changing. (legacy-mode is already 'enabled' in upstream's meson_options.)
EXTRA_OEMESON = " \
    -Dwerror=false \
    -Dtests=disabled \
"

SYSTEMD_SERVICE:${PN} = "xyz.openbmc_project.VirtualMedia.service"

# The daemon does no I/O itself: it shells out to /usr/sbin/nbdkit to serve the
# image over a unix socket (curl plugin for an https:// URL, file plugin for a
# file on a mounted CIFS share) and to /usr/sbin/nbd-client to attach that
# socket to /dev/nbdN. Neither path is exercised until a mount is attempted, so
# a missing binary shows up as a failed InsertMedia rather than as a service
# that will not start.
RDEPENDS:${PN} += "nbd-client nbdkit"
