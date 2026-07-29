/** @file
  BiosCfgOobDxe — host side of the OpenBMC "BIOS OOB config" IPMI command set
  (NetFn 0x30, cmds 0xD3-0xD8), the mirror image of
  openbmc/intel-ipmi-oem src/biosconfigcommands.cpp.

  ---------------------------------------------------------------------------
  Why this module exists
  ---------------------------------------------------------------------------
  The stock ASRock X570D4I-2T BIOS (2.59C and 2.59F alike) contains NO BIOS
  config OOB producer at all: its SendInfoBmcIpmiDxe (FILE_GUID
  9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24) is a 3 KB module that never issues
  cmd 0xD3/0xD5, and the strings a BIOS-config XML generator would need
  ("biosknobs", "setupType", "checkbox", ...) appear nowhere in the 32 MiB
  image, compressed or otherwise.

  What has been driving the BMC's /redfish/v1/Systems/system/Bios is an
  EARLIER build of our own injected driver, which carried a 13719-byte
  LZMA-compressed XML BAKED IN AS A CONSTANT and pushed it verbatim on every
  POST. That explains every oddity the BMC side had to work around:

    * every knob's CurrentVal equals its own default (the blob is a static
      *defaults template*, not a reading of anything);
    * the payload is byte-identical on every boot, so its CRC32 never moves;
    * CSM009 and SLOTOPROM022 both claim to describe Setup byte 0x01BA yet
      report different "current" values, which no live read could produce.

  This module replaces that. The schema half is unchanged in kind (the knob
  registry genuinely is static firmware metadata), but the VALUES are now read
  out of the real UEFI variables with gRT->GetVariable at EndOfDxe, and staged
  Redfish changes are pulled back and written with gRT->SetVariable. The BMC
  no longer has to steer the SPI mux and parse the AMI NVAR store while the
  host is powered off just to learn what the BIOS is actually set to.

  ---------------------------------------------------------------------------
  Protocol (host side of intel-ipmi-oem, reversed)
  ---------------------------------------------------------------------------
    0xD3 SetBIOSCapabilities   [capability][0][0][0]
    0xD4 GetBIOSCapabilities
    0xD5 SetPayload            host -> BMC, chunked + CRC32
           state 0 start       [0][type][ver(2) size(4) crc(4) flag(1)] -> resId
           state 1 inProgress  [1][type][resId(4) off(4) len(4) crc(4)][data]
           state 2 end         [2][type][resId(4)]
           state 3 abort       [3][type]
    0xD6 GetPayload            BMC -> host
           param 0 info        [0][type] -> ver(2) type(1) size(4) crc(4)
                                            flag(1) status(1) timestamp(4)
           param 1 data        [1][type][off(4) len(4)]
                                        -> type(1) readCount(4) crc(4) data
           param 2 status      [2][type] -> type(1) status(1)

  Payload types (PayloadType in biosconfigcommands.hpp; Intel uses 0/1 for the
  XML and 5 for an opaque blob, so 2 and 3 are free):

    1  schema XML, LZMA-compressed        host -> BMC, sent only when the BMC
                                          does not already hold this exact CRC
    2  live varstore snapshot  "ASVS"     host -> BMC, every boot (~2 KB)
    3  pending settings        "ASPS"     BMC  -> host, applied via SetVariable

  Because the host now asks the BMC what it already has (0xD6 info) before
  sending, the steady-state cost of a boot is two short GetPayload commands
  instead of a 287-chunk KCS transfer. This is the gate the BMC could never
  enforce on its own: the BIOS ignores completion codes, so a BMC-side refusal
  could not stop the transfer, only the parse.

  POST codes on port 0x80 (0x6x is unused by AMI on this board; SmbiosBmcPush
  owns 0x7x):
    0x60 entry            0x61 snapshot built     0x62 snapshot push ok
    0x63 snapshot skipped 0x64 schema push ok     0x65 schema skipped
    0x66 pending fetched  0x67 pending applied    0x68 resetting
    0x6C BMC not ready    0x6D transfer failed

  Copyright (c) 2024, ASRockRack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "HostBmcKcs.h"
#include "BiosCfgOobVarstores.h"
#include "BiosCfgOobSchema.h"

/* ── IPMI numbers (must match biosconfigcommands.hpp) ─────────────────────── */
#define OOB_NETFN                 0x30u
#define CMD_SET_BIOS_CAP          0xD3u
#define CMD_GET_BIOS_CAP          0xD4u
#define CMD_SET_PAYLOAD           0xD5u
#define CMD_GET_PAYLOAD           0xD6u

