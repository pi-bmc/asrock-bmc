/** @file
  Host side of the openbmc/intel-ipmi-oem command set.

  intel-ipmi-oem is a BMC-side ipmid provider: every command it registers is one
  the *host firmware* is expected to issue. This library is the missing other
  half — the BIOS-side caller for each command that is genuinely host-initiated,
  with request and response layouts transcribed from the handler signatures in
  that repository so the two sides agree byte for byte.

  ---------------------------------------------------------------------------
  Scope: which of intel-ipmi-oem's 99 commands appear here
  ---------------------------------------------------------------------------
  Most of those 99 exist to serve a *remote* client (Redfish, ipmitool over
  LAN): Get Chassis Status, the SEL and FRU readers, fan and thermal control,
  the firmware-update state machine. Those have no host side — the BIOS never
  sends them — so implementing a "reversed" version would be inventing traffic,
  not mirroring a protocol.

  What is here is the set the BIOS actually originates:

    NetFn 0x30 (intel::netFnGeneral)
      0x01  GetBmcVersionString        0x9A  GetProcessorErrConfig
      0x02  RestoreConfiguration       0x9B  SetProcessorErrConfig
      0x27  GetOemDeviceInfo           0xB3  GetSecurityMode
      0x44  SendEmbeddedFwUpdStatus    0xB4  SetSecurityMode
      0x57  SetFaultIndication         0xD7  SetBiosPwdHashInfo
      0x66  GetBufferSize              0xD8  GetBiosPwdHash
      0x8E  SetDimmOffset              0xE5  GetNmiSource
      0x8F  GetDimmOffset              0xEA  SetEfiBootOptions
      0x93  ReadBaseBoardProductId     0xEB  GetEfiBootOptions
                                       0xED  SetNmiSource

    NetFn 0x04 (Sensor)   0x02  Platform Event
    NetFn 0x0A (Storage)  0x44  Add SEL Entry

  0xD3-0xD6 (Set/Get BIOS Capabilities, Set/Get Payload) are the other four
  host-initiated NetFn 0x30 commands. They are not here because BiosCfgOobDxe
  already implements them, including the chunked SetPayload state machine; this
  library covers what that driver does not.

  ---------------------------------------------------------------------------
  Deliberately absent: MDR II (NetFn 0x3E, cmds 0x30-0x3D)
  ---------------------------------------------------------------------------
  intel-ipmi-oem's twelve MDR II commands look like the natural host-side SMBIOS
  push, and they are not implemented here on purpose.

  MDR II does not carry data over IPMI. Look at the handler signature:

      mdr2SendDataBlock(uint16_t agentId, uint16_t lockHandle,
                        uint32_t xferOffset, uint32_t xferLength,
                        uint32_t checksum)

  There is no payload parameter. The bytes travel through a shared memory
  aperture: the BMC opens /dev/vgasharedmem and mmaps it at mdr2SMBaseAddress
  (0x9FF00000), and the IPMI commands only describe offsets into that window.

  This board has no such aperture. /dev/vgasharedmem does not exist on the BMC,
  the aspeed VGA shared-memory driver is not built, and there is no device-tree
  node for it. Worse, the only MDR V2 code we ship is openbmc/smbios-mdr, which
  registers no IPMI handlers at all — it provides the D-Bus service plus the
  smbios-ipmi-blobs handler and nothing else. So a host-side MDR II
  implementation would have no responder to talk to even if the memory window
  existed.

  SmbiosBmcPushDxe therefore uses phosphor-ipmi-blobs (NetFn 0x2E cmd 0x80,
  blob "/smbios"), which carries the table inline over KCS and needs no shared
  memory. That is the correct transport for this hardware, not a workaround.

  ---------------------------------------------------------------------------
  Error convention
  ---------------------------------------------------------------------------
  Every function distinguishes a transport failure from a BMC refusal, because
  the two need different handling: the first means retry or give up on the BMC,
  the second means the BMC is alive and answered "no".

    return value   EFI_SUCCESS  the transaction completed; the BMC replied.
                   other        KCS-level failure; *CompletionCode untouched.
    CompletionCode the IPMI completion code from that reply. 0 is success.

  Pass NULL for CompletionCode when a non-zero code is not interesting to
  distinguish; the function then folds it into EFI_PROTOCOL_ERROR.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPEN_OOB_INTEL_OEM_LIB_H_
#define OPEN_OOB_INTEL_OEM_LIB_H_

#include <Uefi/UefiBaseType.h>

///
/// intel::netFnGeneral. netFnOemOne in ipmid's api-types.hpp.
///
#define OOB_INTEL_NETFN_GENERAL   0x30
///
/// intel::netFnPlatform (netFnOemTwo) and intel::netFnApp (netFnOemEight).
/// Declared for completeness; no command below uses them.
///
#define OOB_INTEL_NETFN_PLATFORM  0x32
#define OOB_INTEL_NETFN_APP       0x3E

///
/// Command bytes, from include/oemcommands.hpp.
///
#define OOB_INTEL_CMD_GET_BMC_VERSION_STRING  0x01
#define OOB_INTEL_CMD_RESTORE_CONFIGURATION   0x02
#define OOB_INTEL_CMD_GET_OEM_DEVICE_INFO     0x27
#define OOB_INTEL_CMD_SEND_FW_UPD_STATUS      0x44
#define OOB_INTEL_CMD_SET_FAULT_INDICATION    0x57
#define OOB_INTEL_CMD_GET_BUFFER_SIZE         0x66
#define OOB_INTEL_CMD_SET_DIMM_OFFSET         0x8E
#define OOB_INTEL_CMD_GET_DIMM_OFFSET         0x8F
#define OOB_INTEL_CMD_READ_BOARD_PRODUCT_ID   0x93
#define OOB_INTEL_CMD_GET_PROC_ERR_CONFIG     0x9A
#define OOB_INTEL_CMD_SET_PROC_ERR_CONFIG     0x9B
#define OOB_INTEL_CMD_GET_SECURITY_MODE       0xB3
#define OOB_INTEL_CMD_SET_SECURITY_MODE       0xB4
#define OOB_INTEL_CMD_SET_BIOS_PWD_HASH_INFO  0xD7
#define OOB_INTEL_CMD_GET_BIOS_PWD_HASH       0xD8
#define OOB_INTEL_CMD_GET_NMI_SOURCE          0xE5
#define OOB_INTEL_CMD_SET_EFI_BOOT_OPTIONS    0xEA
#define OOB_INTEL_CMD_GET_EFI_BOOT_OPTIONS    0xEB
#define OOB_INTEL_CMD_SET_NMI_SOURCE          0xED

///
/// Standard-NetFn commands intel-ipmi-oem also answers.
///
#define OOB_IPMI_CMD_PLATFORM_EVENT  0x02
#define OOB_IPMI_CMD_ADD_SEL_ENTRY   0x44

///
/// OEMDevEntityType, oemcommands.hpp:252. Selects what Get OEM Device Info
/// returns.
///
#define OOB_INTEL_DEV_ENTITY_BIOS_ID  0x00
#define OOB_INTEL_DEV_ENTITY_DEV_VER  0x01
#define OOB_INTEL_DEV_ENTITY_SDR_VER  0x02

///
/// dimmOffsetTypes, oemcommands.hpp:374.
///
#define OOB_INTEL_DIMM_OFFSET_STATIC_CLTT  0x00
#define OOB_INTEL_DIMM_OFFSET_DIMM_POWER   0x02

///
/// FWUpdateTarget, oemcommands.hpp:259.
///
#define OOB_INTEL_FWUPD_TARGET_BMC      0x00
#define OOB_INTEL_FWUPD_TARGET_BIOS     0x01
#define OOB_INTEL_FWUPD_TARGET_ME       0x02
#define OOB_INTEL_FWUPD_TARGET_OEM_EWS  0x04

///
/// RestoreConfiguration sub-commands, oemcommands.cpp:3120. The request is
/// preceded by the literal ASCII "CLR" guard, which this library supplies.
///
#define OOB_INTEL_RESTORE_STATUS        0x00
#define OOB_INTEL_RESTORE_DEFAULT       0xAA
#define OOB_INTEL_RESTORE_FULL          0xBB
#define OOB_INTEL_RESTORE_FORMAT        0xCC

///
/// BootOptionParameter for Set/Get EFI Boot Options, oemcommands.cpp:3467.
/// These mirror the IPMI chassis boot-option parameter numbers, but the command
/// is Intel's own: its only expressible boot source is HTTP boot. Ordinary
/// boot-source overrides go through the standard chassis command (see
/// OobIpmiDxe), not through this.
///
#define OOB_INTEL_BOOT_PARAM_SET_IN_PROGRESS  0x00
#define OOB_INTEL_BOOT_PARAM_BOOT_FLAGS       0x05

/// Boot-flag bits accepted by Set EFI Boot Options, oemcommands.cpp:3476.
#define OOB_INTEL_BOOT_FLAGS_VALID_ONE_TIME   0x80
#define OOB_INTEL_BOOT_FLAGS_PERMANENT        0x40
#define OOB_INTEL_BOOT_FLAGS_VALID_PERMANENT  0xC0
/// The HTTP-boot device selector this command exists to carry.
#define OOB_INTEL_BOOT_SOURCE_HTTP            0x0D

/// Set-in-progress states, oemcommands.cpp:3473.
#define OOB_INTEL_SET_COMPLETE     0x00
#define OOB_INTEL_SET_IN_PROGRESS  0x01

///
/// Sizes from biosconfigcommands.hpp:27.
///
#define OOB_INTEL_PWD_SEED_SIZE  32
#define OOB_INTEL_PWD_HASH_SIZE  64

///
/// IPMI software IDs for Platform Event. Over the system interface
/// intel-ipmi-oem requires bit 0 set (sensorcommands.cpp: isSoftwareID) and
/// rejects the request otherwise, so a generator ID of 0 will not do.
///
#define OOB_IPMI_SOFTWARE_ID_BIOS_POST  0x01

#pragma pack(1)

///
/// Response to Get DIMM Offset for one DIMM.
///
typedef struct {
  UINT8    Index;
  UINT8    Value;
} OOB_INTEL_DIMM_OFFSET_ENTRY;

///
/// Get Processor Error Config response, from the 14-field RspType in
/// oemcommands.cpp:938. ipmid packs sub-byte fields LSB-first and coalesces
/// them, so those fourteen fields are seven bytes on the wire: the three
/// reset-enable bools plus a 5-bit reserved share byte 0, and each per-CPU byte
/// carries a 6-bit IERR count with a 2-bit status above it.
///
typedef struct {
  UINT8    ResetFlags;      ///< bit0 IERR, bit1 ERR2, bit2 MCERR, bits3-7 rsvd
  UINT8    Reserved;        ///< the BMC returns 0x3F here
  UINT8    Cpu1IerrStatus;  ///< bits0-5 count, bits6-7 status
  UINT8    Cpu2IerrStatus;
  UINT8    Cpu3IerrStatus;
  UINT8    Cpu4IerrStatus;
  UINT8    CrashdumpCount;
} OOB_INTEL_PROC_ERR_CONFIG;

/// Bit positions within OOB_INTEL_PROC_ERR_CONFIG.ResetFlags, and within the
/// optional third request byte of Set Processor Error Config.
#define OOB_INTEL_PROC_ERR_RESET_ON_IERR       BIT0
#define OOB_INTEL_PROC_ERR_RESET_ON_ERR2       BIT1
#define OOB_INTEL_PROC_ERR_RESET_ON_MCERR      BIT2
#define OOB_INTEL_PROC_ERR_CLEAR_CPU_COUNT     BIT0
#define OOB_INTEL_PROC_ERR_CLEAR_CRASHDUMP     BIT1

/// Extract the count and status halves of a per-CPU IERR byte.
#define OOB_INTEL_PROC_IERR_COUNT(b)   ((UINT8)((b) & 0x3F))
#define OOB_INTEL_PROC_IERR_STATUS(b)  ((UINT8)(((b) >> 6) & 0x03))

///
/// A 16-byte IPMI SEL record, IPMI v2.0 Table 32. Add SEL Entry takes exactly
/// this. Field order matches ipmiStorageAddSELEntry's parameter list.
///
typedef struct {
  UINT16    RecordId;
  UINT8     RecordType;
  UINT32    Timestamp;
  UINT16    GeneratorId;
  UINT8     EvmRev;
  UINT8     SensorType;
  UINT8     SensorNum;
  UINT8     EventType;
  UINT8     EventData1;
  UINT8     EventData2;
  UINT8     EventData3;
} OOB_IPMI_SEL_RECORD;

#pragma pack()

/* ------------------------------------------------------------------------- */
/* NetFn 0x30 — intel::netFnGeneral                                          */
/* ------------------------------------------------------------------------- */

