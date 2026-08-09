/** @file
  OobInventoryBlob.h — the on-wire contract for host->BMC storage/PCIe inventory
  transferred over the AST2500 PCIe-to-AHB (P2A) bridge.

  The host BIOS (OpenOobPkg/InventoryOobDxe) enumerates PCIe functions and NVMe
  drives at ReadyToBoot and writes this blob into a fixed, DT-reserved window of
  BMC DRAM. A BMC daemon (recipes-asrock/p2a-inventory-monitor) maps that same
  window, validates the blob, and republishes it as phosphor Inventory.Item.*
  objects that bmcweb turns into Redfish Storage / Drive / StorageController /
  PCIeDevice resources — the path the dead M.2 SMBus sideband could never feed.

  THIS FILE IS DUPLICATED, BYTE-FOR-BYTE, on the BMC side at
  meta-x570d4i/recipes-asrock/p2a-inventory-monitor/files/oob-inventory-blob.h
  (the two repos do not share an include tree). Any change here MUST be mirrored
  there, and the FORMAT_VERSION bumped, or the daemon will reject the blob.

  All multi-byte integers are little-endian. Both the AST2500 AHB and the x86
  host are little-endian, so the bytes land unswapped. Strings are fixed-width,
  space- or NUL-padded exactly as NVMe Identify returns them; the consumer trims.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OOB_INVENTORY_BLOB_H_
#define OOB_INVENTORY_BLOB_H_

#include <stdint.h>

/*
 * Where the blob lives in BMC DRAM.
 *
 * This is the base of the `pci_memory` reserved-memory region declared in
 * aspeed-bmc-asrock-x570d4i2t.dts (region@9a000000, no-map, 64 KiB) and wired to
 * &p2a via `memory-region`. The BMC's aspeed-p2a-ctrl driver reports exactly
 * this base/length through ASPEED_P2A_CTRL_IOCTL_GET_MEMORY_CONFIG, so the
 * daemon never hard-codes it — but the HOST has no such lookup, so the address
 * is fixed by contract here. If the DT region moves, both sides change.
 *
 * The host reaches it by pointing the P2A remap window (RBAR) at
 * OOB_INV_BMC_PHYS_ADDR & 0xFFFF0000 and writing through the AST2500 VGA BAR1
 * MMIO aperture. RBAR carries bits [31:16], so the region must stay 64 KiB
 * aligned (it is).
 */
#define OOB_INV_BMC_PHYS_ADDR   0x9A000000u
#define OOB_INV_REGION_LEN      0x00010000u   /* 64 KiB, matches the DT reg size */

/* "OOBINV\0\0" as a little-endian u64, so a single load identifies the blob. */
#define OOB_INV_MAGIC           0x00565049424F4FULL   /* 'O','O','B','I','N','V',0,0 */
#define OOB_INV_FORMAT_VERSION  0x0001u

/* Sanity ceilings so a corrupt count can never walk us off the region. */
#define OOB_INV_MAX_PCIE        64u
#define OOB_INV_MAX_DRIVES      16u

/* DriveProtocol / MediaType small enums (mapped to phosphor strings on the BMC). */
#define OOB_INV_PROTO_NVME      0u
#define OOB_INV_MEDIA_SSD       1u
#define OOB_INV_MEDIA_HDD       2u

/* PCIe DeviceType (mapped to Inventory.Item.PCIeDevice.DeviceTypes.*). */
#define OOB_INV_PCIE_SINGLE_FN  1u
#define OOB_INV_PCIE_MULTI_FN   2u

/* Sentinel for "not determined" in a PCIe location byte. */
#define OOB_INV_LOC_UNKNOWN     0xFFu

#pragma pack(1)

/*
 * Blob header. `Crc32` covers the ENTIRE blob (header + all records, i.e.
 * TotalSize bytes) computed with the Crc32 field itself treated as zero. Both
 * sides use the standard reflected CRC-32 (poly 0xEDB88420, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF).
 *
 * `BootSerial` changes every host boot (low bits of the TSC at driver entry).
 * The daemon uses it only to notice a fresh push; it is NOT part of validity.
 */