#define XFER_START                0x00u
#define XFER_IN_PROGRESS          0x01u
#define XFER_END                  0x02u
#define XFER_ABORT                0x03u

#define GET_PARAM_INFO            0x00u
#define GET_PARAM_DATA            0x01u

#define PAYLOAD_SCHEMA            0x01u   /* XML, LZMA "alone" container */
#define PAYLOAD_SNAPSHOT          0x02u   /* "ASVS" live varstore bytes   */
#define PAYLOAD_PENDING           0x03u   /* "ASPS" staged Redfish writes */

#define PAYLOAD_STATUS_VALID      0x01u

/* The capability byte the BMC has always been told; kept so a BMC that has
   persisted it across our change sees no difference. */
#define OOB_CAPABILITY            0x02u

/* 48 data bytes per chunk. The KCS message limit is 255 bytes and the
   inProgress header costs 18, so this is comfortably inside it and matches
   the chunk size that has been running on this board. */
#define XFER_CHUNK                48u

/* Bounds. The snapshot is ~2 KB in practice (Setup 511 + ServerSetup 739 +
   UsbSupport 49 + AMITSESetup 65 + eleven small ones + headers). */
#define MAX_SNAPSHOT              8192u
#define MAX_VARSTORE_BYTES        2048u
#define MAX_PENDING_RECORDS       256u

/* ── on-the-wire payload layouts ──────────────────────────────────────────── */
#pragma pack(1)

/* SetPayload state 0 body. */
typedef struct {
  UINT16    Version;
  UINT32    TotalSize;
  UINT32    TotalChecksum;
  UINT8     Flag;
} OOB_START_XFER;

/* GetPayload param 0 response body (after the completion code). */
typedef struct {
  UINT16    Version;
  UINT8     Type;
  UINT32    TotalSize;
  UINT32    TotalChecksum;
  UINT8     Flag;
  UINT8     Status;
  UINT32    Timestamp;
} OOB_PAYLOAD_INFO;

/* Payload type 2 — live varstore snapshot, host -> BMC. */
typedef struct {
  CHAR8     Magic[4];        /* "ASVS" */
  UINT16    Version;         /* 1 */
  UINT16    Count;
  UINT32    Reserved;
} OOB_SNAPSHOT_HDR;

typedef struct {
  UINT8     VarIndex;        /* XML varstoreIndex */
  UINT8     NameLen;         /* ASCII, not NUL-terminated */
  UINT16    DataLen;
  UINT32    Attributes;      /* UEFI variable attributes, as read */
  UINT8     Guid[16];
  /* CHAR8 Name[NameLen]; UINT8 Data[DataLen]; padded to 4 bytes */
} OOB_SNAPSHOT_REC;

/* Payload type 3 — pending settings, BMC -> host. Fixed-size records keep the
   firmware-side parser trivial and bounded. */
typedef struct {
  CHAR8     Magic[4];        /* "ASPS" */
  UINT16    Version;         /* 1 */
  UINT16    Count;
  UINT32    Generation;      /* bumped by the BMC on every change */
} OOB_PENDING_HDR;

typedef struct {
  UINT8     VarIndex;
  UINT8     Size;            /* 1, 2, 4 or 8 */
  UINT16    Offset;
  UINT64    Value;           /* little-endian, low Size bytes are written */
} OOB_PENDING_REC;