/**
  Get BMC Version String (0x01).

  The BMC answers with VERSION_ID from its /etc/os-release. Useful to record in
  the host's own log so a BIOS-side capture identifies which BMC build it ran
  against.

  @param[out]     Version         Receives a NUL-terminated ASCII string.
  @param[in,out]  VersionSize     On entry the capacity of Version including the
                                  terminator; on exit the string length without
                                  it.
  @param[out]     CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetBmcVersionString (
  OUT    CHAR8  *Version,
  IN OUT UINTN  *VersionSize,
  OUT    UINT8  *CompletionCode  OPTIONAL
  );

/**
  Restore Configuration (0x02).

  Guarded by a literal "CLR" in the request, which this function supplies, so a
  stray command cannot wipe the BMC's configuration.

  @param[in]  SubCommand      One of OOB_INTEL_RESTORE_*.
  @param[out] RestoreStatus   Optional; the BMC's progress byte.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelRestoreConfiguration (
  IN  UINT8  SubCommand,
  OUT UINT8  *RestoreStatus  OPTIONAL,
  OUT UINT8  *CompletionCode OPTIONAL
  );

/**
  Get OEM Device Info (0x27).

  @param[in]      EntityType      One of OOB_INTEL_DEV_ENTITY_*. Only
                                  ..._BIOS_ID takes Count/Offset; for the other
                                  two the BMC rejects a request that carries
                                  them, so pass Count = 0.
  @param[in]      Count           Bytes to read (BIOS ID only).
  @param[in]      Offset          Offset to read from (BIOS ID only).
  @param[out]     Data            Receives the reply body.
  @param[in,out]  DataSize        Capacity on entry, bytes written on exit.
  @param[out]     CompletionCode  Optional; see the file header.
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
  );

/**
  Send Embedded Firmware Update Status (0x44).

  Lets the host report progress of a firmware update it is driving, so the BMC
  can surface it rather than appearing to hang.

  @param[in]  Status          Update status code.
  @param[in]  Target          One of OOB_INTEL_FWUPD_TARGET_*.
  @param[in]  MajorRevision   Major version of the image being applied.
  @param[in]  MinorRevision   Minor version.
  @param[in]  AuxInfo         Target-defined auxiliary information.
  @param[out] CompletionCode  Optional; see the file header.
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
  );

/**
  Set Fault Indication (0x57).

  @param[in]  SourceId        Who is reporting (BIOS, ME, ...).
  @param[in]  FaultType       Fault classification.
  @param[in]  FaultState      Asserted or cleared.
  @param[in]  FaultGroup      Group within the type.
  @param[in]  LedStateData    Exactly 8 bytes of per-LED state.
  @param[out] CompletionCode  Optional; see the file header.
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
  );

/**
  Get Buffer Size (0x66).

  Both values are returned in units of four bytes; this function multiplies them
  out so callers work in bytes. Worth calling before a long chunked transfer:
  BiosCfgOobDxe's XFER_CHUNK is a compile-time guess at the same number, and the
  BMC's answer is authoritative for its half of the KCS path.

  Note the upstream handler hardcodes 63/4 for KCS, with the honest comment that
  the host driver's limit is unknowable from the BMC side. Treat the result as
  an upper bound to clamp against, not a target to grow into.

  @param[out] KcsMaxBytes     Optional; maximum KCS transfer, in bytes.
  @param[out] IpmbMaxBytes    Optional; maximum IPMB transfer, in bytes.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetBufferSize (
  OUT UINT16  *KcsMaxBytes,
  OUT UINT16  *IpmbMaxBytes,
  OUT UINT8   *CompletionCode OPTIONAL
  );

/**
  Set DIMM Offset (0x8E).

  @param[in]  Type            OOB_INTEL_DIMM_OFFSET_STATIC_CLTT or ..._DIMM_POWER.
  @param[in]  Entries         Index/value pairs. Must not be empty; the BMC
                              rejects a zero-length list.
  @param[in]  EntryCount      Number of pairs.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetDimmOffset (
  IN  UINT8                              Type,
  IN  CONST OOB_INTEL_DIMM_OFFSET_ENTRY  *Entries,
  IN  UINTN                              EntryCount,
  OUT UINT8                              *CompletionCode OPTIONAL
  );

/**
  Get DIMM Offset (0x8F).

  @param[in]  Type            OOB_INTEL_DIMM_OFFSET_*.
  @param[in]  Index           DIMM index.
  @param[out] Value           Receives the stored offset.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetDimmOffset (
  IN  UINT8  Type,
  IN  UINT8  Index,
  OUT UINT8  *Value,
  OUT UINT8  *CompletionCode OPTIONAL
  );

/**
  Read Base Board Product ID (0x93).

  @param[out] ProductId       Receives the board product ID byte.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelReadBaseBoardProductId (
  OUT UINT8  *ProductId,
  OUT UINT8  *CompletionCode OPTIONAL
  );

/**
  Get Processor Error Config (0x9A).

  @param[out] Config          Receives the packed configuration.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetProcessorErrConfig (
  OUT OOB_INTEL_PROC_ERR_CONFIG  *Config,
  OUT UINT8                      *CompletionCode OPTIONAL
  );

/**
  Set Processor Error Config (0x9B).

  @param[in]  ResetOnIerr     Reset the host on IERR.
  @param[in]  ResetOnErr2     Reset the host on ERR2.
  @param[in]  ResetOnMcerr    Reset the host on MCERR.
  @param[in]  ClearCounts     Optional trailing byte: bit0 clears the CPU error
                              count, bit1 clears the crashdump count. Pass NULL
                              to omit it; the upstream handler takes these as
                              std::optional and accepts the shorter request.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetProcessorErrConfig (
  IN  BOOLEAN  ResetOnIerr,
  IN  BOOLEAN  ResetOnErr2,
  IN  BOOLEAN  ResetOnMcerr,
  IN  CONST UINT8  *ClearCounts OPTIONAL,
  OUT UINT8    *CompletionCode OPTIONAL
  );

/**
  Get Security Mode (0xB3).

  @param[out] RestrictionMode Optional; the KCS restriction mode.
  @param[out] SpecialMode     Optional; the special (manufacturing) mode.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetSecurityMode (
  OUT UINT8  *RestrictionMode OPTIONAL,
  OUT UINT8  *SpecialMode     OPTIONAL,
  OUT UINT8  *CompletionCode  OPTIONAL
  );

/**
  Set Security Mode (0xB4).

  @param[in]  RestrictionMode The KCS restriction mode to apply.
  @param[in]  SpecialMode     Optional trailing byte; pass NULL to omit.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetSecurityMode (
  IN  UINT8        RestrictionMode,
  IN  CONST UINT8  *SpecialMode OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  );

/**
  Set BIOS Password Hash Info (0xD7).

  Hands the BMC the seed and admin-password hash so a Redfish client can change
  BIOS passwords out of band. The request is fixed length: 32-byte seed, one
  algorithm byte, 64-byte hash.

  This is the half of the BIOS-config command set that BiosCfgOobDxe does not
  cover. Only call it once the firmware genuinely holds a hash — sending zeros
  publishes a valid-looking credential the BMC will offer to Redfish.

  @param[in]  PasswordSeed    OOB_INTEL_PWD_SEED_SIZE bytes.
  @param[in]  AlgorithmInfo   Hash algorithm identifier.
  @param[in]  AdminPwdHash    OOB_INTEL_PWD_HASH_SIZE bytes.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetBiosPwdHashInfo (
  IN  CONST UINT8  PasswordSeed[OOB_INTEL_PWD_SEED_SIZE],
  IN  UINT8        AlgorithmInfo,
  IN  CONST UINT8  AdminPwdHash[OOB_INTEL_PWD_HASH_SIZE],
  OUT UINT8        *CompletionCode OPTIONAL
  );

/**
  Get BIOS Password Hash (0xD8).

  The upstream handler refuses this once POST has completed, so it is only
  answerable from within the DXE phase — which is exactly where an injected
  driver runs.

  @param[out]     Data            Receives the reply body.
  @param[in,out]  DataSize        Capacity on entry, bytes written on exit.
  @param[out]     CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetBiosPwdHash (
  OUT    VOID   *Data,
  IN OUT UINTN  *DataSize,
  OUT    UINT8  *CompletionCode OPTIONAL
  );

/**
  Get NMI Source (0xE5).

  @param[out] Source          Receives the BMC's recorded NMI source.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelGetNmiSource (
  OUT UINT8  *Source,
  OUT UINT8  *CompletionCode OPTIONAL
  );

/**
  Set NMI Source (0xED).

  @param[in]  Source          NMI source identifier.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetNmiSource (
  IN  UINT8  Source,
  OUT UINT8  *CompletionCode OPTIONAL
  );

/**
  Set EFI Boot Options (0xEA).

  Two shapes, matching ipmiOemSetEfiBootOptions:

    Parameter = OOB_INTEL_BOOT_PARAM_SET_IN_PROGRESS
        Data = OOB_INTEL_SET_IN_PROGRESS or ..._SET_COMPLETE, BootOption NULL.
    Parameter = OOB_INTEL_BOOT_PARAM_BOOT_FLAGS
        Data = the boot-flags byte (OOB_INTEL_BOOT_FLAGS_*), BootOption
        required.

  Any other parameter is rejected with "parameter not supported", and both
  bytes zero is rejected outright.

  @param[in]  Parameter       OOB_INTEL_BOOT_PARAM_*.
  @param[in]  Data            Parameter-dependent, see above.
  @param[in]  BootOption      Optional trailing byte; required for boot flags.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIntelSetEfiBootOptions (
  IN  UINT8        Parameter,
  IN  UINT8        Data,
  IN  CONST UINT8  *BootOption OPTIONAL,
  OUT UINT8        *CompletionCode OPTIONAL
  );

/**
  Get EFI Boot Options (0xEB).

  @param[in]      Parameter       OOB_INTEL_BOOT_PARAM_*.
  @param[in]      Set             Set selector; ignored by the BMC but part of
                                  the request.
  @param[in]      Block           Block selector; likewise.
  @param[out]     Data            Receives [version][param][data...].
  @param[in,out]  DataSize        Capacity on entry, bytes written on exit.
  @param[out]     CompletionCode  Optional; see the file header.
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
  );

/* ------------------------------------------------------------------------- */
/* Standard NetFns that intel-ipmi-oem also answers                          */
/* ------------------------------------------------------------------------- */

