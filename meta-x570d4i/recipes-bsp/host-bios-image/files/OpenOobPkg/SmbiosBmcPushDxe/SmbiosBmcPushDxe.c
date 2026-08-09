/** @file
  SmbiosBmcPushDxe — push host-side platform data to the BMC. Two independent
  payloads, deliberately carried by ONE module:

    1. SMBIOS  — the structure table, over phosphor-ipmi-blobs (blob "/smbios")
                 on the host KCS interface (LPC I/O 0xCA2/0xCA3).
    2. Inventory — PCIe function and NVMe drive topology, over the AST2500
                 PCIe-to-AHB (P2A) bridge into BMC DRAM. Feeds Redfish
                 Storage / Drive / StorageController / PCIeDevice, which the BMC
                 can populate no other way: this board routes no NVMe-MI SMBus
                 or MCTP sideband to the BMC (every channel of the i2c0 PCA9545
                 is silent, SATA is invisible), so nvmesensor and phosphor-nvme
                 have nothing to read. DXE does — PciIo and NvmExpressPassThru
                 describe every device — so the data is gathered here and pushed.

  WHY ONE MODULE. This driver reaches the flash by REPLACING AMI's
  SendInfoBmcIpmiDxe FFS slot (9DF02DFD), which is on the recipe's default
  OOB_REPLACE_SLOTS path and needs no free space, no strip profile and no new
  FFS file. A second driver would have to be grafted in as a NEW file, which
  only happens under HOST_BIOS_STRIP_OOB=1 with a strip profile — a path that has
  never been booted on this board. Carrying the inventory payload here is what
  makes it reach hardware at all. Cost of the coupling: an inventory fault would
  take the working SMBIOS push down with it, so SMBIOS runs FIRST and every
  inventory failure is non-fatal and never touches the SMBIOS telemetry fields.

  TIMING — the two payloads are NOT ready at the same moment, and this is the
  whole reason the scheduling below looks redundant:
    - SMBIOS is available in DXE (protocol, or the config table AMI fills).
    - NVMe is NOT. NvmExpressPassThru only exists once NvmExpressDxe has bound
      the controller, which happens when BDS calls ConnectController — AFTER DXE
      dispatch. At our entry point there is never an NVMe handle to find. The
      trigger that works is a protocol notify on NvmExpressPassThru: it is
      signalled synchronously inside InstallProtocolInterface the moment a drive
      becomes enumerable, so unlike a timer it does not depend on the tick, and
      unlike ReadyToBoot it does not depend on BDS reaching the end.
  Consequently every notify is armed UNCONDITIONALLY and each payload guards
  itself with its own completion flag. Do not reintroduce a shared "done" gate:
  the previous single mDone meant a successful SMBIOS push at entry skipped
  arming any notify at all, which would have left the inventory payload with one
  shot at the one moment NVMe cannot exist.

  Historical note: the comments that used to assert EndOfDxe/ReadyToBoot "never
  fire on this board" date from the era when the board stalled at POST 0x99 and
  DXE never completed. Above-4G Decoding fixed that; the 2026-08-05 boot profile
  measures a ReadyToBoot->ExitBootServices window, so ReadyToBoot does fire now.
  It is still not RELIED on here — our 0x76 marker aliases AMI's own codes, so
  port 80 cannot prove our callback ran. The telemetry NVAR settles it.

  SMBIOS wire protocol (validated end-to-end by SmbiosBmcPushApp):
    IPMI OEM/Group NetFn 0x2E, Cmd 0x80, OEN 0xCF 0xC2 0x00
    request : [OEN(3)][subcmd][CRC16(payload) LE][payload]
    response: [OEN(3)][CRC16(2)][data]            (after the completion code)
    Open 0x02 / Write 0x04 / Commit 0x05 / Close 0x06
    Commit payload: [session(2)][commit_data_len(1)][...]
    CRC16: CCITT poly 0x1021 init 0xFFFF, 2 extra rounds, over payload
  Data pushed: [SMBIOS structure table (Type 0..127)][synthesized "_SM3_" entry
  point AT THE END].  smbios-mdr parses structures from byte 0 (so the table
  must start there or Type 0/version is skipped) yet also requires an
  "_SM_"/"_SM3_" anchor somewhere in the buffer (checkSMBIOSVersion); the
  trailing entry point satisfies both.  A LEADING entry point breaks Type 0.

  Copyright (c) 2024, ASRockRack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Guid/EventGroup.h>
#include <Guid/SmBios.h>
#include <Protocol/Smbios.h>
#include <Protocol/PciIo.h>
#include <Protocol/NvmExpressPassthru.h>
#include <IndustryStandard/Pci.h>

/* KCS transport and the POST-code helper. This driver owns neither — the
   interface state machine lives in the library so there is exactly one copy. */
#include <Library/IpmiKcsLib.h>

/* Progress is recorded to an NV variable, not to port 0x80. On this board a
   driver that provably ran had its port-80 signature never reach the BMC snoop,
   and IPMI request counts vary between boots of a byte-identical image — the
   NVAR record is the only signal that has held up. Read it back with
   tools/probecheck.py after pulling the BIOS mux. */
#include <Library/OobTelemetryLib.h>

/* The host<->BMC inventory blob contract. Mirrored byte-for-byte on the BMC
   side at meta-x570d4i/recipes-asrock/p2a-inventory-monitor/files/. */
#include <OobInventoryBlob.h>

/* NVMe admin opcodes, spelled out rather than pulling IndustryStandard/Nvme.h. */
#define NVME_OPC_GET_LOG_PAGE   0x02u
#define NVME_OPC_IDENTIFY       0x06u

/* Inventory-specific telemetry. Deliberately confined to the high half: the
   shared low-half bits (DATA_FOUND, XFER_OK, ...) belong to the SMBIOS payload,
   and the whole point of one module carrying two payloads is that a decoder can
   still tell which one did what. For the same reason the inventory path never
   calls OobTelemetryDetail — DataLen/LastStatus/LastCc describe the SMBIOS push. */
#define OOB_TLM_INV_PCIE_DONE   0x00010000u
#define OOB_TLM_INV_NVME_DONE   0x00020000u
#define OOB_TLM_INV_VGA_FOUND   0x00040000u
#define OOB_TLM_INV_P2A_OK      0x00080000u
#define OOB_TLM_INV_P2A_FAIL    0x00100000u
#define OOB_TLM_INV_NO_DATA     0x00200000u