#pragma pack()

/* Records the last pending generation this host actually applied, so a reset
   happens at most once per BMC-side change and never loops. */
STATIC CONST CHAR16  mAppliedVarName[] = L"AsrOobApplied";
STATIC EFI_GUID      mAppliedVarGuid = {
  0xA5B0C1D2, 0x3E4F, 0x4A5B,
  { 0x8C, 0x6D, 0x7E, 0x8F, 0x90, 0xA1, 0xB2, 0xC3 }
};

STATIC BOOLEAN  mOobDone      = FALSE;
STATIC BOOLEAN  mOobResetDone = FALSE;

/* ── CRC32 (zlib / boost::crc_32_type — what the BMC checks against) ──────── */
STATIC CONST UINT32  mCrcNibble[16] = {
  0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
  0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
  0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

STATIC UINT32
Crc32Update (
  UINT32       Crc,
  CONST UINT8  *Data,
  UINTN        Len
  )
{
  UINTN  i;

  for (i = 0; i < Len; i++) {
    Crc ^= Data[i];
    Crc = mCrcNibble[Crc & 0x0F] ^ (Crc >> 4);
    Crc = mCrcNibble[Crc & 0x0F] ^ (Crc >> 4);
  }
  return Crc;
}

STATIC UINT32
Crc32 (
  CONST UINT8  *Data,
  UINTN        Len
  )
{
  return Crc32Update (0xFFFFFFFFu, Data, Len) ^ 0xFFFFFFFFu;
}

/* ── one OOB command ──────────────────────────────────────────────────────── */

/**
  Issue NetFn 0x30 / Cmd with Body, returning the response data that follows
  the completion code.

  @param[out] Cc   IPMI completion code, valid whenever EFI_SUCCESS is
                   returned. Callers must check it: this BIOS-facing path
                   treats a non-zero code as "the BMC declined", never as a
                   transport failure.
**/
STATIC EFI_STATUS
OobCmd (
  UINT8        Cmd,
  CONST UINT8  *Body,
  UINT32       BodyLen,
  UINT8        *Out,
  UINT32       OutMax,
  UINT32       *OutLen,
  UINT8        *Cc
  )
{
  UINT8       Req[2 + 18 + XFER_CHUNK];
  UINT8       Raw[3 + 64];
  UINT32      RawLen = sizeof (Raw);
  UINT32      i      = 0;
  EFI_STATUS  Status;

  if (BodyLen > sizeof (Req) - 2) {
    return EFI_INVALID_PARAMETER;
  }

  Req[i++] = (UINT8)(OOB_NETFN << 2);
  Req[i++] = Cmd;
  if (BodyLen != 0) {
    CopyMem (&Req[i], Body, BodyLen);
    i += BodyLen;
  }

  Status = KcsTxn (Req, i, Raw, &RawLen);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (RawLen < 3) {
    return EFI_DEVICE_ERROR;
  }

  *Cc = Raw[2];
  if ((Out != NULL) && (OutMax != 0)) {
    UINT32  Data = RawLen - 3;

    if (Data > OutMax) {
      Data = OutMax;
    }
    CopyMem (Out, &Raw[3], Data);
    if (OutLen != NULL) {
      *OutLen = Data;
    }
  } else if (OutLen != NULL) {
    *OutLen = 0;
  }
  return EFI_SUCCESS;
}

/**
  Ask the BMC what it already holds for a payload type.

  @retval TRUE   Info was read; *Info is populated.
  @retval FALSE  The BMC did not answer, or answered short.
**/
STATIC BOOLEAN
OobGetInfo (
  UINT8             Type,
  OOB_PAYLOAD_INFO  *Info
  )
{
  UINT8   Body[2];
  UINT8   Rsp[sizeof (OOB_PAYLOAD_INFO) + 8];
  UINT32  RspLen = 0;
  UINT8   Cc     = 0xFF;

  Body[0] = GET_PARAM_INFO;
  Body[1] = Type;
  if (EFI_ERROR (OobCmd (CMD_GET_PAYLOAD, Body, sizeof (Body),
                         Rsp, sizeof (Rsp), &RspLen, &Cc)))
  {
    return FALSE;
  }
  if ((Cc != 0) || (RspLen < sizeof (OOB_PAYLOAD_INFO))) {
    return FALSE;
  }
  CopyMem (Info, Rsp, sizeof (OOB_PAYLOAD_INFO));
  return TRUE;
}

/**
  Push a whole payload with the chunked SetPayload state machine.

  Skips the transfer entirely when the BMC already reports a valid payload of
  this type with the same size and CRC32.

  @retval EFI_SUCCESS       Transferred, or already present (see *Skipped).
  @retval EFI_DEVICE_ERROR  The BMC rejected a step; the transfer was aborted.
**/
STATIC EFI_STATUS
OobSendPayload (
  UINT8        Type,
  CONST UINT8  *Data,
  UINT32       Len,
  UINT32       Crc,
  BOOLEAN      *Skipped
  )
{
  OOB_PAYLOAD_INFO  Info;
  OOB_START_XFER    Start;
  UINT8             Body[18 + XFER_CHUNK];
  UINT8             Rsp[8];
  UINT32            RspLen;
  UINT8             Cc  = 0xFF;
  UINT32            ResId;
  UINT32            Off;

  *Skipped = FALSE;

  if ((Data == NULL) || (Len == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (OobGetInfo (Type, &Info) &&
      (Info.Status == PAYLOAD_STATUS_VALID) &&
      (Info.TotalSize == Len) &&
      (Info.TotalChecksum == Crc))
  {
    *Skipped = TRUE;
    return EFI_SUCCESS;
  }

  /* --- state 0: start ------------------------------------------------- */
  Start.Version       = 1;
  Start.TotalSize     = Len;
  Start.TotalChecksum = Crc;
  Start.Flag          = 0;

  Body[0] = XFER_START;
  Body[1] = Type;
  CopyMem (&Body[2], &Start, sizeof (Start));
  RspLen = sizeof (Rsp);
  if (EFI_ERROR (OobCmd (CMD_SET_PAYLOAD, Body, 2 + sizeof (Start),
                         Rsp, sizeof (Rsp), &RspLen, &Cc)))
  {
    return EFI_DEVICE_ERROR;
  }
  if ((Cc != 0) || (RspLen < 4)) {
    return EFI_DEVICE_ERROR;
  }
  CopyMem (&ResId, Rsp, sizeof (ResId));

  /* --- state 1: chunks ------------------------------------------------ */
  for (Off = 0; Off < Len; ) {
    UINT32  Chunk = Len - Off;
    UINT32  ChunkCrc;

    if (Chunk > XFER_CHUNK) {
      Chunk = XFER_CHUNK;
    }
    ChunkCrc = Crc32 (Data + Off, Chunk);

    Body[0] = XFER_IN_PROGRESS;
    Body[1] = Type;
    CopyMem (&Body[2],  &ResId,    4);
    CopyMem (&Body[6],  &Off,      4);
    CopyMem (&Body[10], &Chunk,    4);
    CopyMem (&Body[14], &ChunkCrc, 4);
    CopyMem (&Body[18], Data + Off, Chunk);

    RspLen = sizeof (Rsp);
    if (EFI_ERROR (OobCmd (CMD_SET_PAYLOAD, Body, 18 + Chunk,
                           Rsp, sizeof (Rsp), &RspLen, &Cc)) || (Cc != 0))
    {
      goto Abort;
    }
    Off += Chunk;
  }

  /* --- state 2: end --------------------------------------------------- */
  Body[0] = XFER_END;
  Body[1] = Type;
  CopyMem (&Body[2], &ResId, 4);
  RspLen = sizeof (Rsp);
  if (EFI_ERROR (OobCmd (CMD_SET_PAYLOAD, Body, 6,
                         Rsp, sizeof (Rsp), &RspLen, &Cc)) || (Cc != 0))
  {
    goto Abort;
  }
  return EFI_SUCCESS;

Abort:
  /* Leave no half-open session behind: the BMC keeps one transfer slot and a
     stale reservation would reject the next boot's push too. */
  Body[0] = XFER_ABORT;
  Body[1] = Type;
  RspLen  = sizeof (Rsp);
  OobCmd (CMD_SET_PAYLOAD, Body, 2, Rsp, sizeof (Rsp), &RspLen, &Cc);
  return EFI_DEVICE_ERROR;
}

/**
  Fetch a whole payload with chunked GetPayload(Data), verifying each chunk's
  CRC32 and then the whole payload's. Caller frees *Out.
**/
STATIC EFI_STATUS
OobFetchPayload (
  UINT8   Type,
  UINT8   **Out,
  UINT32  *OutLen
  )
{
  OOB_PAYLOAD_INFO  Info;
  UINT8             Body[10];
  UINT8             Rsp[9 + XFER_CHUNK];
  UINT32            RspLen;
  UINT8             Cc = 0xFF;
  UINT8             *Buf;
  UINT32            Off;

  *Out    = NULL;
  *OutLen = 0;

  if (!OobGetInfo (Type, &Info)) {
    return EFI_NOT_FOUND;
  }
  if ((Info.Status != PAYLOAD_STATUS_VALID) || (Info.TotalSize == 0) ||
      (Info.TotalSize > MAX_SNAPSHOT))
  {
    return EFI_NOT_FOUND;
  }

  Buf = AllocatePool (Info.TotalSize);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Off = 0; Off < Info.TotalSize; ) {
    UINT32  Want = Info.TotalSize - Off;
    UINT32  ReadCount;
    UINT32  ChunkCrc;

    if (Want > XFER_CHUNK) {
      Want = XFER_CHUNK;
    }

    Body[0] = GET_PARAM_DATA;
    Body[1] = Type;
    CopyMem (&Body[2], &Off,  4);
    CopyMem (&Body[6], &Want, 4);

    RspLen = sizeof (Rsp);
    if (EFI_ERROR (OobCmd (CMD_GET_PAYLOAD, Body, sizeof (Body),
                           Rsp, sizeof (Rsp), &RspLen, &Cc)) || (Cc != 0))
    {
      goto Fail;
    }
    /* response: type(1) readCount(4) checksum(4) data... */
    if (RspLen < 9) {
      goto Fail;
    }
    CopyMem (&ReadCount, &Rsp[1], 4);
    CopyMem (&ChunkCrc,  &Rsp[5], 4);
    if ((ReadCount == 0) || (ReadCount > RspLen - 9)) {
      goto Fail;
    }
    if (Crc32 (&Rsp[9], ReadCount) != ChunkCrc) {
      goto Fail;
    }
    CopyMem (Buf + Off, &Rsp[9], ReadCount);
    Off += ReadCount;
  }

  if (Crc32 (Buf, Info.TotalSize) != Info.TotalChecksum) {
    goto Fail;
  }

  *Out    = Buf;
  *OutLen = Info.TotalSize;
  return EFI_SUCCESS;

Fail:
  FreePool (Buf);
  return EFI_DEVICE_ERROR;
}

/* ── payload type 2: live varstore snapshot ───────────────────────────────── */

/**
  Read every known varstore with gRT->GetVariable and serialise them into the
  "ASVS" blob. Varstores that do not exist on this board (or are larger than
  MAX_VARSTORE_BYTES) are simply left out; the BMC publishes the knobs behind a
  missing varstore as read-only rather than inventing values for them.

  Caller frees *Out.
**/
STATIC EFI_STATUS
BuildSnapshot (
  UINT8   **Out,
  UINT32  *OutLen
  )
{
  UINT8             *Buf;
  UINT32            Off = sizeof (OOB_SNAPSHOT_HDR);
  UINT16            Count = 0;
  UINTN             i;
  OOB_SNAPSHOT_HDR  Hdr;

  Buf = AllocatePool (MAX_SNAPSHOT);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (Buf, MAX_SNAPSHOT);

  for (i = 0; i < OOB_VARSTORE_COUNT; i++) {
    CONST OOB_VARSTORE  *Vs = &mOobVarstores[i];
    UINTN               Size = 0;
    UINT32              Attr = 0;
    EFI_STATUS          Status;
    OOB_SNAPSHOT_REC    Rec;
    UINTN               NameLen;
    UINT32              Need;
    UINTN               n;
    EFI_GUID            Guid;

    /* GetVariable takes a non-const GUID pointer; keep the table itself const
       by handing it a local copy. */
    CopyMem (&Guid, &Vs->Guid, sizeof (Guid));

    Status = gRT->GetVariable ((CHAR16 *)Vs->Name, &Guid, &Attr, &Size, NULL);
    if ((Status != EFI_BUFFER_TOO_SMALL) || (Size == 0) ||
        (Size > MAX_VARSTORE_BYTES))
    {
      continue;
    }

    NameLen = StrLen (Vs->Name);
    Need    = (UINT32)(sizeof (Rec) + NameLen + Size);
    Need    = (Need + 3) & ~3u;
    if (Off + Need > MAX_SNAPSHOT) {
      continue;
    }

    /* Variable names on this board are ASCII; narrow them so the BMC can use
       them as plain strings without a UTF-16 decode. */
    for (n = 0; n < NameLen; n++) {
      Buf[Off + sizeof (Rec) + n] = (UINT8)Vs->Name[n];
    }

    /* Read the data BEFORE building the record header: the size probe above is
       only required to return the length, so Attr is not trustworthy until a
       call that actually retrieves the variable has succeeded. */
    Status = gRT->GetVariable ((CHAR16 *)Vs->Name, &Guid, &Attr, &Size,
                               Buf + Off + sizeof (Rec) + NameLen);
    if (EFI_ERROR (Status)) {
      ZeroMem (Buf + Off, Need);
      continue;
    }

    Rec.VarIndex   = Vs->Index;
    Rec.NameLen    = (UINT8)NameLen;
    Rec.DataLen    = (UINT16)Size;
    Rec.Attributes = Attr;
    CopyMem (Rec.Guid, &Vs->Guid, sizeof (Rec.Guid));
    CopyMem (Buf + Off, &Rec, sizeof (Rec));

    Off += Need;
    Count++;
  }

  if (Count == 0) {
    FreePool (Buf);
    return EFI_NOT_FOUND;
  }

  Hdr.Magic[0] = 'A';
  Hdr.Magic[1] = 'S';
  Hdr.Magic[2] = 'V';
  Hdr.Magic[3] = 'S';
  Hdr.Version  = 1;
  Hdr.Count    = Count;
  Hdr.Reserved = 0;
  CopyMem (Buf, &Hdr, sizeof (Hdr));

  *Out    = Buf;
  *OutLen = Off;
  return EFI_SUCCESS;
}

/* ── payload type 3: apply pending settings ───────────────────────────────── */

STATIC CONST OOB_VARSTORE *
FindVarstore (
  UINT8  Index
  )
{
  UINTN  i;

  for (i = 0; i < OOB_VARSTORE_COUNT; i++) {
    if (mOobVarstores[i].Index == Index) {
      return &mOobVarstores[i];
    }
  }
  return NULL;
}

/**
  Apply one staged write to its backing UEFI variable.

  Reads the whole variable, patches Size bytes at Offset, writes it back with
  the attributes it already had, then re-reads to confirm the bytes stuck. A
  write that does not verify is reported as not applied, which keeps the reset
  below from firing on a variable the firmware silently refuses.

  @retval TRUE   The variable now holds Value at Offset.
  @retval FALSE  Not applied (unknown varstore, out of range, or write refused).
**/
STATIC BOOLEAN
ApplyPendingRecord (
  CONST OOB_PENDING_REC  *Rec
  )
{
  CONST OOB_VARSTORE  *Vs;
  UINTN               Size = 0;
  UINT32              Attr = 0;
  EFI_STATUS          Status;
  UINT8               *Buf;
  BOOLEAN             Ok = FALSE;
  UINTN               n;
  EFI_GUID            Guid;

  if ((Rec->Size == 0) || (Rec->Size > 8)) {
    return FALSE;
  }
  Vs = FindVarstore (Rec->VarIndex);
  if (Vs == NULL) {
    return FALSE;
  }
  CopyMem (&Guid, &Vs->Guid, sizeof (Guid));

  Status = gRT->GetVariable ((CHAR16 *)Vs->Name, &Guid, &Attr, &Size, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (Size == 0) ||
      (Size > MAX_VARSTORE_BYTES))
  {
    return FALSE;
  }
  if ((UINTN)Rec->Offset + Rec->Size > Size) {
    return FALSE;
  }

  Buf = AllocatePool (Size);
  if (Buf == NULL) {
    return FALSE;
  }
  Status = gRT->GetVariable ((CHAR16 *)Vs->Name, &Guid, &Attr, &Size, Buf);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (n = 0; n < Rec->Size; n++) {
    Buf[Rec->Offset + n] = (UINT8)(Rec->Value >> (8 * n));
  }

  Status = gRT->SetVariable ((CHAR16 *)Vs->Name, &Guid, Attr, Size, Buf);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  /* Verify: a successful SetVariable is not proof the value survived. */
  ZeroMem (Buf, Size);
  Status = gRT->GetVariable ((CHAR16 *)Vs->Name, &Guid, &Attr, &Size, Buf);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  Ok = TRUE;
  for (n = 0; n < Rec->Size; n++) {
    if (Buf[Rec->Offset + n] != (UINT8)(Rec->Value >> (8 * n))) {
      Ok = FALSE;
      break;
    }
  }

Done:
  FreePool (Buf);
  return Ok;
}

/**
  Pull the BMC's staged settings and write them into the varstores.

  @param[out] Applied     Number of records that verified.
  @param[out] Generation  Generation of the blob that was processed.

  @retval TRUE  A blob was fetched and parsed (Applied may still be 0).
**/
STATIC BOOLEAN
ApplyPending (
  UINT32  *Applied,
  UINT32  *Generation
  )
{
  UINT8             *Blob = NULL;
  UINT32            BlobLen = 0;
  OOB_PENDING_HDR   Hdr;
  UINT32            i;

  *Applied    = 0;
  *Generation = 0;

  if (EFI_ERROR (OobFetchPayload (PAYLOAD_PENDING, &Blob, &BlobLen))) {
    return FALSE;
  }
  if (BlobLen < sizeof (Hdr)) {
    FreePool (Blob);
    return FALSE;
  }
  CopyMem (&Hdr, Blob, sizeof (Hdr));
  if ((Hdr.Magic[0] != 'A') || (Hdr.Magic[1] != 'S') ||
      (Hdr.Magic[2] != 'P') || (Hdr.Magic[3] != 'S') || (Hdr.Version != 1))
  {
    FreePool (Blob);
    return FALSE;
  }
  if ((Hdr.Count > MAX_PENDING_RECORDS) ||
      (sizeof (Hdr) + (UINTN)Hdr.Count * sizeof (OOB_PENDING_REC) > BlobLen))
  {
    FreePool (Blob);
    return FALSE;
  }

  *Generation = Hdr.Generation;
  Post (0x66);

  for (i = 0; i < Hdr.Count; i++) {
    OOB_PENDING_REC  Rec;

    CopyMem (&Rec, Blob + sizeof (Hdr) + i * sizeof (Rec), sizeof (Rec));
    if (ApplyPendingRecord (&Rec)) {
      (*Applied)++;
    }
  }

  FreePool (Blob);
  return TRUE;
}

/* ── orchestration ────────────────────────────────────────────────────────── */

VOID
BiosCfgOobInit (
  VOID
  )
{
  mOobDone      = FALSE;
  mOobResetDone = FALSE;
}

BOOLEAN
BiosCfgOobIsDone (
  VOID
  )
{
  return mOobDone;
}

VOID
BiosCfgOobRun (
  VOID
  )
{
  UINT8       Cap[4];
  UINT8       Cc = 0xFF;
  UINT32      RspLen = 0;
  UINT8       *Snap = NULL;
  UINT32      SnapLen = 0;
  BOOLEAN     Skipped = FALSE;
  UINT32      Applied = 0;
  UINT32      Generation = 0;
  UINT32      LastGen = 0;
  UINTN       LastGenSize = sizeof (LastGen);
  UINT32      Attr = 0;

  if (mOobDone) {
    return;
  }
  Post (0x60);

  /* Announce capabilities first, exactly as the previous driver did, so a BMC
     that has persisted the capability byte sees no change. A failure here
     means KCS is not usable yet — retry on the next tick rather than
     half-completing the exchange. */
  Cap[0] = OOB_CAPABILITY;
  Cap[1] = 0;
  Cap[2] = 0;
  Cap[3] = 0;
  if (EFI_ERROR (OobCmd (CMD_SET_BIOS_CAP, Cap, sizeof (Cap),
                         NULL, 0, &RspLen, &Cc)))
  {
    Post (0x6C);
    return;
  }

  /* 1. Take the BMC's staged writes first, so the snapshot we publish below
        already reflects them and the BMC can retire the pending entries. */
  if (ApplyPending (&Applied, &Generation) && (Applied != 0)) {
    Post (0x67);

    /* Setup has already been consumed by the time this driver runs, so the
       new values only take effect after a restart. Bound that to one reset
       per BMC-side change: record the generation we acted on BEFORE resetting,
       and never reset twice in one boot. */
    LastGen = 0;
    if (EFI_ERROR (gRT->GetVariable ((CHAR16 *)mAppliedVarName,
                                     &mAppliedVarGuid, &Attr,
                                     &LastGenSize, &LastGen)))
    {
      LastGen = 0;
    }
    if (!mOobResetDone && (Generation != 0) && (Generation != LastGen)) {
      mOobResetDone = TRUE;
      gRT->SetVariable ((CHAR16 *)mAppliedVarName, &mAppliedVarGuid,
                        EFI_VARIABLE_NON_VOLATILE |
                        EFI_VARIABLE_BOOTSERVICE_ACCESS,
                        sizeof (Generation), &Generation);
      Post (0x68);
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      /* not reached */
    }
  }

  /* 2. Schema. Static firmware metadata, so this is skipped on every boot
        after the first — the BMC is asked what it holds before we send. */
  if (EFI_ERROR (OobSendPayload (PAYLOAD_SCHEMA, mBiosCfgSchema,
                                 mBiosCfgSchemaSize, mBiosCfgSchemaCrc32,
                                 &Skipped)))
  {
    Post (0x6D);
    return;
  }
  Post (Skipped ? 0x65 : 0x64);

  /* 3. Live values. Small, and the whole point of this module. */
  if (EFI_ERROR (BuildSnapshot (&Snap, &SnapLen))) {
    Post (0x6D);
    return;
  }
  Post (0x61);

  if (EFI_ERROR (OobSendPayload (PAYLOAD_SNAPSHOT, Snap, SnapLen,
                                 Crc32 (Snap, SnapLen), &Skipped)))
  {
    FreePool (Snap);
    Post (0x6D);
    return;
  }
  FreePool (Snap);
  Post (Skipped ? 0x63 : 0x62);

  mOobDone = TRUE;
}
