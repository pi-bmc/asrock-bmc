#!/bin/sh
# Re-enable the AST2500 host-facing VGA PCIe function so the KVM can capture
# host (BIOS/UEFI/OS) video.
#
# Why this is needed:
#   The aspeed XDMA engine (&xdma) is intentionally disabled on this board
#   (see aspeed-bmc-asrock-x570d4i2t.dts). The in-kernel aspeed-xdma driver is
#   the only code that programs SCU PCIE_CONF (0x1e6e2180); in its default
#   "bmc" mode it CLEARS the VGA_EN bit and presents the host PCIe device as
#   the non-VGA BMC/XDMA device (1a03:2402, class 0xff) instead of the VGA
#   controller (1a03:2000, class 0x0300). That blanks the KVM. With xdma
#   removed, nothing sets VGA_EN, and the SCU PCIe-config lives in the AST2500
#   "VGA / never-reset" domain — so a cold reset can leave VGA_EN clear.
#
#   This oneshot sets VGA_EN unconditionally at every BMC boot (before the host
#   is powered), guaranteeing the host enumerates the AST2500 VGA and the
#   aspeed-video engine has a signal to capture.
#
# Bits (from drivers/soc/aspeed/aspeed-xdma.c):
#   SCU PCIE_CONF (0x1e6e2180): VGA_EN bit0, VGA_EN_MMIO bit1, VGA_EN_MSI bit3,
#     VGA_EN_IRQ bit5, VGA_EN_DMA bit6  -> vga mask 0x6B
#     BMC_EN bit8, BMC_EN_MSI bit11, BMC_EN_IRQ bit13, BMC_EN_DMA bit14 -> 0x6900
#   SCU BMC_CLASS_REV (0x1e6e219c): class/rev of the host PCIe device.
set -e

[ -e /dev/mem ] || mknod /dev/mem c 1 1

# Unlock the System Control Unit (protection key -> SCU00).
devmem 0x1e6e2000 32 0x1688A8A8

# PCIE_CONF: set the VGA enables, clear the BMC enables, leave other bits.
cur=$(devmem 0x1e6e2180 32)
new=$(( (cur & ~0x696B & 0xFFFFFFFF) | 0x6B ))
devmem 0x1e6e2180 32 "$(printf '0x%08X' "$new")"

# Give the host PCIe device the VGA class (0x030000), rev 1.
devmem 0x1e6e219c 32 0x03000001

logger -t x570-vga-enable "AST2500 host VGA enabled: PCIE_CONF=$(devmem 0x1e6e2180 32)" 2>/dev/null || true