/* AST2500 VGA function and its P2A register layout within BAR1. Same interface
   the `culvert` tool drives from Linux. */
#define AST_VGA_VID             0x1A03u
#define AST_VGA_DID             0x2000u
#define AST_P2A_BAR             1
#define AST_P2A_PKR             0xF000u   /* protection key: write 1 unlock, 0 lock */
#define AST_P2A_RBAR            0xF004u   /* remap base, bits [31:16] */
#define AST_P2A_RBAR_MASK       0xFFFF0000u
#define AST_P2A_WINDOW          0x10000u  /* 64 KiB sliding aperture */
#define AST_P2A_WINDOW_LEN      0x10000u

/* Inline the standard PI/UEFI GUID so the binary is independent of the EDK2
   tree used to build (QEMU's MdePkg.dec ships a wrong 0x4940 variant). */
STATIC EFI_GUID mEfiSmbiosProtocolGuid = {
  0x03583FF6, 0xCB36, 0x11D4,
  { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
};

/* Late-DXE protocols we hang retries off.

   Protocol notifies are signalled synchronously on the installing driver's
   thread inside InstallProtocolInterface, so they do not depend on the timer
   tick or on BDS running — which on this board is the difference between a
   retry that happens and one that does not.

   These bracket the window where the payloads become complete:
     SMBIOS protocol            — the SMBIOS data source appearing at all
     PCI enumeration complete   — late DXE; all PciIo handles now exist
     DXE SMM ready to lock      — very late DXE, last useful moment
     NvmExpressPassThru         — a drive just became enumerable (BDS connect);
                                  the ONLY reliable inventory trigger, see the
                                  timing note in the file header. */
STATIC EFI_GUID mPciEnumCompleteGuid = {
  0x30CFE3E7, 0x3DE1, 0x4586,
  { 0xBE, 0x20, 0xDE, 0xAB, 0xA1, 0xB3, 0xB7, 0x93 }
};
STATIC EFI_GUID mDxeSmmReadyToLockGuid = {
  0x60FF8964, 0xE906, 0x41D0,
  { 0xAF, 0xED, 0xF2, 0x41, 0xE9, 0x74, 0xE0, 0x8E }
};


#define BLOB_NETFN 0x2Eu
#define BLOB_CMD   0x80u
#define SUB_OPEN   0x02u
#define SUB_WRITE  0x04u
#define SUB_COMMIT 0x05u
#define SUB_CLOSE  0x06u
#define SUB_STAT   0x08u
#define OPEN_WRITE 0x0002u
#define WRITE_MAX  54u

/* phosphor-ipmi-blobs StateFlags::committed (1<<3) — set in the blob stat
   response when the BMC already has the SMBIOS table persisted on disk. */
#define BLOB_STATE_COMMITTED 0x0008u

STATIC CONST CHAR8 mBlobId[] = "/smbios";
STATIC CONST UINT8 mOen[3] = { 0xCF, 0xC2, 0x00 };

/* One flag per payload. NEVER merge these — see the header. */
STATIC BOOLEAN mSmbiosDone = FALSE;
STATIC BOOLEAN mInvDone    = FALSE;

STATIC UINT8   mLastCC = 0xFF;


STATIC UINT16 GenCrc (CONST UINT8 *data, UINT32 size)
{
  UINT16 crc = 0xFFFF;
  for (UINT32 i = 0; i < size + 2; i++) {
    for (UINT32 j = 0; j < 8; j++) {
      UINT32 xf = (crc & 0x8000) ? 1 : 0;
      crc = (UINT16)(crc << 1);
      if (i < size && (data[i] & (1 << (7 - j)))) crc++;
      if (xf) crc ^= 0x1021;
    }
  }
  return crc;
}

STATIC EFI_STATUS
BlobCmd (UINT8 sub, CONST UINT8 *pay, UINT32 paylen,
         UINT8 *out, UINT32 outmax, UINT32 *outlen, UINT8 *cc)
{
  UINT8 req[2 + 3 + 1 + 2 + 256];
  UINT8 raw[3 + 2 + 256];
  UINT32 rl = sizeof (raw), i = 0;
  EFI_STATUS s;

  req[i++] = (UINT8)(BLOB_NETFN << 2);
  req[i++] = BLOB_CMD;
  req[i++] = mOen[0]; req[i++] = mOen[1]; req[i++] = mOen[2];
  req[i++] = sub;
  if (paylen) {
    UINT16 crc = GenCrc (pay, paylen);
    req[i++] = (UINT8)(crc & 0xFF); req[i++] = (UINT8)(crc >> 8);
    CopyMem (&req[i], pay, paylen); i += paylen;
  }
  s = IpmiKcsRawTransaction (req, i, raw, &rl);
  if (EFI_ERROR (s)) return s;
  if (rl < 3) return EFI_DEVICE_ERROR;
  *cc = raw[2];
  if (out && outmax) {
    UINT32 dlen = (rl > 8) ? (rl - 8) : 0;
    if (dlen > outmax) dlen = outmax;
    CopyMem (out, &raw[8], dlen);
    if (outlen) *outlen = dlen;
  } else if (outlen) {
    *outlen = 0;
  }
  return EFI_SUCCESS;
}

/* Stat the "/smbios" blob (sub 0x08) WITHOUT opening a session.  The patched
   BMC handler reports the table already persisted at /var/lib/smbios/smbios2
   as committed, with a CRC of the stored table payload in the stat metadata
   (computed with this same GenCrc).  We skip the push only when that CRC equals
   the CRC of the table we just built — so any hardware change (DIMM/CPU/FRU)
   alters the table and forces a fresh push.  Upstream (and a fresh BMC with no
   smbios2) reports failure -> push.  Response data (raw[8..]):
   blobState(2) size(4) metaLen(1) metadata(=CRC LE, 2). */
STATIC BOOLEAN
BmcHasSmbios (UINT16 ExpectCrc)
{
  EFI_STATUS s; UINT8 cc; UINT8 pay[16]; UINT32 plen = 0, rlen = 0; UINT8 rsp[16];
  UINT16 state, mdCrc; UINT8 mdLen;

  for (UINT32 i = 0; mBlobId[i]; i++) pay[plen++] = (UINT8)mBlobId[i];
  pay[plen++] = 0;
  s = BlobCmd (SUB_STAT, pay, plen, rsp, sizeof (rsp), &rlen, &cc);
  if (EFI_ERROR (s) || cc != 0 || rlen < 9) return FALSE;
  state = (UINT16)(rsp[0] | ((UINT16)rsp[1] << 8));
  mdLen = rsp[6];                              /* rsp[2..5] = size (unused) */
  if (!(state & BLOB_STATE_COMMITTED) || mdLen < 2) return FALSE;
  mdCrc = (UINT16)(rsp[7] | ((UINT16)rsp[8] << 8));
  return (BOOLEAN)(mdCrc == ExpectCrc);
}

STATIC EFI_STATUS
SendViaBlob (UINT8 *Buf, UINT32 Len)
{
  EFI_STATUS s; UINT8 cc; UINT8 pay[256]; UINT32 plen, rlen; UINT8 rsp[16];
  UINT16 sid; UINT32 off;

  plen = 0;
  pay[plen++] = (UINT8)(OPEN_WRITE & 0xFF);
  pay[plen++] = (UINT8)((OPEN_WRITE >> 8) & 0xFF);
  for (UINT32 i = 0; mBlobId[i]; i++) pay[plen++] = (UINT8)mBlobId[i];
  pay[plen++] = 0;
  rlen = 0;
  s = BlobCmd (SUB_OPEN, pay, plen, rsp, sizeof (rsp), &rlen, &cc);
  mLastCC = cc;
  if (EFI_ERROR (s) || cc != 0 || rlen < 2) return EFI_DEVICE_ERROR;
  sid = (UINT16)(rsp[0] | ((UINT16)rsp[1] << 8));

  off = 0;
  while (off < Len) {
    UINT32 chunk = Len - off; if (chunk > WRITE_MAX) chunk = WRITE_MAX;
    plen = 0;
    pay[plen++] = (UINT8)(sid & 0xFF); pay[plen++] = (UINT8)(sid >> 8);
    pay[plen++] = (UINT8)off; pay[plen++] = (UINT8)(off >> 8);
    pay[plen++] = (UINT8)(off >> 16); pay[plen++] = (UINT8)(off >> 24);
    CopyMem (&pay[plen], Buf + off, chunk); plen += chunk;
    s = BlobCmd (SUB_WRITE, pay, plen, NULL, 0, NULL, &cc);
    if (EFI_ERROR (s) || cc != 0) {
      pay[0] = (UINT8)(sid & 0xFF); pay[1] = (UINT8)(sid >> 8);
      BlobCmd (SUB_CLOSE, pay, 2, NULL, 0, NULL, &cc);
      return EFI_DEVICE_ERROR;
    }
    off += chunk;
  }

  pay[0] = (UINT8)(sid & 0xFF); pay[1] = (UINT8)(sid >> 8); pay[2] = 0;
  s = BlobCmd (SUB_COMMIT, pay, 3, NULL, 0, NULL, &cc);
  if (EFI_ERROR (s) || cc != 0) {
    BlobCmd (SUB_CLOSE, pay, 2, NULL, 0, NULL, &cc);
    return EFI_DEVICE_ERROR;
  }
  BlobCmd (SUB_CLOSE, pay, 2, NULL, 0, NULL, &cc);
  return EFI_SUCCESS;
}

/* Length of one SMBIOS structure: formatted area (hdr[1]) + string-set,
   which ends with a double NUL (00 00). */
STATIC UINT32 SmbiosRecLen (UINT8 *r)
{
  UINT8 *p = r + r[1];
  while (!(p[0] == 0 && p[1] == 0)) p++;
  return (UINT32)((p + 2) - r);
}

/* SMBIOS3 entry point structure layout (SMBIOS spec 3.x):
   offset 0: AnchorString[5] "_SM3_"
   offset 5: Checksum
   offset 6: EntryPointLength (24)
   offset 7: MajorVersion
   offset 8: MinorVersion
   offset 9: DocRev
   offset 10: EntryPointRevision
   offset 11: Reserved
   offset 12: TableMaximumSize (UINT32 LE)
   offset 16: TableAddress (UINT64 LE) */

/* Walk raw SMBIOS structure table to find its exact byte length. */
STATIC UINT32
RawTableLen (UINT8 *Base, UINT32 MaxSz)
{
  UINT8 *p = Base, *fence = Base + MaxSz;
  while (p + 4 < fence) {
    UINT32 l = SmbiosRecLen (p);
    if (p + l > fence) break;
    if (p[0] == 0x7F) { p += l; break; }   /* type 127 = end-of-table */
    p += l;
  }
  return (UINT32)(p - Base);
}

/* gEfiSmbios3TableGuid inline — avoids dependency on SmBios.h GUID header.
   Standard value: {0xF2FD1544, 0x9794, 0x4A2C, {0xBC,0xAA,0x75,0x0C,0xB3,0x49,0x35,0x5D}} */
STATIC EFI_GUID mSmbios3TableGuid = {
  0xF2FD1544, 0x9794, 0x4A2C,
  { 0xBC, 0xAA, 0x75, 0x0C, 0xB3, 0x49, 0x35, 0x5D }
};

/* gEfiSmbiosTableGuid inline (SMBIOS 2.x entry point, installed by many AMI BIOSes):
   {0xEB9D2D31, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}} */
STATIC EFI_GUID mSmbios2TableGuid = {
  0xEB9D2D31, 0x2D88, 0x11D3,
  { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
};

/* Write a 24-byte synthesized SMBIOS3 "_SM3_" entry point at p, describing a
   structure table of TableLen bytes.  smbios-mdr's checkSMBIOSVersion() scans
   the whole stored buffer for an "_SM_"/"_SM3_" anchor and reads only the
   major/minor from it — it does NOT use the table address.  TableAddress is
   therefore set to 0 here. */
STATIC VOID
WriteSm3Ep (UINT8 *p, UINT32 TableLen, UINT8 Maj, UINT8 Min)
{
  p[0]='_'; p[1]='S'; p[2]='M'; p[3]='3'; p[4]='_';
  p[5]=0;             /* checksum (unused by smbios-mdr) */
  p[6]=24;            /* EntryPointLength */
  p[7]=Maj; p[8]=Min;
  p[9]=0;             /* DocRev */
  p[10]=1;            /* EntryPointRevision */
  p[11]=0;            /* Reserved */
  p[12]=(UINT8)TableLen; p[13]=(UINT8)(TableLen>>8);
  p[14]=(UINT8)(TableLen>>16); p[15]=(UINT8)(TableLen>>24);
  p[16]=0; p[17]=0; p[18]=0; p[19]=0; p[20]=0; p[21]=0; p[22]=0; p[23]=0;
}

/* Build [SMBIOS structure table (Type 0 .. Type 127)][synthesized "_SM3_"
   entry point AT THE END].  This exact layout is required by smbios-mdr:
   - its structure walk (getSMBIOSTypePtr) starts at byte 0, so the table MUST
     begin there or Type 0 (BIOS Information / version) is skipped -> version
     reads back null;
   - BUT checkSMBIOSVersion() rejects the whole table ("Unsupported SMBIOS
     table version") unless an "_SM_"/"_SM3_" anchor exists somewhere in the
     buffer.
   Putting the entry point at the END satisfies both (validated live: the BMC
   then reports BIOS version correctly).  A leading entry point breaks Type 0.
   Primary source: EFI_SMBIOS_PROTOCOL (available from early DXE).
   Fallback B: gEfiSmbios3TableGuid (SMBIOS3 entry point in config table).
   Fallback C: gEfiSmbiosTableGuid  (SMBIOS2 entry point — common on AMI).
   Both B and C are populated by AMI at ReadyToBoot. Caller frees *Out. */
STATIC EFI_STATUS
BuildBlob (UINT8 **Out, UINT32 *OutLen)
{
  UINT8    *tbl = NULL;
  UINT32    total = 0;
  UINT8     maj = 3, min = 3;
  CONST UINT32 epLen = 24;
  UINT8    *b;
  UINT32    off;

  /* --- Path A: EFI_SMBIOS_PROTOCOL (fast, early, works at EndOfDxe) --- */
  EFI_SMBIOS_PROTOCOL    *Smbios = NULL;
  EFI_SMBIOS_HANDLE       h;
  EFI_SMBIOS_TABLE_HEADER *rec;
  gBS->LocateProtocol (&mEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (Smbios) {
    UINT32 cnt = 0;
    h = SMBIOS_HANDLE_PI_RESERVED;
    while (Smbios->GetNext (Smbios, &h, NULL, &rec, NULL) == EFI_SUCCESS) {
      total += SmbiosRecLen ((UINT8 *)rec); cnt++;
    }
    if (total && cnt) {
      maj = Smbios->MajorVersion; min = Smbios->MinorVersion;
      if (maj != 3 || min == 1) { maj = 3; min = 3; }
      b = AllocatePool (total + epLen);
      if (!b) return EFI_OUT_OF_RESOURCES;
      off = 0;
      h = SMBIOS_HANDLE_PI_RESERVED;
      while (Smbios->GetNext (Smbios, &h, NULL, &rec, NULL) == EFI_SUCCESS) {
        UINT32 l = SmbiosRecLen ((UINT8 *)rec);
        CopyMem (b + off, rec, l); off += l;
      }
      WriteSm3Ep (b + total, total, maj, min);
      *Out = b; *OutLen = total + epLen;
      return EFI_SUCCESS;
    }
  }

  /* --- Path B: SMBIOS3 configuration table (always valid at ReadyToBoot) --- */
  for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mSmbios3TableGuid)) {
      UINT8 *ep = (UINT8 *)gST->ConfigurationTable[i].VendorTable;
      /* Validate anchor and entry-point length */
      if (ep[0]!='_'||ep[1]!='S'||ep[2]!='M'||ep[3]!='3'||ep[4]!='_'||ep[6]!=24)
        continue;
      UINT32 maxSz; CopyMem (&maxSz, ep + 12, 4);
      UINT64 addr;  CopyMem (&addr,  ep + 16, 8);
      tbl = (UINT8 *)(UINTN)addr;
      if (!tbl || !maxSz) continue;
      total = RawTableLen (tbl, maxSz);
      if (!total) continue;
      maj = ep[7]; min = ep[8];
      if (maj != 3 || min == 1) { maj = 3; min = 3; }
      break;
    }
  }
  /* --- Path C: SMBIOS2 configuration table (gEfiSmbiosTableGuid "_SM_") ---
     SMBIOS2 EP layout:
       offset  0: AnchorString[4] "_SM_"
       offset  5: EntryPointLength (0x1F=31)
       offset  6: MajorVersion
       offset  7: MinorVersion
       offset 24: StructureTableLength (UINT16)
       offset 26: StructureTableAddress (UINT32) */
  if (!total || !tbl) {
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
      if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mSmbios2TableGuid)) {
        UINT8 *ep = (UINT8 *)gST->ConfigurationTable[i].VendorTable;
        if (ep[0]!='_'||ep[1]!='S'||ep[2]!='M'||ep[3]!='_') continue;
        UINT16 maxSz; CopyMem (&maxSz, ep + 22, 2);   /* StructureTableLength */
        UINT32 addr32; CopyMem (&addr32, ep + 24, 4); /* StructureTableAddress */
        if (!addr32 || !maxSz) continue;
        tbl = (UINT8 *)(UINTN)addr32;
        total = RawTableLen (tbl, maxSz);
        if (!total) continue;
        maj = 3; min = 3;          /* synthesize SMBIOS3 EP from 2.x data */
        break;
      }
    }
  }

  if (!total || !tbl) return EFI_NOT_FOUND;

  b = AllocatePool (total + epLen);
  if (!b) return EFI_OUT_OF_RESOURCES;
  CopyMem (b, tbl, total);
  WriteSm3Ep (b + total, total, maj, min);
  *Out = b; *OutLen = total + epLen;
  return EFI_SUCCESS;
}

