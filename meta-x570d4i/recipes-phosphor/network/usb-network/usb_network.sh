#!/bin/bash

set -e

GADGET_DIR="/sys/kernel/config/usb_gadget/obmc_redfish"
UDC_NAME="1e6a0000.usb-vhub:p1"

BMC_MAC="02:00:16:92:54:17"
HOST_MAC="02:00:16:92:54:18"

# ---------------------------------------------------------------------------
# 0. Bounded waits (2026-08-03)
# ---------------------------------------------------------------------------
# usb-network.service now runs in the sysinit phase, ahead of basic.target, so
# that usb0 exists before the host powers on. Two things that used to be
# implicitly true that late in boot are no longer guaranteed, so wait for them:
#
#   * the UDC -- aspeed_vhub is builtin but probed asynchronously (t=22.2s on
#     this board), and nothing orders us after that probe;
#   * the netdev -- registration is asynchronous with the UDC write below, and
#     Type=oneshot only means "usb0 exists" if we actually wait for it.
#
# Both are polled with a ceiling rather than blocking forever; the unit also
# carries TimeoutStartSec as a backstop. busybox sleep handles fractional
# seconds here (verified on target).
wait_for() {
    local path=$1
    local tries=${2:-100} # 100 * 0.1s = 10s
    while [ "$tries" -gt 0 ]; do
        if [ -e "$path" ]; then
            return 0
        fi
        tries=$((tries - 1))
        sleep 0.1
    done
    return 1
}

if ! wait_for "/sys/class/udc/$UDC_NAME"; then
    echo "usb_network: UDC $UDC_NAME never appeared" >&2
    exit 1
fi

# 1. Create the Gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

# Idempotent: this is a oneshot in the boot path, and it may also be re-run by
# hand while debugging. If the gadget is already bound to a UDC there is
# nothing to do -- rewriting the descriptors of a live gadget is rejected by
# configfs anyway. Note an *unbound* UDC file still reads back as a newline, so
# test the stripped contents rather than using `test -s`.
current_udc=$(cat UDC 2>/dev/null || true)
current_udc=${current_udc//[[:space:]]/}
if [ -n "$current_udc" ]; then
    echo "usb_network: gadget already bound to $current_udc"
    exit 0
fi

# idVendor 0x046B (AMI) / idProduct 0xFFB0 are MANDATORY: the host BIOS's
# RedfishHi DXE driver gates host-interface bring-up on EXACTLY this USB VID/PID
# (UsbGetDeviceDescriptor: cmp idVendor,0x046B; cmp idProduct,0xFFB0). With any
# other VID/PID it refuses to bind the NIC, never assigns 169.254.0.18, and the
# whole RHI credential/Redfish flow stalls. (Reverse-engineered from RedfishHi.efi
# @0x6b0f/0x6b1b — these are the only USB VID/PID literals in that binary.)
echo 0x046b > idVendor  # AMI (required by host RedfishHi driver)
echo 0xffb0 > idProduct # AMI Redfish Host Interface RNDIS device
echo 0x0200 > bcdUSB    # USB 2.0
echo 0x0100 > bcdDevice # v1.0.0

# Device-class triple = 0x02/0x00/0x00 (Communications/CDC), matching the STOCK
# AMI MegaRAC gadget (eth.ko CreateEthernetDescriptor emits exactly this: device
# class 0x02, sub 0x00, proto 0x00, flat CDC-ACM, NO IAD). The X570D4I-2T BIOS's
# EDK2 UsbRndis driver binds the RNDIS control interface (0x02/0x02/0xFF) + data
# interface (0x0A) regardless, and Windows still loads RNDIS via the MS OS
# descriptors below. We previously used the Microsoft single-interface 0xEF/0x04/0x01
# class, which differs from the stock identity the BIOS was validated against.
echo 0x02 > bDeviceClass
echo 0x00 > bDeviceSubClass
echo 0x00 > bDeviceProtocol

# 3. Set standard string descriptors
mkdir -p strings/0x409
echo "ASRock Rack" > strings/0x409/manufacturer
echo "X570D4I-2T BMC" > strings/0x409/product
echo "0000000000" > strings/0x409/serialnumber

# 4. Create the Configuration
mkdir -p configs/c.1/strings/0x409
echo "Redfish Host Interface" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower # 500mA max draw

# 5. Create the RNDIS Function (Windows-compatible host interface).
mkdir -p functions/rndis.usb0

# (Optional) Set MAC addresses. If omitted, random ones are generated.
# Host MAC is what the OS sees; dev_addr is what the BMC sees.
echo "$HOST_MAC" > functions/rndis.usb0/host_addr
echo "$BMC_MAC" > functions/rndis.usb0/dev_addr

# 5a. Microsoft OS descriptors so a Windows / AMI-BIOS host auto-loads the RNDIS
# driver with no .inf. sub_compatible_id 5162001 selects the RNDIS 6.0 driver.
echo 1       > os_desc/use
echo 0xcd    > os_desc/b_vendor_code
echo MSFT100 > os_desc/qw_sign
echo RNDIS   > functions/rndis.usb0/os_desc/interface.rndis/compatible_id
echo 5162001 > functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id

# 6. Link the RNDIS function to the configuration, and link the config into
# os_desc (REQUIRED — without it the host never requests the MS OS descriptor).
# Guarded so a partially-created gadget (script interrupted, or the UDC bind
# below having failed on a previous run) can be completed rather than aborting
# on an existing symlink.
[ -e configs/c.1/rndis.usb0 ] || ln -s functions/rndis.usb0 configs/c.1/
[ -e os_desc/c.1 ] || ln -s configs/c.1 os_desc

# 7. Bind the Gadget to the ASPEED Virtual Hub Hardware
# The AST2500 vhub exposes its virtual ports as UDCs (USB Device Controllers).
# "1e6a0000.usb-vhub:p1" is Port 1 of the AST2500 vhub.
echo "$UDC_NAME" > UDC

# 7a. Do not report success until usb0 is really there. The whole point of the
# sysinit-phase ordering is that phosphor-network's startup enumeration and
# ipmid's channel-8 lookup both find the interface; returning early would put
# the race straight back.
if ! wait_for /sys/class/net/usb0; then
    echo "usb_network: usb0 did not appear after binding $UDC_NAME" >&2
    exit 1
fi

# 8. Addressing is intentionally NOT done here.
# systemd-networkd owns the usb0 address via 00-bmc-usb0.network, which assigns
# 169.254.0.17/16 with **scope global** so phosphor-network classifies it as
# AddressOrigin.Static (required for IPMI "Get LAN Config ch8" to return the IP
# to the host BIOS Redfish Host Interface). busybox `ip addr ... scope global`
# silently applies scope LINK instead, which phosphor tags LinkLocal and the
# transport handler then drops -> Get LAN returns 0.0.0.0. So leave it to networkd.
