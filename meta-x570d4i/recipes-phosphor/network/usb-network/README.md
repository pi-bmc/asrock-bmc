# usb-network — usb0 Redfish Host Interface gadget (X570D4I-2T)

Brings up `usb0`, the BMC-side USB network gadget the host BIOS uses for the Redfish
Host Interface (RHI). BMC = **169.254.0.17/16**.

## Files

- `usb_network.sh` — creates the configfs USB gadget `obmc_redfish` on the AST2500
  vhub (`1e6a0000.usb-vhub:p1`).
- `00-bmc-usb0.network` — systemd-networkd: static `169.254.0.17/16`, **scope
  global**, `ConfigureWithoutCarrier`, always up. Scope global is required so
  phosphor-network reports `AddressOrigin.Static` for IPMI "Get LAN ch8" (busybox
  `ip ... scope global` silently applies scope LINK, which gets tagged LinkLocal and
  Get-LAN returns 0.0.0.0 — so networkd owns the address, the gadget script does not).

## Must be RNDIS

The BIOS ships exactly ONE USB-network class driver, `UsbRndisDriverSrc [11e32c34]`
(RNDIS; no CDC-ECM/NCM). So the gadget MUST present RNDIS. `usb_network.sh` uses
`functions/rndis.usb0` (Linux f_rndis) + Microsoft OS descriptors (`os_desc/use=1`,
`b_vendor_code=0xcd`, `qw_sign=MSFT100`, `interface.rndis/compatible_id=RNDIS`,
`sub_compatible_id=5162001`). The BIOS `UsbRndis.Supported()` binds interface
class/subclass/proto `0x02/0x02/0xFF` | `0xEF/0x04/0x01` | `0x0A/0x00/0x00`, all of
which f_rndis already presents — so the **gadget identity is correct**; it is not the
blocker. Kernel needs `CONFIG_USB_CONFIGFS_RNDIS=y` (+`USB_F_RNDIS`) in `x570d4i2t.cfg`.

## Important vhub caveat

On the AST2500 vhub, the gadget's configfs `UDC state` and the `usb0` carrier stay
`configured`/`1` **even when the host is powered OFF** — they are vhub-driven, not
host-driven. Do NOT use them to infer that the host has enumerated/bound the gadget.
Reliable host-activity signals are: `usb0` `rx_packets`, `RNDIS_MSG_INIT` in BMC
dmesg (enable `echo 'file rndis.c +p' > /sys/kernel/debug/dynamic_debug/control`),
and the BIOS KCS `0x2C` request appearing in the ipmid log.

## Status

The gadget binds and the BIOS enumerates it, but (pre-RHI-IPMI-fix) the host never
sent `RNDIS_MSG_INIT` (enumerate-but-no-data) and rx stayed 0. The gating turned out
to be the **KCS IPMI RHI credential handshake** (NetFn 0x2C), not the gadget — see
[`asrock-ipmi-oem/README.md`](../../ipmi/asrock-ipmi-oem/README.md) and
[`../../../RHI-HANDOFF.md`](../../../RHI-HANDOFF.md).