/* POST-code namespace. The SMBIOS half (0x70-0x7B) predates the boot profile
   and 20 of those values alias AMI's own codes, so it is kept as-is for
   continuity with existing captures. The inventory half uses 0x7C-0x85, which
   the 2026-08-05 two-boot capture measured as genuinely unused by AMI on this
   ROM — so an inventory code on port 80 is unambiguous where a SMBIOS one is not.
     0x70 = DXE entry
     0x74 = timer tick
     0x75 = EndOfDxe fired
     0x76 = ReadyToBoot fired
     0x77 = SMBIOS found (blob built)
     0x78 = no SMBIOS
     0x79 = SMBIOS push OK
     0x7A = SMBIOS push fail
     0x7B = SMBIOS skipped — BMC already holds a byte-identical table
     0x7C = inventory attempt (collection started)
     0x7D = inventory collected (blob built)
     0x7E = inventory pushed OK over P2A
     0x7F = inventory P2A write failed
     0x80 = inventory aborted — AST2500 VGA function not found
     0x81 = inventory found nothing to report (no PCIe, no drives) */

STATIC VOID DoPush (VOID)
{
  EFI_STATUS s;

  if (mSmbiosDone) return;
  UINT8 *Buf = NULL; UINT32 Len = 0;
  if (EFI_ERROR (BuildBlob (&Buf, &Len))) {
    OobPostCode (0x78);
    /* Not flagged as a hard failure: on this board the timer starts ticking
       long before AMI has populated the SMBIOS table, so "missing" is the
       normal state for the first several ticks. The flag records that we got
       far enough to look. */
    OobTelemetryFlag (OOB_TLM_DATA_MISSING);
    return;
  }
  OobPostCode (0x77);
  OobTelemetryDetail (Len, EFI_SUCCESS, mLastCC);
  OobTelemetryFlag (OOB_TLM_DATA_FOUND);
  /* Build first, then skip the KCS transfer only if the BMC already holds a
     byte-identical table (matching CRC).  A hardware change alters the table,
     so the CRC differs and we re-push. */
  if (BmcHasSmbios (GenCrc (Buf, Len))) {
    mSmbiosDone = TRUE; FreePool (Buf); OobPostCode (0x7B);
    OobTelemetryFlag (OOB_TLM_XFER_SKIP);
    return;
  }
  s = SendViaBlob (Buf, Len);
  if (!EFI_ERROR (s)) {
    mSmbiosDone = TRUE; OobPostCode (0x79);
    OobTelemetryDetail (Len, s, mLastCC);
    OobTelemetryFlag (OOB_TLM_XFER_OK);
  } else {
    OobPostCode (0x7A);
    OobTelemetryDetail (Len, s, mLastCC);
    OobTelemetryFlag (OOB_TLM_XFER_FAIL);
  }
  FreePool (Buf);
}

