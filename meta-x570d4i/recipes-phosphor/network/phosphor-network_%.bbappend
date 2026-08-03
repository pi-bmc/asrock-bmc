# ---------------------------------------------------------------------------
# Disable IPv4 link-local autoconfiguration (Redfish Host Interface)
# ---------------------------------------------------------------------------
# The host interface lives on usb0 at 169.254.0.17/16 -- link-local by
# necessity: the stock BIOS has "https://169.254.0.17/redfish/v1/" compiled in
# (a literal in the DXE volume), and it asks the BMC for that same address over
# IPMI "Get LAN Configuration Parameters, channel 8, parameter 3" during POST.
# Two things in phosphor-network break that, and BOTH are gated on the
# LINK_LOCAL_AUTOCONFIGURATION build flag:
#
#   1. addAddr() classifies ANY scope-link IPv4 as AddressOrigin.LinkLocal
#      (ethernet_interface.cpp:189-235). The IPMI transport handler only returns
#      Static- or DHCP-origin addresses (transporthandler.cpp originsV4), so
#      channel 8 parameter 3 reads 0.0.0.0 instead of 169.254.0.17. systemd
#      assigns RT_SCOPE_LINK to 169.254.x automatically and phosphor-network
#      never emits "Scope=", so scope-link is unavoidable while it manages usb0.
#
#   2. The config writer emits "LinkLocalAddressing=yes" from the same flag
#      (ethernet_interface.cpp:905) -- the LinkLocalAutoConf D-Bus property is
#      IGNORED. IPv4LL then adds a second 169.254.x address that becomes primary
#      and demotes ours to "secondary".
#
# Worse, ethernet_interface.cpp:218 calls deleteLinkLocalIPv4ViaNetlink() on a
# scope-link IPv4 once the link reports routable -- it would DELETE 169.254.0.17
# at exactly the moment the host finishes enumerating the USB gadget.
#
# Turning the flag off compiles out that whole block: the address keeps the
# Static origin it is initialised with, no IPv4LL companion is configured, and
# nothing reaps it.
#
# SCOPE: global, so eth0/eth1 also lose IPv4LL fallback. They are DHCP on a
# managed network, where a 169.254.x self-assignment is not useful anyway.
#
# HISTORY: this previously used IGNORED_INTERFACES=usb0 to hand usb0 to
# systemd-networkd outright. That produced a correct address but removed the
# /xyz/openbmc_project/network/usb0 D-Bus object, which made IPMI channel 8 fail
# -- and a full POST capture then showed the BIOS querying channel 8 three times
# (parameter 3 twice, parameter 0x38 once). The earlier capture suggesting it
# never did was from a POST that aborted at the AMI OEM handshake before
# reaching RHI discovery.
EXTRA_OEMESON:append = " -Ddefault-link-local-autoconf=false -Ddefault-ipv6-accept-ra=false"
