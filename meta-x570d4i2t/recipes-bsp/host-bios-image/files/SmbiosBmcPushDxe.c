/** @file
  SmbiosBmcPushDxe — at ReadyToBoot, push the host SMBIOS structure table to the
  BMC via the standard OpenBMC phosphor-ipmi-blobs protocol (blob ID "/smbios"),
  over the host KCS interface (LPC I/O 0xCA2/0xCA3).

  Built as a normal edk2 DXE_DRIVER (UefiDriverEntryPoint + MdePkg libs) so the
  AMI Aptio DXE dispatcher loads/runs it like any other driver.

  Wire protocol (validated end-to-end by SmbiosBmcPushApp):
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

  POST-code markers on port 0x80 (BMC-snooped, BIOS-unused values) trace
  progress: 0x58 entry, 0x5B smbios-found, 0x5C no-smbios, 0xB8 push-ok,
  0xB9 push-fail, 0xBA ReadyToBoot.

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

/* Inline the standard PI/UEFI GUID so the binary is independent of the EDK2
   tree used to build (QEMU's MdePkg.dec ships a wrong 0x4940 variant). */
STATIC EFI_GUID mEfiSmbiosProtocolGuid = {
  0x03583FF6, 0xCB36, 0x11D4,
  { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
};

#define KCS_DATA 0x0CA2u
#define KCS_CMD  0x0CA3u
#define KCS_OBF  0x01u
#define KCS_IBF  0x02u
#define KCS_STATE(s) ((s) >> 6)
#define KCS_IDLE 0u
#define KCS_READ 1u
#define KCS_WRITE 2u
#define KCS_WRITE_START 0x61u
#define KCS_WRITE_END   0x62u
#define KCS_READ_BYTE   0x68u
#define KCS_SPIN 1000000u  /* ~500ms bound per spin; covers slow BMC response */

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
STATIC BOOLEAN mDone = FALSE;
STATIC UINT8   mLastCC = 0xFF;

STATIC VOID Post (UINT8 v) { IoWrite8 (0x80, v); }

/* ── KCS ─────────────────────────────────────────────────────────────────── */
STATIC INTN KcsWaitIbf (VOID) { for (UINT32 i=0;i<KCS_SPIN;i++) if (!(IoRead8(KCS_CMD)&KCS_IBF)) return 0; return -1; }
STATIC INTN KcsWaitObf (VOID) { for (UINT32 i=0;i<KCS_SPIN;i++) if (IoRead8(KCS_CMD)&KCS_OBF) return 0; return -1; }
STATIC VOID KcsClrObf (VOID) { if (IoRead8(KCS_CMD)&KCS_OBF) IoRead8(KCS_DATA); }

STATIC EFI_STATUS
KcsTxn (CONST UINT8 *req, UINT32 rlen, UINT8 *rsp, UINT32 *rsplen)
{
  UINT32 i, n, k;
  /* don't collide with the BIOS's own IPMI: wait until KCS is IDLE + input empty */
  for (k = 0; k < KCS_SPIN; k++) { UINT8 st = IoRead8 (KCS_CMD); if (!(st & KCS_IBF) && KCS_STATE(st) == KCS_IDLE) break; }
  if (k >= KCS_SPIN) return EFI_NOT_READY;
  if (KcsWaitIbf()) return EFI_TIMEOUT;
  KcsClrObf();
  IoWrite8 (KCS_CMD, KCS_WRITE_START);
  if (KcsWaitIbf()) return EFI_TIMEOUT;
  if (KCS_STATE(IoRead8(KCS_CMD)) != KCS_WRITE) return EFI_DEVICE_ERROR;
  KcsClrObf();
  for (i = 0; i + 1 < rlen; i++) {
    IoWrite8 (KCS_DATA, req[i]);
    if (KcsWaitIbf()) return EFI_TIMEOUT;
    if (KCS_STATE(IoRead8(KCS_CMD)) != KCS_WRITE) return EFI_DEVICE_ERROR;
    KcsClrObf();
  }
  IoWrite8 (KCS_CMD, KCS_WRITE_END);
  if (KcsWaitIbf()) return EFI_TIMEOUT;
  if (KCS_STATE(IoRead8(KCS_CMD)) != KCS_WRITE) return EFI_DEVICE_ERROR;
  KcsClrObf();
  IoWrite8 (KCS_DATA, req[rlen - 1]);
  n = 0;
  for (;;) {
    UINT8 st;
    UINT32 w;
    /* After writing a byte IBF is set; wait for BMC to read it (IBF clears). */
    if (KcsWaitIbf()) return EFI_TIMEOUT;
    /* BMC is now processing; state may still be WRITE while it computes the
       response.  Spin until state transitions to READ or IDLE. */
    for (w = 0; w < KCS_SPIN; w++) {
      st = IoRead8 (KCS_CMD);
      if (KCS_STATE(st) == KCS_READ || KCS_STATE(st) == KCS_IDLE) break;
    }
    if (w >= KCS_SPIN) return EFI_TIMEOUT;
    st = IoRead8 (KCS_CMD);
    if (KCS_STATE(st) == KCS_READ) {
      if (KcsWaitObf()) return EFI_TIMEOUT;
      { UINT8 d = IoRead8 (KCS_DATA); if (n < *rsplen) rsp[n++] = d; }
      IoWrite8 (KCS_DATA, KCS_READ_BYTE);
    } else {   /* IDLE: transfer complete */
      if (KcsWaitObf()) return EFI_TIMEOUT;
      IoRead8 (KCS_DATA);
      break;
    }
  }
  *rsplen = n;
  return EFI_SUCCESS;
}

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
  s = KcsTxn (req, i, raw, &rl);
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

/* POST-code namespace:
   0x70 = DXE entry         (AMI does not use 0x6x/0x7x on this platform)
   0x71 = KCS ping attempt  (immediate, synchronous in entry — proves dispatch)
   0x72 = KCS ping OK
   0x73 = KCS ping FAIL
   0x74 = timer tick        (replaces 0xBB which AMI also uses)
   0x75 = EndOfDxe fired
   0x76 = ReadyToBoot fired
   0x77 = SMBIOS found (blob built)
   0x78 = no SMBIOS
   0x79 = push OK
   0x7A = push fail
   0x7B = skipped — BMC already holds a byte-identical table (CRC match) */

STATIC VOID DoPush (VOID)
{
  if (mDone) return;
  UINT8 *Buf = NULL; UINT32 Len = 0;
  if (EFI_ERROR (BuildBlob (&Buf, &Len))) { Post (0x78); return; }
  Post (0x77);
  /* Build first, then skip the KCS transfer only if the BMC already holds a
     byte-identical table (matching CRC).  A hardware change alters the table,
     so the CRC differs and we re-push. */
  if (BmcHasSmbios (GenCrc (Buf, Len))) { mDone = TRUE; FreePool (Buf); Post (0x7B); return; }
  if (!EFI_ERROR (SendViaBlob (Buf, Len))) { mDone = TRUE; Post (0x79); }
  else Post (0x7A);
  FreePool (Buf);
}

STATIC VOID EFIAPI OnReadyToBoot (IN EFI_EVENT E, IN VOID *C) { Post (0x76); DoPush (); }
STATIC VOID EFIAPI OnEndOfDxe   (IN EFI_EVENT E, IN VOID *C) { Post (0x75); DoPush (); }

STATIC UINTN     mTicks = 0;
STATIC EFI_EVENT mTimer = NULL;

STATIC VOID EFIAPI
OnTimer (IN EFI_EVENT Event, IN VOID *Context)
{
  Post (0x74);
  DoPush ();
  mTicks++;
  if (mDone || mTicks > 600) {
    gBS->SetTimer (Event, TimerCancel, 0);
    gBS->CloseEvent (Event);
    mTimer = NULL;
  }
}

EFI_STATUS EFIAPI
SmbiosBmcPushEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT Event;
  Post (0x70);    /* unique entry marker — 0x6x/0x7x not used by AMI on this board */

  /* Immediate synchronous KCS ping at entry: proves the DXE was dispatched and
     KCS is reachable without relying on any timer or event.  Appears in the BMC
     ipmid journal as netfn=0x06 cmd=0x01 ch=3.  Remove once confirmed. */
  Post (0x71);
  {
    UINT8 req[2] = { (UINT8)(0x06u << 2), 0x01u };
    UINT8 rsp[20]; UINT32 rl = sizeof (rsp);
    if (EFI_ERROR (KcsTxn (req, sizeof (req), rsp, &rl))) { Post (0x73); }
    else { Post (0x72); }
  }

  if (!mDone) {
    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnEndOfDxe, NULL,
                        &gEfiEndOfDxeEventGroupGuid, &Event);
    if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                      OnTimer, NULL, &mTimer))) {
      gBS->SetTimer (mTimer, TimerPeriodic, 20000000ULL);
    }
    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
                        &gEfiEventReadyToBootGuid, &Event);
  }
  return EFI_SUCCESS;
}