/* ---------------------------------------------------------------------------
   Inventory payload: PCIe enumeration
   --------------------------------------------------------------------------- */

/* Walk the PCI capability list for the PCI Express capability (ID 0x10). Returns
   its config-space offset, or 0 if absent (non-PCIe function). */
STATIC UINT8
FindPcieCap (EFI_PCI_IO_PROTOCOL *PciIo)
{
  UINT16 Status;
  UINT8  Ptr, Id;
  UINTN  Guard;

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_PRIMARY_STATUS_OFFSET, 1, &Status);
  if ((Status & EFI_PCI_STATUS_CAPABILITY) == 0) {
    return 0;
  }
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, PCI_CAPBILITY_POINTER_OFFSET, 1, &Ptr);
  Guard = 0;
  while (Ptr >= 0x40 && (Ptr & 0x3) == 0 && Guard++ < 48) {
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, Ptr, 1, &Id);
    if (Id == EFI_PCI_CAPABILITY_ID_PCIEXP) {
      return Ptr;
    }
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, (UINT32)Ptr + 1, 1, &Ptr);
  }
  return 0;
}

/* Map a PCIe link-speed encoding (1..5) straight to a generation number. */
STATIC UINT8
LinkSpeedToGen (UINT16 Enc)
{
  return (Enc >= 1 && Enc <= 5) ? (UINT8)Enc : 0;
}

