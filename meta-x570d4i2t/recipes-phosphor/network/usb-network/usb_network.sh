#!/bin/bash

set -e

GADGET_DIR="/sys/kernel/config/usb_gadget/obmc_redfish"

BMC_MAC="02:00:16:92:54:17"
HOST_MAC="02:00:16:92:54:18"

# 1. Create the Gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

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
ln -s functions/rndis.usb0 configs/c.1/
ln -s configs/c.1 os_desc

# 7. Bind the Gadget to the ASPEED Virtual Hub Hardware
# The AST2500 vhub exposes its virtual ports as UDCs (USB Device Controllers).
# "1e6a0000.usb-vhub:p1" is Port 1 of the AST2500 vhub.
echo "1e6a0000.usb-vhub:p1" > UDC

# 8. Addressing is intentionally NOT done here.
# systemd-networkd owns the usb0 address via 00-bmc-usb0.network, which assigns
# 169.254.0.17/16 with **scope global** so phosphor-network classifies it as
# AddressOrigin.Static (required for IPMI "Get LAN Config ch8" to return the IP
# to the host BIOS Redfish Host Interface). busybox `ip addr ... scope global`
# silently applies scope LINK instead, which phosphor tags LinkLocal and the
# transport handler then drops -> Get LAN returns 0.0.0.0. So leave it to networkd.