typedef struct {
  uint64_t Magic;         /* OOB_INV_MAGIC */
  uint16_t FormatVersion; /* OOB_INV_FORMAT_VERSION */
  uint16_t HeaderSize;    /* sizeof(OOB_INV_HEADER) */
  uint32_t TotalSize;     /* header + PcieCount*rec + DriveCount*rec */
  uint32_t Crc32;         /* over [0..TotalSize) with this field = 0 */
  uint32_t BootSerial;    /* per-boot nonce; freshness only */
  uint16_t PcieRecSize;   /* sizeof(OOB_INV_PCIE_RECORD), for fwd-compat */
  uint16_t DriveRecSize;  /* sizeof(OOB_INV_DRIVE_RECORD) */
  uint16_t PcieCount;
  uint16_t DriveCount;
  uint32_t Reserved;
  /* OOB_INV_PCIE_RECORD[PcieCount] then OOB_INV_DRIVE_RECORD[DriveCount] */
} OOB_INV_HEADER;

typedef struct {
  uint16_t Segment;
  uint8_t  Bus;
  uint8_t  Device;
  uint8_t  Function;
  uint8_t  DeviceType;       /* OOB_INV_PCIE_SINGLE_FN / MULTI_FN */
  uint16_t VendorId;
  uint16_t DeviceId;
  uint16_t SubsysVendorId;
  uint16_t SubsysId;
  uint8_t  ClassBase;        /* PCI class code byte 2 */
  uint8_t  ClassSub;         /* byte 1 */
  uint8_t  ClassProgIf;      /* byte 0 */
  uint8_t  RevisionId;
  uint8_t  GenerationInUse;  /* negotiated PCIe gen 1..5, 0 = unknown */
  uint8_t  GenerationMax;    /* capable PCIe gen 1..5, 0 = unknown */
  uint8_t  LanesInUse;       /* negotiated width, 0 = unknown */
  uint8_t  LanesMax;         /* capable width, 0 = unknown */
} OOB_INV_PCIE_RECORD;

typedef struct {
  /* PCIe location of the owning NVMe controller, or 0xFF sentinels if the
     driver could not resolve it. Lets the daemon cross-link a Drive to its
     PCIeDevice; purely informational. */
  uint16_t Segment;
  uint8_t  Bus;
  uint8_t  Device;
  uint8_t  Function;
  uint8_t  Protocol;         /* OOB_INV_PROTO_NVME */
  uint8_t  MediaType;        /* OOB_INV_MEDIA_SSD */
  uint8_t  SmartCritWarning; /* SMART byte 0; 0 == healthy */
  uint32_t NamespaceId;
  uint64_t CapacityBytes;    /* NSZE * (1 << LBADS) */
  uint32_t BlockSizeBytes;   /* 1 << LBADS */
  uint16_t CompositeTempK;   /* SMART bytes 1..2, Kelvin */
  uint8_t  PercentageUsed;   /* SMART byte 5 */
  uint8_t  AvailableSpare;   /* SMART byte 3, percent */
  char     Model[40];        /* Identify Controller MN */
  char     Serial[20];       /* Identify Controller SN */
  char     Firmware[8];      /* Identify Controller FR */
} OOB_INV_DRIVE_RECORD;

#pragma pack()

/*
 * Standard reflected CRC-32 (zlib/PNG), table-free so it drops into both a
 * freestanding UEFI driver and a Linux daemon with no dependency. `crc` starts
 * and is compared as the final value (init + final-XOR folded in by callers via
 * the one-shot wrapper below).
 */
static inline uint32_t OobInvCrc32 (const void *data, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t i, b;
  for (i = 0; i < len; i++) {
    crc ^= p[i];
    for (b = 0; b < 8; b++) {
      uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88420u & mask);
    }
  }
  return ~crc;
}

#endif /* OOB_INVENTORY_BLOB_H_ */