STATIC VOID
FillPcieRecord (EFI_PCI_IO_PROTOCOL *PciIo, OOB_INV_PCIE_RECORD *R)
{
  UINTN  Seg, Bus, Dev, Fun;
  UINT32 Id, Sub, ClassRev;
  UINT8  HdrType, Cap;

  ZeroMem (R, sizeof (*R));

  PciIo->GetLocation (PciIo, &Seg, &Bus, &Dev, &Fun);
  R->Segment  = (UINT16)Seg;
  R->Bus      = (UINT8)Bus;
  R->Device   = (UINT8)Dev;
  R->Function = (UINT8)Fun;

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_VENDOR_ID_OFFSET, 1, &Id);
  R->VendorId = (UINT16)(Id & 0xFFFF);
  R->DeviceId = (UINT16)(Id >> 16);

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_REVISION_ID_OFFSET, 1, &ClassRev);
  R->RevisionId  = (UINT8)(ClassRev & 0xFF);
  R->ClassProgIf = (UINT8)((ClassRev >> 8) & 0xFF);
  R->ClassSub    = (UINT8)((ClassRev >> 16) & 0xFF);
  R->ClassBase   = (UINT8)((ClassRev >> 24) & 0xFF);

  /* Subsystem IDs live at 0x2C only for a type-0 (endpoint) header. */
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, PCI_HEADER_TYPE_OFFSET, 1, &HdrType);
  R->DeviceType = (HdrType & HEADER_TYPE_MULTI_FUNCTION)
                    ? OOB_INV_PCIE_MULTI_FN : OOB_INV_PCIE_SINGLE_FN;
  if ((HdrType & HEADER_LAYOUT_CODE) == HEADER_TYPE_DEVICE) {
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_SUBSYSTEM_VENDOR_ID_OFFSET, 1, &Sub);
    R->SubsysVendorId = (UINT16)(Sub & 0xFFFF);
    R->SubsysId       = (UINT16)(Sub >> 16);
  }

  Cap = FindPcieCap (PciIo);
  if (Cap != 0) {
    UINT32 LinkCap;
    UINT16 LinkSta;
    /* PCI Express Capability: LinkCapabilities @cap+0x0C, LinkStatus @cap+0x12. */
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, (UINT32)Cap + 0x0C, 1, &LinkCap);
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, (UINT32)Cap + 0x12, 1, &LinkSta);
    R->GenerationMax   = LinkSpeedToGen ((UINT16)(LinkCap & 0xF));
    R->LanesMax        = (UINT8)((LinkCap >> 4) & 0x3F);
    R->GenerationInUse = LinkSpeedToGen ((UINT16)(LinkSta & 0xF));
    R->LanesInUse      = (UINT8)((LinkSta >> 4) & 0x3F);
  }
}