/**
  Platform Event, a.k.a. Event Message (Sensor NetFn 0x04, cmd 0x02).

  The system-interface form of this command puts a software ID first, and
  intel-ipmi-oem rejects the request unless bit 0 of that byte is set — it uses
  the bit to distinguish a software ID from a slave address before folding it
  into the SEL generator ID. Pass OOB_IPMI_SOFTWARE_ID_BIOS_POST unless there is
  a reason not to.

  @param[in]  SoftwareId      Generator software ID; bit 0 must be set.
  @param[in]  EvmRev          Event message format revision (0x04 for IPMI 2.0).
  @param[in]  SensorType      IPMI sensor type.
  @param[in]  SensorNumber    Sensor number.
  @param[in]  EventDirType    Event direction in bit 7, event/reading type in
                              bits 0-6.
  @param[in]  EventData1      First event data byte.
  @param[in]  EventData2      Optional; pass NULL to send a shorter message.
  @param[in]  EventData3      Optional; ignored unless EventData2 is present.
  @param[out] CompletionCode  Optional; see the file header.
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
  );

/**
  Add SEL Entry (Storage NetFn 0x0A, cmd 0x44).

  Prefer OobIpmiPlatformEvent() for ordinary sensor events: it lets the BMC
  build the generator ID and timestamp itself. Use this when the host needs to
  place a fully-formed record, for instance replaying an event it logged before
  the BMC was responsive.

  @param[in]  Record          The 16-byte SEL record to add.
  @param[out] RecordId        Optional; the record ID the BMC assigned.
  @param[out] CompletionCode  Optional; see the file header.
**/
EFI_STATUS
EFIAPI
OobIpmiAddSelEntry (
  IN  CONST OOB_IPMI_SEL_RECORD  *Record,
  OUT UINT16                     *RecordId OPTIONAL,
  OUT UINT8                      *CompletionCode OPTIONAL
  );

#endif // OPEN_OOB_INTEL_OEM_LIB_H_
