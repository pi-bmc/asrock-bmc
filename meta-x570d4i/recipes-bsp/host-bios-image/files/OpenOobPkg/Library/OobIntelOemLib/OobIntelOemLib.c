/** @file
  Host side of the openbmc/intel-ipmi-oem command set. See
  Include/Library/OobIntelOemLib.h for scope, the MDR II exclusion, and the
  error convention.

  Every request and response layout below was transcribed from the matching
  handler in intel-ipmi-oem; the source line is cited at each function so the
  two can be re-checked against each other after an uprev.

  Two properties of ipmid's marshalling shape most of this file and are worth
  stating once:

    * Sub-byte fields are packed LSB-first and coalesced into whole bytes. A
      handler taking (bool, bool, bool, uint5_t) reads ONE byte, not four.
    * std::string is UCSD-Pascal style: a single length byte, then that many
      characters, with no NUL terminator.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

#include <Library/IpmiKcsLib.h>
#include <Library/OobIntelOemLib.h>

/* Add SEL Entry takes the record as raw bytes, so a stray compiler pad would
   silently shift every field past it. IPMI v2.0 Table 32 fixes the size at 16. */
STATIC_ASSERT (
  sizeof (OOB_IPMI_SEL_RECORD) == 16,
  "SEL record must be exactly 16 bytes on the wire"
  );

/* Seven wire bytes for fourteen RspType fields; see the struct's comment. */
STATIC_ASSERT (
  sizeof (OOB_INTEL_PROC_ERR_CONFIG) == 7,
  "Get Processor Error Config response must be 7 bytes"
  );