/* ---------------------------------------------------------------------------
   Inventory payload: NVMe enumeration
   --------------------------------------------------------------------------- */

/* Allocate a page-aligned admin buffer (covers any NVMe IoAlign requirement). */
STATIC VOID *
AllocAligned (UINTN Bytes)
{
  EFI_PHYSICAL_ADDRESS Addr = 0;
  UINTN Pages = EFI_SIZE_TO_PAGES (Bytes);
  if (EFI_ERROR (gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &Addr))) {
    return NULL;
  }
  ZeroMem ((VOID *)(UINTN)Addr, EFI_PAGES_TO_SIZE (Pages));
  return (VOID *)(UINTN)Addr;
}

STATIC VOID
FreeAligned (VOID *Buf, UINTN Bytes)
{
  if (Buf != NULL) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Buf, EFI_SIZE_TO_PAGES (Bytes));
  }
}

/* Issue one NVMe admin command through the pass-thru protocol. */
STATIC EFI_STATUS
NvmeAdmin (
  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *Pt,
  UINT32 Nsid, UINT8 Opcode, UINT32 Cdw10,
  VOID *Buf, UINT32 Len)
{
  EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET Pkt;
  EFI_NVM_EXPRESS_COMMAND     Cmd;
  EFI_NVM_EXPRESS_COMPLETION  Cpl;

  ZeroMem (&Pkt, sizeof (Pkt));
  ZeroMem (&Cmd, sizeof (Cmd));
  ZeroMem (&Cpl, sizeof (Cpl));

  Cmd.Cdw0.Opcode         = Opcode;
  Cmd.Cdw0.FusedOperation = NORMAL_CMD;
  Cmd.Nsid                = Nsid;
  Cmd.Cdw10               = Cdw10;
  Cmd.Flags               = CDW10_VALID;

  Pkt.NvmeCmd        = &Cmd;
  Pkt.NvmeCompletion = &Cpl;
  Pkt.TransferBuffer = Buf;
  Pkt.TransferLength = Len;
  Pkt.CommandTimeout = 5000000; /* 500 ms in 100 ns units */
  Pkt.QueueType      = NVME_ADMIN_QUEUE;

  return Pt->PassThru (Pt, Nsid, &Pkt, NULL);
}

/* Trim trailing spaces/NULs an NVMe identify string carries, in place. */
STATIC VOID
TrimField (char *Dst, CONST UINT8 *Src, UINTN Len)
{
  UINTN i;
  CopyMem (Dst, Src, Len);
  for (i = Len; i > 0; i--) {
    if (Dst[i - 1] == ' ' || Dst[i - 1] == 0) {
      Dst[i - 1] = 0;
    } else {
      break;
    }
  }
}

/* Enumerate namespaces on one NVMe controller and append a drive record each. */
STATIC VOID
CollectNvme (
  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *Pt,
  OOB_INV_DRIVE_RECORD *Recs, UINT16 *Count, UINT16 Max)
{
  UINT8  *Ident;  /* Identify Controller (4 KiB) */
  UINT8  *NsBuf;  /* Identify Namespace (4 KiB)  */
  UINT8  *Smart;  /* SMART / Health log (512 B)  */
  UINT32  Nsid;
  char    Model[40], Serial[20], Firmware[8];
  BOOLEAN HaveCtrl = FALSE;

  Ident = AllocAligned (4096);
  NsBuf = AllocAligned (4096);
  Smart = AllocAligned (512);
  if (Ident == NULL || NsBuf == NULL || Smart == NULL) {
    goto Done;
  }

  /* Identify Controller: CNS=1, NSID=0. */
  if (!EFI_ERROR (NvmeAdmin (Pt, 0, NVME_OPC_IDENTIFY, 1, Ident, 4096))) {
    TrimField (Serial,   Ident + 4,  20);
    TrimField (Model,    Ident + 24, 40);
    TrimField (Firmware, Ident + 64, 8);
    HaveCtrl = TRUE;
  }

  Nsid = 0xFFFFFFFF;
  while (*Count < Max && Pt->GetNextNamespace (Pt, &Nsid) == EFI_SUCCESS) {
    OOB_INV_DRIVE_RECORD *R = &Recs[*Count];
    UINT64 Nsze;
    UINT8  Flbas, Lbads = 9; /* default 512 B if we cannot read the LBA format */

    ZeroMem (R, sizeof (*R));
    R->Segment  = OOB_INV_LOC_UNKNOWN;
    R->Bus      = OOB_INV_LOC_UNKNOWN;
    R->Device   = OOB_INV_LOC_UNKNOWN;
    R->Function = OOB_INV_LOC_UNKNOWN;
    R->Protocol = OOB_INV_PROTO_NVME;
    R->MediaType = OOB_INV_MEDIA_SSD;
    R->NamespaceId = Nsid;
    if (HaveCtrl) {
      CopyMem (R->Model, Model, sizeof (R->Model));
      CopyMem (R->Serial, Serial, sizeof (R->Serial));
      CopyMem (R->Firmware, Firmware, sizeof (R->Firmware));
    }

    /* Identify Namespace: CNS=0, NSID=Nsid. */
    if (!EFI_ERROR (NvmeAdmin (Pt, Nsid, NVME_OPC_IDENTIFY, 0, NsBuf, 4096))) {
      Nsze  = *(UINT64 *)(NsBuf + 0);
      Flbas = NsBuf[26] & 0x0F;             /* current LBA format index */
      Lbads = (NsBuf[128 + Flbas * 4 + 2]) & 0x3F; /* LBAF[.].LBADS = bits 23:16 */
      if (Lbads == 0 || Lbads > 31) {
        Lbads = 9;
      }
      R->BlockSizeBytes = (UINT32)1u << Lbads;
      R->CapacityBytes  = Nsze << Lbads;
    }

    /* SMART / Health Information log: LID=0x02, whole 512 B, global NSID. */
    if (!EFI_ERROR (NvmeAdmin (Pt, 0xFFFFFFFF, NVME_OPC_GET_LOG_PAGE,
                               ((512u / 4u - 1u) << 16) | 0x02u, Smart, 512))) {
      R->SmartCritWarning = Smart[0];
      R->CompositeTempK   = (UINT16)(Smart[1] | ((UINT16)Smart[2] << 8));
      R->AvailableSpare   = Smart[3];
      R->PercentageUsed   = Smart[5];
    }

    (*Count)++;
  }

Done:
  FreeAligned (Ident, 4096);
  FreeAligned (NsBuf, 4096);
  FreeAligned (Smart, 512);
}

