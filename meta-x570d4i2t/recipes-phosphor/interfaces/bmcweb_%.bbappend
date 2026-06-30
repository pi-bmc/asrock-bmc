# bmcweb tuning for the X570D4I-2T. No OEM Redfish Host Interface routes are
# added on this board: the in-band USB-NIC / Redfish Host Interface approach was
# removed entirely. SMBIOS arrives over IPMI (AMI-MDR, handled in
# asrock-ipmi-oem); BIOS configuration is served by the stock bmcweb /Bios
# routes backed by xyz.openbmc_project.BIOSConfigManager (no host-push path).

# Disable bmcweb's zstd HTTP compression.
#
# Upstream bmcweb 8a32dac3 (and surrounding commits) attempts zstd compression
# when the client offers it in Accept-Encoding, but the fallback path when
# ZSTD_createCCtx() fails returns an empty body instead of falling back to
# gzip/br. Result: any modern browser (Chrome 123+, Firefox 126+) sees a
# blank page because the HTML/JS responses are 0 bytes.
#
# Disabling http-zstd here forces bmcweb to use gzip/br only — which is
# what every browser already supports, and which works reliably.
#
# Once upstream bmcweb is fixed (correct fallback when zstd init fails, or
# the zstd init issue itself is resolved on ARM AST2500), this bbappend can
# be reverted.

PACKAGECONFIG:remove = "http-zstd"

# Default 30 MB upstream HTTP body limit is too small for our 64 MB BMC image
# tarball uploaded via Redfish UpdateService HttpPushUri / MultipartHttpPushUri.
# Bump to the upstream meson-options max (512 MB) so the firmware push
# endpoint accepts the full image without a 30 MB silent truncation.
EXTRA_OEMESON:append = " -Dhttp-body-limit=512"