/**
  Run one IPMI transaction and apply this library's error convention.

  @param[in]     NetFn     IPMI network function.
  @param[in]     Cmd       IPMI command.
  @param[in]     Req       Request body, or NULL.
  @param[in]     ReqLen    Length of Req.
  @param[out]    Rsp       Response body after the completion code, or NULL.
  @param[in,out] RspLen    Capacity on entry, bytes written on exit.
  @param[in]     MinRspLen Shortest response the caller can use. A shorter reply
                           with a success completion code is a protocol error,
                           not a truncation to paper over.
  @param[out]    CcOut     Optional; receives the completion code. When NULL a
                           non-zero code becomes EFI_PROTOCOL_ERROR.
**/
STATIC
EFI_STATUS
OobOemXact (
  IN     UINT8       NetFn,
  IN     UINT8       Cmd,
  IN     CONST VOID  *Req      OPTIONAL,
  IN     UINTN       ReqLen,
  OUT    VOID        *Rsp      OPTIONAL,
  IN OUT UINTN       *RspLen   OPTIONAL,
  IN     UINTN       MinRspLen,
  OUT    UINT8       *CcOut    OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT8       Cc      = 0xFF;
  UINTN       Got     = 0;

  /* A response buffer with no length is a caller bug that would otherwise
     present as a mysterious EFI_BUFFER_TOO_SMALL: Got would stay 0 and the
     transport would see a zero-capacity buffer. */
  if ((Rsp != NULL) != (RspLen != NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Rsp != NULL) {
    Got = *RspLen;
  }

  Status = IpmiKcsCommand (
             NetFn,
             Cmd,
             Req,
             ReqLen,
             &Cc,
             Rsp,
             (Rsp != NULL) ? &Got : NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CcOut != NULL) {
    *CcOut = Cc;
  }

  if (Cc != 0) {
    /* The BMC answered and declined. A caller that asked for the code owns the
       decision; one that did not gets a distinct status so it cannot mistake
       a refusal for a completed operation. */
    if (RspLen != NULL) {
      *RspLen = 0;
    }
    return (CcOut != NULL) ? EFI_SUCCESS : EFI_PROTOCOL_ERROR;
  }

  if (Got < MinRspLen) {
    return EFI_DEVICE_ERROR;
  }

  if (RspLen != NULL) {
    *RspLen = Got;
  }

  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* NetFn 0x30 — intel::netFnGeneral                                          */
/* ------------------------------------------------------------------------- */

/**
  Get BMC Version String (0x01).

  Reference: ipmiOEMGetBmcVersionString, oemcommands.cpp. Returns
  RspType<std::string>, so the wire form is [len][chars] with no terminator;
  this adds one.
**/
EFI_STATUS
EFIAPI
OobIntelGetBmcVersionString (
  OUT    CHAR8  *Version,
  IN OUT UINTN  *VersionSize,
  OUT    UINT8  *CompletionCode  OPTIONAL
  )
{
  UINT8       Rsp[1 + 255];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;
  UINTN       Len;

  if ((Version == NULL) || (VersionSize == NULL) || (*VersionSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_GET_BMC_VERSION_STRING,
             NULL,
             0,
             Rsp,
             &RspLen,
             1,
             CompletionCode
             );
  if (EFI_ERROR (Status) || (RspLen == 0)) {
    return Status;
  }

  Len = Rsp[0];
  if (Len > RspLen - 1) {
    /* Length byte disagrees with what actually arrived; trust the short one. */
    Len = RspLen - 1;
  }
  if (Len > *VersionSize - 1) {
    Len = *VersionSize - 1;
  }

  CopyMem (Version, &Rsp[1], Len);
  Version[Len] = '\0';
  *VersionSize = Len;

  return EFI_SUCCESS;
}

/**
  Restore Configuration (0x02).

  Reference: ipmiRestoreConfiguration, oemcommands.cpp:3111. Request is the
  literal "CLR" followed by the sub-command; response is one status byte.
**/
EFI_STATUS
EFIAPI
OobIntelRestoreConfiguration (
  IN  UINT8  SubCommand,
  OUT UINT8  *RestoreStatus  OPTIONAL,
  OUT UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8       Req[4] = { 'C', 'L', 'R', 0 };
  UINT8       Rsp[1];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  Req[3] = SubCommand;

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_RESTORE_CONFIGURATION,
             Req,
             sizeof (Req),
             Rsp,
             &RspLen,
             1,
             CompletionCode
             );
  if (!EFI_ERROR (Status) && (RspLen >= 1) && (RestoreStatus != NULL)) {
    *RestoreStatus = Rsp[0];
  }

  return Status;
}

/**
  Get OEM Device Info (0x27).

  Reference: ipmiOEMGetDeviceInfo, oemcommands.cpp. Count and Offset are
  std::optional and are only meaningful for the BIOS-ID entity; the handler
  returns "request data length invalid" if they are missing for that entity, so
  they are sent only when Count is non-zero.
**/
EFI_STATUS
EFIAPI
OobIntelGetOemDeviceInfo (
  IN     UINT8  EntityType,
  IN     UINT8  Count,
  IN     UINT8  Offset,
  OUT    VOID   *Data,
  IN OUT UINTN  *DataSize,
  OUT    UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8  Req[3];
  UINTN  ReqLen;

  if ((Data == NULL) || (DataSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = EntityType;
  ReqLen = 1;
  if (Count != 0) {
    Req[1] = Count;
    Req[2] = Offset;
    ReqLen = 3;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_GET_OEM_DEVICE_INFO,
           Req,
           ReqLen,
           Data,
           DataSize,
           0,
           CompletionCode
           );
}

/**
  Send Embedded Firmware Update Status (0x44).

  Reference: ipmiOEMSendEmbeddedFwUpdStatus, oemcommands.cpp. Request is
  (status, target, major, minor, uint32 auxInfo) — eight bytes, auxInfo
  little-endian.
**/
EFI_STATUS
EFIAPI
OobIntelSendEmbeddedFwUpdStatus (
  IN  UINT8   Status,
  IN  UINT8   Target,
  IN  UINT8   MajorRevision,
  IN  UINT8   MinorRevision,
  IN  UINT32  AuxInfo,
  OUT UINT8   *CompletionCode OPTIONAL
  )
{
  UINT8  Req[8];

  Req[0] = Status;
  Req[1] = Target;
  Req[2] = MajorRevision;
  Req[3] = MinorRevision;
  WriteUnaligned32 ((UINT32 *)&Req[4], AuxInfo);

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SEND_FW_UPD_STATUS,
           Req,
           sizeof (Req),
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Set Fault Indication (0x57).

  Reference: ipmiOEMSetFaultIndication, oemcommands.cpp. Request is
  (sourceId, faultType, faultState, faultGroup, uint8[8] ledStateData).
**/
EFI_STATUS
EFIAPI
OobIntelSetFaultIndication (
  IN  UINT8        SourceId,
  IN  UINT8        FaultType,
  IN  UINT8        FaultState,
  IN  UINT8        FaultGroup,
  IN  CONST UINT8  LedStateData[8],
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[12];

  if (LedStateData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = SourceId;
  Req[1] = FaultType;
  Req[2] = FaultState;
  Req[3] = FaultGroup;
  CopyMem (&Req[4], LedStateData, 8);

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_FAULT_INDICATION,
           Req,
           sizeof (Req),
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Get Buffer Size (0x66).

  Reference: ipmiOEMGetBufferSize, oemcommands.cpp. Both response bytes are in
  four-byte units; multiplied out here so callers deal in bytes.
**/
EFI_STATUS
EFIAPI
OobIntelGetBufferSize (
  OUT UINT16  *KcsMaxBytes,
  OUT UINT16  *IpmbMaxBytes,
  OUT UINT8   *CompletionCode OPTIONAL
  )
{
  UINT8       Rsp[2];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_GET_BUFFER_SIZE,
             NULL,
             0,
             Rsp,
             &RspLen,
             2,
             CompletionCode
             );
  if (EFI_ERROR (Status) || (RspLen < 2)) {
    return Status;
  }

  if (KcsMaxBytes != NULL) {
    *KcsMaxBytes = (UINT16)(Rsp[0] * 4);
  }
  if (IpmbMaxBytes != NULL) {
    *IpmbMaxBytes = (UINT16)(Rsp[1] * 4);
  }

  return EFI_SUCCESS;
}

/**
  Set DIMM Offset (0x8E).

  Reference: ipmiOEMSetDimmOffset, oemcommands.cpp. Request is a type byte
  followed by a vector of (index, value) tuples, which is just the pairs laid
  end to end. The handler rejects an empty vector.
**/
EFI_STATUS
EFIAPI
OobIntelSetDimmOffset (
  IN  UINT8                              Type,
  IN  CONST OOB_INTEL_DIMM_OFFSET_ENTRY  *Entries,
  IN  UINTN                              EntryCount,
  OUT UINT8                              *CompletionCode OPTIONAL
  )
{
  UINT8  Req[1 + (2 * 64)];
  UINTN  Index;

  if ((Entries == NULL) || (EntryCount == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (EntryCount > (sizeof (Req) - 1) / 2) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = Type;
  for (Index = 0; Index < EntryCount; Index++) {
    Req[1 + (Index * 2)]     = Entries[Index].Index;
    Req[1 + (Index * 2) + 1] = Entries[Index].Value;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_DIMM_OFFSET,
           Req,
           1 + (EntryCount * 2),
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Get DIMM Offset (0x8F).

  Reference: ipmiOEMGetDimmOffset(uint8_t type, uint8_t index), oemcommands.cpp.
**/
EFI_STATUS
EFIAPI
OobIntelGetDimmOffset (
  IN  UINT8  Type,
  IN  UINT8  Index,
  OUT UINT8  *Value,
  OUT UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8       Req[2];
  UINT8       Rsp[1];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = Type;
  Req[1] = Index;

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_GET_DIMM_OFFSET,
             Req,
             sizeof (Req),
             Rsp,
             &RspLen,
             1,
             CompletionCode
             );
  if (!EFI_ERROR (Status) && (RspLen >= 1)) {
    *Value = Rsp[0];
  }

  return Status;
}

/**
  Read Base Board Product ID (0x93).

  Reference: ipmiOEMReadBoardProductId, oemcommands.cpp. No request body.
**/
EFI_STATUS
EFIAPI
OobIntelReadBaseBoardProductId (
  OUT UINT8  *ProductId,
  OUT UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8       Rsp[1];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  if (ProductId == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_READ_BOARD_PRODUCT_ID,
             NULL,
             0,
             Rsp,
             &RspLen,
             1,
             CompletionCode
             );
  if (!EFI_ERROR (Status) && (RspLen >= 1)) {
    *ProductId = Rsp[0];
  }

  return Status;
}

/**
  Get Processor Error Config (0x9A).

  Reference: oemcommands.cpp:938. Fourteen RspType fields, seven wire bytes —
  see OOB_INTEL_PROC_ERR_CONFIG.
**/
EFI_STATUS
EFIAPI
OobIntelGetProcessorErrConfig (
  OUT OOB_INTEL_PROC_ERR_CONFIG  *Config,
  OUT UINT8                      *CompletionCode OPTIONAL
  )
{
  UINTN  RspLen = sizeof (OOB_INTEL_PROC_ERR_CONFIG);

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Config, sizeof (*Config));

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_GET_PROC_ERR_CONFIG,
           NULL,
           0,
           Config,
           &RspLen,
           sizeof (OOB_INTEL_PROC_ERR_CONFIG),
           CompletionCode
           );
}

/**
  Set Processor Error Config (0x9B).

  Reference: ipmiOEMSetProcessorErrConfig, oemcommands.cpp. The three reset
  bools plus a 5-bit reserved occupy byte 0; byte 1 is a reserved uint8. The
  handler rejects the request outright if either reserved field is non-zero, so
  both are forced to zero here rather than exposed. The optional third byte
  carries the two clear-counter bools with a 6-bit reserved above them.
**/
EFI_STATUS
EFIAPI
OobIntelSetProcessorErrConfig (
  IN  BOOLEAN      ResetOnIerr,
  IN  BOOLEAN      ResetOnErr2,
  IN  BOOLEAN      ResetOnMcerr,
  IN  CONST UINT8  *ClearCounts OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[3];
  UINTN  ReqLen;

  Req[0] = (UINT8)((ResetOnIerr  ? OOB_INTEL_PROC_ERR_RESET_ON_IERR  : 0) |
                   (ResetOnErr2  ? OOB_INTEL_PROC_ERR_RESET_ON_ERR2  : 0) |
                   (ResetOnMcerr ? OOB_INTEL_PROC_ERR_RESET_ON_MCERR : 0));
  Req[1] = 0;
  ReqLen = 2;

  if (ClearCounts != NULL) {
    /* Only the two defined bits; the handler rejects a non-zero reserved3. */
    Req[2] = (UINT8)(*ClearCounts & (OOB_INTEL_PROC_ERR_CLEAR_CPU_COUNT |
                                     OOB_INTEL_PROC_ERR_CLEAR_CRASHDUMP));
    ReqLen = 3;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_PROC_ERR_CONFIG,
           Req,
           ReqLen,
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Get Security Mode (0xB3).

  Reference: ipmiGetSecurityMode, oemcommands.cpp:2951. Two response bytes:
  restriction mode then special mode.
**/
EFI_STATUS
EFIAPI
OobIntelGetSecurityMode (
  OUT UINT8  *RestrictionMode OPTIONAL,
  OUT UINT8  *SpecialMode     OPTIONAL,
  OUT UINT8  *CompletionCode  OPTIONAL
  )
{
  UINT8       Rsp[2];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_GET_SECURITY_MODE,
             NULL,
             0,
             Rsp,
             &RspLen,
             2,
             CompletionCode
             );
  if (EFI_ERROR (Status) || (RspLen < 2)) {
    return Status;
  }

  if (RestrictionMode != NULL) {
    *RestrictionMode = Rsp[0];
  }
  if (SpecialMode != NULL) {
    *SpecialMode = Rsp[1];
  }

  return EFI_SUCCESS;
}

/**
  Set Security Mode (0xB4).

  Reference: ipmiSetSecurityMode, oemcommands.cpp. The special-mode byte is
  std::optional, so a one-byte request is legal.
**/
EFI_STATUS
EFIAPI
OobIntelSetSecurityMode (
  IN  UINT8        RestrictionMode,
  IN  CONST UINT8  *SpecialMode OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[2];
  UINTN  ReqLen;

  Req[0] = RestrictionMode;
  ReqLen = 1;
  if (SpecialMode != NULL) {
    Req[1] = *SpecialMode;
    ReqLen = 2;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_SECURITY_MODE,
           Req,
           ReqLen,
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Set BIOS Password Hash Info (0xD7).

  Reference: ipmiOEMSetBIOSHashInfo, biosconfigcommands.cpp. Fixed 97-byte
  request: 32-byte seed, algorithm byte, 64-byte hash.
**/
EFI_STATUS
EFIAPI
OobIntelSetBiosPwdHashInfo (
  IN  CONST UINT8  PasswordSeed[OOB_INTEL_PWD_SEED_SIZE],
  IN  UINT8        AlgorithmInfo,
  IN  CONST UINT8  AdminPwdHash[OOB_INTEL_PWD_HASH_SIZE],
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[OOB_INTEL_PWD_SEED_SIZE + 1 + OOB_INTEL_PWD_HASH_SIZE];

  if ((PasswordSeed == NULL) || (AdminPwdHash == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (&Req[0], PasswordSeed, OOB_INTEL_PWD_SEED_SIZE);
  Req[OOB_INTEL_PWD_SEED_SIZE] = AlgorithmInfo;
  CopyMem (
    &Req[OOB_INTEL_PWD_SEED_SIZE + 1],
    AdminPwdHash,
    OOB_INTEL_PWD_HASH_SIZE
    );

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_BIOS_PWD_HASH_INFO,
           Req,
           sizeof (Req),
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Get BIOS Password Hash (0xD8).

  Reference: ipmiOEMGetBIOSHash, biosconfigcommands.cpp:1119. Refused once POST
  has completed, so this only answers from within the DXE phase.
**/
EFI_STATUS
EFIAPI
OobIntelGetBiosPwdHash (
  OUT    VOID   *Data,
  IN OUT UINTN  *DataSize,
  OUT    UINT8  *CompletionCode OPTIONAL
  )
{
  if ((Data == NULL) || (DataSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_GET_BIOS_PWD_HASH,
           NULL,
           0,
           Data,
           DataSize,
           0,
           CompletionCode
           );
}

/**
  Get NMI Source (0xE5).

  Reference: ipmiOEMGetNmiSource, oemcommands.cpp.
**/
EFI_STATUS
EFIAPI
OobIntelGetNmiSource (
  OUT UINT8  *Source,
  OUT UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8       Rsp[1];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  if (Source == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = OobOemXact (
             OOB_INTEL_NETFN_GENERAL,
             OOB_INTEL_CMD_GET_NMI_SOURCE,
             NULL,
             0,
             Rsp,
             &RspLen,
             1,
             CompletionCode
             );
  if (!EFI_ERROR (Status) && (RspLen >= 1)) {
    *Source = Rsp[0];
  }

  return Status;
}

/**
  Set NMI Source (0xED).

  Reference: ipmiOEMSetNmiSource(uint8_t sourceId), oemcommands.cpp.
**/
EFI_STATUS
EFIAPI
OobIntelSetNmiSource (
  IN  UINT8  Source,
  OUT UINT8  *CompletionCode OPTIONAL
  )
{
  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_NMI_SOURCE,
           &Source,
           1,
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Set EFI Boot Options (0xEA).

  Reference: ipmiOemSetEfiBootOptions, oemcommands.cpp:3484. Rejects
  (0, 0) outright, rejects any parameter other than set-in-progress or boot
  flags, and requires the trailing option byte for boot flags while rejecting it
  for set-in-progress. Those four rules are enforced here so a malformed request
  never reaches KCS.
**/
EFI_STATUS
EFIAPI
OobIntelSetEfiBootOptions (
  IN  UINT8        Parameter,
  IN  UINT8        Data,
  IN  CONST UINT8  *BootOption OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[3];
  UINTN  ReqLen;

  if ((Parameter == 0) && (Data == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Parameter == OOB_INTEL_BOOT_PARAM_SET_IN_PROGRESS) {
    if (BootOption != NULL) {
      return EFI_INVALID_PARAMETER;
    }
  } else if (Parameter == OOB_INTEL_BOOT_PARAM_BOOT_FLAGS) {
    if (BootOption == NULL) {
      return EFI_INVALID_PARAMETER;
    }
  } else {
    return EFI_UNSUPPORTED;
  }

  Req[0] = Parameter;
  Req[1] = Data;
  ReqLen = 2;
  if (BootOption != NULL) {
    Req[2] = *BootOption;
    ReqLen = 3;
  }

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_SET_EFI_BOOT_OPTIONS,
           Req,
           ReqLen,
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Get EFI Boot Options (0xEB).

  Reference: ipmiOemGetEfiBootOptions, oemcommands.cpp:3489. Response leads with
  a version byte and echoes the parameter.
**/
EFI_STATUS
EFIAPI
OobIntelGetEfiBootOptions (
  IN     UINT8  Parameter,
  IN     UINT8  Set,
  IN     UINT8  Block,
  OUT    VOID   *Data,
  IN OUT UINTN  *DataSize,
  OUT    UINT8  *CompletionCode OPTIONAL
  )
{
  UINT8  Req[3];

  if ((Data == NULL) || (DataSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = Parameter;
  Req[1] = Set;
  Req[2] = Block;

  return OobOemXact (
           OOB_INTEL_NETFN_GENERAL,
           OOB_INTEL_CMD_GET_EFI_BOOT_OPTIONS,
           Req,
           sizeof (Req),
           Data,
           DataSize,
           2,
           CompletionCode
           );
}

/* ------------------------------------------------------------------------- */
/* Standard NetFns that intel-ipmi-oem also answers                          */
/* ------------------------------------------------------------------------- */

/**
  Platform Event (Sensor 0x04, cmd 0x02).

  Reference: ipmiSenPlatformEvent, sensorcommands.cpp. On the system interface
  the handler unpacks a leading software ID and returns "invalid field" unless
  bit 0 of it is set, so that is checked here rather than spending a KCS round
  trip to be told.

  It also calls p.fullyUnpacked(), which fails on trailing bytes — hence
  EventData3 being ignored unless EventData2 is present, so the message never
  ends up with a gap the unpacker would read as garbage.
**/
EFI_STATUS
EFIAPI
OobIpmiPlatformEvent (
  IN  UINT8        SoftwareId,
  IN  UINT8        EvmRev,
  IN  UINT8        SensorType,
  IN  UINT8        SensorNumber,
  IN  UINT8        EventDirType,
  IN  UINT8        EventData1,
  IN  CONST UINT8  *EventData2 OPTIONAL,
  IN  CONST UINT8  *EventData3 OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  )
{
  UINT8  Req[9];
  UINTN  ReqLen;

  if ((SoftwareId & BIT0) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Req[0] = SoftwareId;
  Req[1] = EvmRev;
  Req[2] = SensorType;
  Req[3] = SensorNumber;
  Req[4] = EventDirType;
  Req[5] = EventData1;
  ReqLen = 6;

  if (EventData2 != NULL) {
    Req[6] = *EventData2;
    ReqLen = 7;

    if (EventData3 != NULL) {
      Req[7] = *EventData3;
      ReqLen = 8;
    }
  }

  return OobOemXact (
           IPMI_NETFN_SENSOR,
           OOB_IPMI_CMD_PLATFORM_EVENT,
           Req,
           ReqLen,
           NULL,
           NULL,
           0,
           CompletionCode
           );
}

/**
  Add SEL Entry (Storage 0x0A, cmd 0x44).

  Reference: ipmiStorageAddSELEntry, storagecommands.cpp. Request is the 16-byte
  record; response is the assigned record ID.
**/
EFI_STATUS
EFIAPI
OobIpmiAddSelEntry (
  IN  CONST OOB_IPMI_SEL_RECORD  *Record,
  OUT UINT16                     *RecordId OPTIONAL,
  OUT UINT8                      *CompletionCode OPTIONAL
  )
{
  UINT8       Rsp[2];
  UINTN       RspLen = sizeof (Rsp);
  EFI_STATUS  Status;

  if (Record == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = OobOemXact (
             IPMI_NETFN_STORAGE,
             OOB_IPMI_CMD_ADD_SEL_ENTRY,
             Record,
             sizeof (OOB_IPMI_SEL_RECORD),
             Rsp,
             &RspLen,
             2,
             CompletionCode
             );
  if (!EFI_ERROR (Status) && (RspLen >= 2) && (RecordId != NULL)) {
    *RecordId = (UINT16)(Rsp[0] | ((UINT16)Rsp[1] << 8));
  }

  return Status;
}