/* ---------------------------------------------------------------------------
   Inventory payload: P2A write
   --------------------------------------------------------------------------- */

STATIC EFI_STATUS
FindAstVga (EFI_PCI_IO_PROTOCOL **Out)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, i;
  EFI_STATUS s;

  s = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (s)) {
    return s;
  }
  for (i = 0; i < Count; i++) {
    EFI_PCI_IO_PROTOCOL *PciIo;
    UINT32 Id;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
      continue;
    }
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_VENDOR_ID_OFFSET, 1, &Id);
    if ((UINT16)(Id & 0xFFFF) == AST_VGA_VID && (UINT16)(Id >> 16) == AST_VGA_DID) {
      *Out = PciIo;
      FreePool (Handles);
      return EFI_SUCCESS;
    }
  }
  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC VOID
P2aWrite32 (EFI_PCI_IO_PROTOCOL *V, UINT64 Off, UINT32 Val)
{
  V->Mem.Write (V, EfiPciIoWidthUint32, AST_P2A_BAR, Off, 1, &Val);
}

/* Push Len bytes (4-byte padded) into BMC physical address Phys via the P2A
   sliding window. Phys must be within the DT-reserved region; Len <= 64 KiB. */
STATIC EFI_STATUS
P2aPush (EFI_PCI_IO_PROTOCOL *V, UINT32 Phys, VOID *Buf, UINT32 Len)
{
  UINT32 Words = (Len + 3) / 4;
  UINT32 Rbar  = Phys & AST_P2A_RBAR_MASK;
  UINT32 WinOff = Phys & ~AST_P2A_RBAR_MASK;

  /* One region, one page: the blob is < 64 KiB and 64 KiB-aligned by contract,
     so a single RBAR programming and a single incrementing burst suffice. */
  if (WinOff + Len > AST_P2A_WINDOW_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  /* Enable memory decode on the VGA BAR, then unlock and aim the window. */
  V->Attributes (V, EfiPciIoAttributeOperationEnable,
                 EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
  P2aWrite32 (V, AST_P2A_PKR, 1);
  P2aWrite32 (V, AST_P2A_RBAR, Rbar);

  /* EfiPciIoWidthUint32 increments the MMIO offset per element, so this walks
     the aperture and lands the whole blob in DRAM in one call. */
  V->Mem.Write (V, EfiPciIoWidthUint32, AST_P2A_BAR,
                AST_P2A_WINDOW + WinOff, Words, Buf);

  P2aWrite32 (V, AST_P2A_PKR, 0);
  return EFI_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Inventory payload: assembly + push
   --------------------------------------------------------------------------- */

/* Every failure path here is silent and non-fatal by design: this module's
   primary job is the SMBIOS push, and an inventory problem must never cost the
   BMC its SMBIOS data. Outcomes are recorded in the high-half telemetry bits. */
STATIC VOID
DoInventory (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, i;
  EFI_PCI_IO_PROTOCOL *Vga = NULL;
  UINT8 *Blob;
  OOB_INV_HEADER *H;
  OOB_INV_PCIE_RECORD *PcieRecs;
  OOB_INV_DRIVE_RECORD *DriveRecs;
  UINT16 PcieN = 0, DriveN = 0;
  UINT32 BlobMax, Total;
  EFI_STATUS s;

  if (mInvDone) {
    return;
  }
  OobPostCode (0x7C);

  BlobMax = sizeof (OOB_INV_HEADER)
            + OOB_INV_MAX_PCIE * sizeof (OOB_INV_PCIE_RECORD)
            + OOB_INV_MAX_DRIVES * sizeof (OOB_INV_DRIVE_RECORD);
  if (BlobMax > OOB_INV_REGION_LEN) {
    return; /* format ceilings must fit the DT region; compile-time truth */
  }
  Blob = AllocateZeroPool (BlobMax);
  if (Blob == NULL) {
    return;
  }
  H         = (OOB_INV_HEADER *)Blob;
  PcieRecs  = (OOB_INV_PCIE_RECORD *)(Blob + sizeof (OOB_INV_HEADER));
  DriveRecs = (OOB_INV_DRIVE_RECORD *)(PcieRecs + OOB_INV_MAX_PCIE);

  /* --- PCIe functions --- */
  s = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (s)) {
    for (i = 0; i < Count && PcieN < OOB_INV_MAX_PCIE; i++) {
      EFI_PCI_IO_PROTOCOL *PciIo;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
        continue;
      }
      FillPcieRecord (PciIo, &PcieRecs[PcieN]);
      PcieN++;
    }
    FreePool (Handles);
  }
  if (PcieN) {
    OobTelemetryFlag (OOB_TLM_INV_PCIE_DONE);
  }

  /* --- NVMe drives --- */
  Handles = NULL; Count = 0;
  s = gBS->LocateHandleBuffer (ByProtocol, &gEfiNvmExpressPassThruProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (s)) {
    for (i = 0; i < Count && DriveN < OOB_INV_MAX_DRIVES; i++) {
      EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *Pt;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiNvmExpressPassThruProtocolGuid, (VOID **)&Pt))) {
        continue;
      }
      CollectNvme (Pt, DriveRecs, &DriveN, OOB_INV_MAX_DRIVES);
    }
    FreePool (Handles);
  }
  if (DriveN) {
    OobTelemetryFlag (OOB_TLM_INV_NVME_DONE);
  }

  /* Nothing worth pushing — leave the region untouched so the BMC keeps
     whatever a prior boot wrote, and retry on the next notify. Expected at the
     entry-point attempt, where NVMe cannot exist yet. */
  if (PcieN == 0 && DriveN == 0) {
    OobPostCode (0x81);
    OobTelemetryFlag (OOB_TLM_INV_NO_DATA);
    FreePool (Blob);
    return;
  }

  /* --- compact the two arrays so drive records follow PCIe records with no gap,
         then finalize the header --- */
  if (DriveN > 0) {
    CopyMem (PcieRecs + PcieN, DriveRecs, (UINTN)DriveN * sizeof (OOB_INV_DRIVE_RECORD));
  }
  Total = sizeof (OOB_INV_HEADER)
          + (UINT32)PcieN * sizeof (OOB_INV_PCIE_RECORD)
          + (UINT32)DriveN * sizeof (OOB_INV_DRIVE_RECORD);

  ZeroMem (H, sizeof (*H));
  H->Magic         = OOB_INV_MAGIC;
  H->FormatVersion = OOB_INV_FORMAT_VERSION;
  H->HeaderSize    = sizeof (OOB_INV_HEADER);
  H->TotalSize     = Total;
  H->BootSerial    = (UINT32)AsmReadTsc ();
  H->PcieRecSize   = sizeof (OOB_INV_PCIE_RECORD);
  H->DriveRecSize  = sizeof (OOB_INV_DRIVE_RECORD);
  H->PcieCount     = PcieN;
  H->DriveCount    = DriveN;
  H->Crc32         = 0;
  H->Crc32         = OobInvCrc32 (Blob, Total);
  OobPostCode (0x7D);

  /* --- ship it over P2A --- */
  if (EFI_ERROR (FindAstVga (&Vga))) {
    OobTelemetryFlag (OOB_TLM_INV_P2A_FAIL);
    OobPostCode (0x80);
    FreePool (Blob);
    return;
  }
  OobTelemetryFlag (OOB_TLM_INV_VGA_FOUND);

  if (EFI_ERROR (P2aPush (Vga, OOB_INV_BMC_PHYS_ADDR, Blob, Total))) {
    OobTelemetryFlag (OOB_TLM_INV_P2A_FAIL);
    OobPostCode (0x7F);
    FreePool (Blob);
    return;
  }

  /* Only latch done once a drive has actually been reported. A PCIe-only push
     is worth sending (the BMC gets its PCIeDevice list early) but must not end
     the retries, or a blob captured before BDS connected the NVMe controller
     would be the final word for the whole boot. */
  if (DriveN > 0) {
    mInvDone = TRUE;
  }
  OobTelemetryFlag (OOB_TLM_INV_P2A_OK);
  OobPostCode (0x7E);
  FreePool (Blob);
}

