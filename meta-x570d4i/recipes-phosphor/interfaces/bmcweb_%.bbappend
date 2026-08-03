# bmcweb tuning for the X570D4I-2T.
#
# Two BIOS-config transports converge on the same D-Bus properties
# (xyz.openbmc_project.BIOSConfig.Manager BaseBIOSTable / PendingAttributes),
# so there is exactly one source of truth:
#
#   0001 — in-band over KCS. The host pushes its attribute table via
#          asrock-ipmi-oem's BIOS-config commands; this patch re-adds the /Bios
#          "Attributes" view upstream removed, surfacing live config at
#          /redfish/v1/Systems/system/Bios.
#
#   0002 — the AMI Redfish Host Interface. The STOCK host BIOS already contains
#          a Redfish client (RedfishHi + FirmwareConfigDrv + AmiRedfishDynExt);
#          given these routes on the USB host-interface link it publishes its
#          own registry and settings and pulls staged writes back, with no
#          firmware modification. That is the path intended to retire the
#          injected DXE drivers. AMI's dialect is not standard Redfish, so it
#          cannot be served by upstream routes; and it is unauthenticated by
#          design, contained to connections accepted on 169.254.0.17. Endpoint
#          set extracted from the stock BIOS binaries — see
#          asrock-bmc/.claude/rhi-http-contract.md.
#
# SMBIOS still arrives over IPMI (blob handler; see smbios-mdr_%.bbappend).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += " \
    file://0001-redfish-bios-attributes-from-basebiostable.patch \
    file://0002-redfish-ami-host-interface-routes.patch \
    "

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