/* Both payloads, SMBIOS first: it is the proven one, and it must not be
   starved by an inventory pass that faults or blocks. */
STATIC VOID DoWork (VOID)
{
  DoPush ();
  DoInventory ();
}

/* Protocol-notify retry. Unlike the timer, this actually fires on this board. */
STATIC VOID EFIAPI
OnLateProtocol (IN EFI_EVENT Event, IN VOID *Context)
{
  OobTelemetryFlag (OOB_TLM_PROTO_READY);
  DoWork ();
}

STATIC VOID
ArmProtocolNotify (IN EFI_GUID *Protocol)
{
  EFI_EVENT  Event;
  VOID       *Registration;

  Event = EfiCreateProtocolNotifyEvent (Protocol, TPL_CALLBACK,
                                        OnLateProtocol, NULL, &Registration);
  (VOID)Event;
}

STATIC VOID EFIAPI OnReadyToBoot (IN EFI_EVENT E, IN VOID *C)
{ OobPostCode (0x76); OobTelemetryFlag (OOB_TLM_READYTOBOOT); DoWork (); }

STATIC VOID EFIAPI OnEndOfDxe (IN EFI_EVENT E, IN VOID *C)
{ OobPostCode (0x75); OobTelemetryFlag (OOB_TLM_ENDOFDXE); DoWork (); }

STATIC UINTN     mTicks = 0;
STATIC EFI_EVENT mTimer = NULL;

STATIC VOID EFIAPI
OnTimer (IN EFI_EVENT Event, IN VOID *Context)
{
  OobPostCode (0x74);
  OobTelemetryTick ();
  OobTelemetryFlag (OOB_TLM_TIMER);
  DoWork ();
  mTicks++;
  if ((mSmbiosDone && mInvDone) || mTicks > 600) {
    gBS->SetTimer (Event, TimerCancel, 0);
    gBS->CloseEvent (Event);
    mTimer = NULL;
  }
}

EFI_STATUS EFIAPI
SmbiosBmcPushEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT Event;

  /* Telemetry FIRST — before any I/O, any library call, anything that could
     fault or block. On the 2026-07-30 run this module left no trace whatsoever
     while the drivers on either side of it in the same volume both recorded
     their entry, so the first job of this entry point is to prove it was
     reached at all. Everything else is now downstream of that evidence. */
  OobTelemetryInit (L"OobSmbios");
  OobTelemetryFlag (OOB_TLM_ENTRY);

  OobPostCode (0x70);

  /* The synchronous KCS ping that used to live here is gone. It was the only
     thing this module did at entry that the two working drivers did not, and a
     KCS transaction against a BMC that is still booting can block for a long
     time. Reachability is now established by the real transfer instead, whose
     outcome the telemetry records anyway. */

  /* Try immediately. SMBIOS may already be there; the inventory pass will
     almost certainly report nothing this early (NVMe is bound at BDS connect,
     long after DXE dispatch) and that is expected, not a failure. */
  DoWork ();

  /* Armed UNCONDITIONALLY — never behind a completion flag. The NvmExpress-
     PassThru notify is the one trigger that fires at the moment a drive becomes
     enumerable, and gating it on the SMBIOS result (as a single shared "done"
     flag once did) would silently reduce the inventory payload to its
     entry-point attempt, i.e. PCIe data and zero drives on every boot. */
  ArmProtocolNotify (&gEfiNvmExpressPassThruProtocolGuid);
  ArmProtocolNotify (&mEfiSmbiosProtocolGuid);
  ArmProtocolNotify (&mPciEnumCompleteGuid);
  ArmProtocolNotify (&mDxeSmmReadyToLockGuid);

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnEndOfDxe, NULL,
                      &gEfiEndOfDxeEventGroupGuid, &Event);
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                    OnTimer, NULL, &mTimer))) {
    if (!EFI_ERROR (gBS->SetTimer (mTimer, TimerPeriodic, 20000000ULL))) {
      OobTelemetryFlag (OOB_TLM_TIMER_ARMED);
    }
  }
  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
                      &gEfiEventReadyToBootGuid, &Event);
  return EFI_SUCCESS;
}
