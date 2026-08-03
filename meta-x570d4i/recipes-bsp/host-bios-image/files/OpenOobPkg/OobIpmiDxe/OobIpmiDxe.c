/** @file
  OobIpmiDxe — serialised IPMI-over-KCS transport for the injected OOB stack.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IpmiKcsLib.h>
#include <Protocol/OobIpmiTransport.h>
#include <Protocol/AmiIpmiTransport.h>
#include <Guid/EventGroup.h>

STATIC OPEN_OOB_IPMI_TRANSPORT_PROTOCOL  mTransport;
STATIC AMI_IPMI_TRANSPORT                mAmiTransport;
STATIC EFI_LOCK                          mLock;

//
// The whole point of AMI_IPMI_TRANSPORT is binary compatibility with a struct
// we recovered from a shipping binary, so pin the offsets. If someone reorders
// the fields the build fails here rather than the board hanging at POST with
// no console — which is the only other way this mistake surfaces.
//
STATIC_ASSERT (
  OFFSET_OF (AMI_IPMI_TRANSPORT, Revision) == 0x00,
  "AMI_IPMI_TRANSPORT.Revision must be at +0x00"
  );
STATIC_ASSERT (
  OFFSET_OF (AMI_IPMI_TRANSPORT, SendIpmiCommand) == 0x10,
  "AMI_IPMI_TRANSPORT.SendIpmiCommand must be at +0x10 (SerialMuxControl "
  "calls through [This+0x10])"
  );
STATIC_ASSERT (
  OFFSET_OF (AMI_IPMI_TRANSPORT, GetBmcStatus) == 0x18,
  "AMI_IPMI_TRANSPORT.GetBmcStatus must be at +0x18"
  );
STATIC_ASSERT (
  OFFSET_OF (AMI_IPMI_TRANSPORT, SendIpmiCommandEx) == 0x20,
  "AMI_IPMI_TRANSPORT.SendIpmiCommandEx must be at +0x20"
  );

STATIC
EFI_STATUS
EFIAPI
OobIpmiExecute (
  IN     OPEN_OOB_IPMI_TRANSPORT_PROTOCOL  *This,
  IN     UINT8                             NetFn,
  IN     UINT8                             Command,
  IN     CONST VOID                        *Request      OPTIONAL,
  IN     UINTN                             RequestSize,
  OUT    UINT8                             *CompletionCode,
  OUT    VOID                              *Response     OPTIONAL,
  IN OUT UINTN                             *ResponseSize OPTIONAL
  )
{
  EFI_STATUS  Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // EfiAcquireLockOrFail rather than EfiAcquireLock: a caller at raised TPL
  // must fail fast instead of deadlocking against a holder it cannot preempt.
  //
  Status = EfiAcquireLockOrFail (&mLock);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_READY;
  }

  Status = IpmiKcsCommand (
             NetFn,
             Command,
             Request,
             RequestSize,
             CompletionCode,
             Response,
             ResponseSize
             );

  EfiReleaseLock (&mLock);
  return Status;
}

// ---------------------------------------------------------------------------
// AMI transport (4A1D0E66) — a real implementation, not a presence marker
// ---------------------------------------------------------------------------
// DxeIpmiBmcInitialize (6372357a) is the sole producer of this GUID and fifteen
// modules DEPEX on it. Three are NOT part of the OOB stack being removed and
// must keep working:
//
//   DefaultOverride   (a647e4e4) — Setup default overrides. Never references the
//                                  protocol in code; the DEPEX is pure ordering,
//                                  so even a bare marker satisfies it.
//   PcieInfoJudgeDxe  (b603a688) — reports PCIe topology to the BMC.
//   SerialMuxControl  (129f6aa7) — COM/SOL mux switching. Proven to call through
//                                  This->SendIpmiCommand at +0x10.
//
// This used to install the GUID with a NULL interface, which unblocked the
// DEPEX but would fault the moment either of the latter two dereferenced it.
// The struct layout is now recovered from the shipping binary (see the
// provenance note in Protocol/AmiIpmiTransport.h), so we publish a working
// vtable instead and the pairing constraint in strip-oob.yaml is lifted.

/**
  Shared locked path to the KCS hardware.

  AMI's response convention matches IpmiKcsCommand()'s: ResponseData carries the
  payload AFTER the completion code. A non-zero completion code is reported as
  EFI_DEVICE_ERROR because AMI's callers only test the EFI_STATUS sign bit and
  have nowhere to read a raw completion code from.
**/
STATIC
EFI_STATUS
AmiIpmiExecuteLocked (
  IN     UINT8  NetFunction,
  IN     UINT8  Command,
  IN     UINT8  *CommandData,
  IN     UINT8  CommandDataSize,
  OUT    UINT8  *ResponseData,
  IN OUT UINT8  *ResponseDataSize
  )
{
  EFI_STATUS  Status;
  UINT8       CompletionCode;
  UINTN       Size;

  Size = (ResponseDataSize != NULL) ? *ResponseDataSize : 0;

  Status = EfiAcquireLockOrFail (&mLock);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  CompletionCode = 0xFF;
  Status         = IpmiKcsCommand (
                     NetFunction,
                     Command,
                     CommandData,
                     CommandDataSize,
                     &CompletionCode,
                     ResponseData,
                     (ResponseData != NULL) ? &Size : NULL
                     );
  EfiReleaseLock (&mLock);

  if (EFI_ERROR (Status)) {
    if (ResponseDataSize != NULL) {
      *ResponseDataSize = 0;
    }

    return EFI_DEVICE_ERROR;
  }

  //
  // Size is UINTN internally but UINT8 in AMI's ABI. IPMI_KCS_MAX_PAYLOAD is
  // 255 so this cannot truncate, but clamp rather than assume.
  //
  if (ResponseDataSize != NULL) {
    *ResponseDataSize = (UINT8)MIN (Size, 0xFF);
  }

  return (CompletionCode == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EFIAPI
AmiSendIpmiCommand (
  IN     AMI_IPMI_TRANSPORT  *This,
  IN     UINT8               NetFunction,
  IN     UINT8               Lun,
  IN     UINT8               Command,
  IN     UINT8               *CommandData,
  IN     UINT8               CommandDataSize,
  OUT    UINT8               *ResponseData,
  IN OUT UINT8               *ResponseDataSize
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Only LUN 0 exists on this board's BMC. Reject anything else rather than
  // silently retargeting it.
  //
  if (Lun != 0) {
    return EFI_UNSUPPORTED;
  }

  return AmiIpmiExecuteLocked (
           NetFunction,
           Command,
           CommandData,
           CommandDataSize,
           ResponseData,
           ResponseDataSize
           );
}

STATIC
EFI_STATUS
EFIAPI
AmiSendIpmiCommandEx (
  IN     AMI_IPMI_TRANSPORT  *This,
  IN     UINT8               NetFunction,
  IN     UINT8               Lun,
  IN     UINT8               Command,
  IN     UINT8               *CommandData,
  IN     UINT8               CommandDataSize,
  OUT    UINT8               *ResponseData,
  IN OUT UINT8               *ResponseDataSize,
  IN     UINT8               InterfaceType
  )
{
  //
  // InterfaceType selects among KCS/BT/SSIF in AMI's implementation. KCS is the
  // only interface wired on this board, so the selector is ignored rather than
  // honoured — there is nothing else to dispatch to.
  //
  return AmiSendIpmiCommand (
           This,
           NetFunction,
           Lun,
           Command,
           CommandData,
           CommandDataSize,
           ResponseData,
           ResponseDataSize
           );
}

STATIC
EFI_STATUS
EFIAPI
AmiGetBmcStatus (
  IN  AMI_IPMI_TRANSPORT  *This,
  OUT AMI_BMC_STATUS      *BmcStatus,
  OUT AMI_SM_COM_ADDRESS  *ComAddress
  )
{
  if ((This == NULL) || (BmcStatus == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Report liveness from the same probe that backs OPEN_OOB's BmcResponsive,
  // so the two protocols can never disagree about whether the BMC is up.
  //
  *BmcStatus = mTransport.BmcResponsive ? AmiBmcStatusOk : AmiBmcStatusNoReply;

  if (ComAddress != NULL) {
    ZeroMem (ComAddress, sizeof (*ComAddress));
    //
    // The stock context keeps the KCS pair at +0x148/+0x14a; 0x0CA2 is the data
    // port on this board. No retained consumer reads this field.
    //
    ComAddress->Port = 0x0CA2;
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// IPMI boot overrides (NetFn 0x00 Chassis, cmd 0x09 / 0x08)
// ---------------------------------------------------------------------------
// One-time boot overrides -- force PXE, force CD/DVD, force BIOS Setup, set by
// the BMC via ipmitool or Redfish -- reach this board over KCS on the LPC bus,
// NOT over the Redfish Host Interface. KCS is live almost as soon as the CPU
// is out of reset, long before USB or any RHI driver exists, which is the only
// reason a boot override can be honoured early enough to matter.
//
// The sequence, decompiled from AMI's Bds and confirmed against live KCS
// captures on this board:
//
//   Get System Boot Options (00/09) param 0  -> Set In Progress
//   Get System Boot Options (00/09) param 5  -> Boot Flags   (the override)
//   ... Bds applies the override to the boot order ...
//   Set System Boot Options (00/08) param 4  -> Boot Info Acknowledge, data
//                                               0x01 = "BIOS/POST handled it"
//
// The acknowledge is what makes the override ONE-TIME: it tells the BMC the
// flags were consumed so the next boot is normal.
//
// *** THIS DRIVER DELIBERATELY DOES NOT ACKNOWLEDGE. ***
//
// AMI's Bds owns that handshake and still performs it -- verified on hardware,
// 20 NetFn 0x00 requests in a boot once OobIpmiDxe occupies the
// DxeIpmiBmcInitialize FFS slot (6372357A) and therefore inherits its DXE
// apriori position. If we also wrote parameter 4, we would race Bds and could
// clear Boot Flags BEFORE it reads parameter 5, silently destroying the very
// override the user asked for. Reading is safe and idempotent; acknowledging is
// not. The read below exists for visibility only.

#define IPMI_NETFN_CHASSIS_LOCAL      0x00
#define IPMI_CMD_GET_BOOT_OPTIONS     0x09
#define IPMI_CMD_SET_BOOT_OPTIONS     0x08
#define IPMI_BOOT_PARAM_SET_IN_PROG   0x00
#define IPMI_BOOT_PARAM_INFO_ACK      0x04
#define IPMI_BOOT_PARAM_BOOT_FLAGS    0x05

///
/// Boot Flags (parameter 5) data byte 1:
///   bit 7 = Boot Flags Valid   (an override is pending)
///   bit 6 = Boot Flags Persistent
///
/// The persistent bit is the whole distinction between Redfish
/// BootSourceOverrideEnabled = "Once" and = "Continuous":
///
///   bit6 = 0  one-time  -- applies to the NEXT boot only, and must be cleared
///                          once consumed or it would repeat forever.
///   bit6 = 1  persistent -- applies to EVERY boot until the BMC revokes it.
///                          Clearing it here would silently downgrade a
///                          Continuous override into a single-shot one.
///
/// Data byte 2 bits [5:2] select the device.
///
#define IPMI_BOOT_FLAGS_VALID         0x80
#define IPMI_BOOT_FLAGS_PERSISTENT    0x40
#define IPMI_BOOT_DEVICE_MASK         0x3C
#define IPMI_BOOT_DEVICE_SHIFT        2

/**
  Read the BMC's Boot Flags and, if an override is pending, acknowledge it.

  ACKNOWLEDGE (parameter 4) ONLY -- NEVER CLEAR (parameter 5).

  That distinction is the whole point. Parameter 4 "Boot Info Acknowledge" is
  informational: it tells the BMC the host has seen the boot info. It does not
  touch the override itself, so it is safe to send early and safe to send more
  than once (Bds sends its own). Verified against the BMC implementation, whose
  parameter-4 path only accumulates the ack bits and logs them -- it never
  reaches the boot source/mode/one-time objects.

  Writing parameter 5 back is the destructive operation. AMI's Bds does that
  unconditionally with every bit zero once it has consumed the flags, which
  reads as a revocation and silently downgrades a Continuous override to a
  single-shot one. That is fixed on the BMC side (see the
  0001-chassis-keep-persistent-boot-override-on-BIOS-clear patch); this driver
  must not add a second source of the same damage.

  Failures are ignored throughout: a BMC that does not answer simply means no
  override, and none of this may prevent the transport being published.
**/
STATIC
VOID
OobIpmiLogBootFlags (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Request[3];
  UINT8       Response[8];
  UINT8       CompletionCode;
  UINTN       RespSize;

  Request[0] = IPMI_BOOT_PARAM_BOOT_FLAGS;
  Request[1] = 0; // set selector
  Request[2] = 0; // block selector

  RespSize       = sizeof (Response);
  CompletionCode = 0xFF;
  ZeroMem (Response, sizeof (Response));

  Status = IpmiKcsCommand (
             IPMI_NETFN_CHASSIS_LOCAL,
             IPMI_CMD_GET_BOOT_OPTIONS,
             Request,
             sizeof (Request),
             &CompletionCode,
             Response,
             &RespSize
           );
  if (EFI_ERROR (Status) || (CompletionCode != 0) || (RespSize < 3)) {
    DEBUG ((
      DEBUG_INFO,
      "OobIpmi: boot flags unavailable (%r, cc=0x%02x, len=%u)\n",
      Status,
      CompletionCode,
      (UINT32)RespSize
      ));
    return;
  }

  //
  // Response[0] is the parameter revision; the parameter data starts at [1],
  // so Boot Flags data byte 1 is Response[1] and data byte 2 is Response[2].
  //
  if ((Response[1] & IPMI_BOOT_FLAGS_VALID) == 0) {
    DEBUG ((DEBUG_INFO, "OobIpmi: no BMC boot override pending\n"));
    return;
  }

  DEBUG ((
    DEBUG_WARN,
    "OobIpmi: BMC boot override ACTIVE, device selector 0x%02x, %a\n",
    (Response[2] & IPMI_BOOT_DEVICE_MASK) >> IPMI_BOOT_DEVICE_SHIFT,
    (Response[1] & IPMI_BOOT_FLAGS_PERSISTENT) ? "PERSISTENT" : "one-time"
    ));

  //
  // Acknowledge: Set System Boot Options, parameter 4, ack data 0x01
  // ("BIOS/POST has handled the boot info"). Frame matches what Bds emits,
  // [04 01 00] -- parameter selector, write mask, acknowledge data.
  //
  // Note this is sent for a PERSISTENT override too, and that is correct: an
  // acknowledge reports that the host saw the flags, it does not consume them.
  // Only a parameter-5 write revokes, and this driver never issues one.
  //
  Request[0]     = IPMI_BOOT_PARAM_INFO_ACK;
  Request[1]     = 0x01; // write mask
  Request[2]     = 0x00; // acknowledge data
  CompletionCode = 0xFF;

  Status = IpmiKcsCommand (
             IPMI_NETFN_CHASSIS_LOCAL,
             IPMI_CMD_SET_BOOT_OPTIONS,
             Request,
             sizeof (Request),
             &CompletionCode,
             NULL,
             NULL
           );
  DEBUG ((
    DEBUG_INFO,
    "OobIpmi: boot info acknowledged - %r, cc=0x%02x\n",
    Status,
    CompletionCode
    ));
}

/**
  Clear a CONSUMED one-time boot override, and only a one-time one.

  Runs at ReadyToBoot, deliberately -- not at driver entry. This driver occupies
  the DxeIpmiBmcInitialize FFS slot (6372357A) and so inherits its DXE apriori
  position, which is far earlier than Bds. Clearing Boot Flags there would wipe
  the override BEFORE Bds reads parameter 5, destroying the very request the
  user made. By ReadyToBoot, Bds has already had its turn.

  In the normal case this therefore finds nothing to do: Bds performs the
  parameter-4 acknowledge and zeroes parameter 5 itself (observed on hardware --
  Set [04 01 00] then Set [05 00 00 00 00 00] for a Redfish "Once" override).
  This exists for the case where that path does not run, which is not
  hypothetical: before the FFS slot was displaced, Bds issued no NetFn 0x00
  traffic at all and a one-time override would have persisted indefinitely.

  The persistent bit is the guard. A Continuous override must survive every
  boot, so it is left strictly alone.
**/
STATIC
VOID
EFIAPI
OobIpmiClearOneTimeBootFlags (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  UINT8       Request[6];
  UINT8       Response[8];
  UINT8       CompletionCode;
  UINTN       RespSize;

  Request[0] = IPMI_BOOT_PARAM_BOOT_FLAGS;
  Request[1] = 0;
  Request[2] = 0;

  RespSize       = sizeof (Response);
  CompletionCode = 0xFF;
  ZeroMem (Response, sizeof (Response));

  Status = IpmiKcsCommand (
             IPMI_NETFN_CHASSIS_LOCAL,
             IPMI_CMD_GET_BOOT_OPTIONS,
             Request,
             3,
             &CompletionCode,
             Response,
             &RespSize
           );
  if (EFI_ERROR (Status) || (CompletionCode != 0) || (RespSize < 3)) {
    return;
  }

  if ((Response[1] & IPMI_BOOT_FLAGS_VALID) == 0) {
    // Already consumed and cleared, almost certainly by Bds. Nothing to do.
    return;
  }

  if ((Response[1] & IPMI_BOOT_FLAGS_PERSISTENT) != 0) {
    //
    // Continuous override: the BMC means it to apply to every boot. Leaving it
    // is the entire point of checking this bit.
    //
    DEBUG ((
      DEBUG_INFO,
      "OobIpmi: boot override is PERSISTENT - left in place\n"
      ));
    return;
  }

  DEBUG ((
    DEBUG_WARN,
    "OobIpmi: one-time boot override still valid at ReadyToBoot - "
    "acknowledging and clearing\n"
    ));

  //
  // Acknowledge first (parameter 4, "BIOS/POST handled the boot info"), then
  // invalidate parameter 5. Same order Bds uses.
  //
  Request[0]     = IPMI_BOOT_PARAM_INFO_ACK;
  Request[1]     = 0x01; // BIOS POST ack
  Request[2]     = 0x00; // write mask
  RespSize       = 0;
  CompletionCode = 0xFF;
  IpmiKcsCommand (
    IPMI_NETFN_CHASSIS_LOCAL,
    IPMI_CMD_SET_BOOT_OPTIONS,
    Request,
    3,
    &CompletionCode,
    NULL,
    NULL
  );

  ZeroMem (Request, sizeof (Request));
  Request[0]     = IPMI_BOOT_PARAM_BOOT_FLAGS; // rest zero => valid bit cleared
  RespSize       = 0;
  CompletionCode = 0xFF;
  IpmiKcsCommand (
    IPMI_NETFN_CHASSIS_LOCAL,
    IPMI_CMD_SET_BOOT_OPTIONS,
    Request,
    sizeof (Request),
    &CompletionCode,
    NULL,
    NULL
  );
}

/**
  Publish the AMI-compatible transport.
**/
STATIC
EFI_STATUS
InstallAmiTransport (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  ZeroMem (&mAmiTransport, sizeof (mAmiTransport));
  //
  // Revision and Reserved stay zero: the stock context is AllocateZeroPool'd
  // and the publisher never writes either field, so zero is what every existing
  // consumer was compiled against.
  //
  mAmiTransport.SendIpmiCommand   = AmiSendIpmiCommand;
  mAmiTransport.GetBmcStatus      = AmiGetBmcStatus;
  mAmiTransport.SendIpmiCommandEx = AmiSendIpmiCommandEx;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gAmiDxeIpmiTransportProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mAmiTransport
                  );
  DEBUG ((
    DEBUG_INFO,
    "OobIpmi: installed AMI transport 4A1D0E66 (functional vtable) - %r\n",
    Status
    ));
  return Status;
}

/**
  Entry point. Publishes the transport protocol unconditionally, even when the
  BMC does not answer, and records liveness in BmcResponsive.

  Publishing regardless is deliberate: on this board the BMC is frequently still
  booting during DXE, and a driver that refused to install would permanently
  deny the protocol to consumers that run much later (ReadyToBoot), by which
  time the BMC is invariably up.
**/
EFI_STATUS
EFIAPI
OobIpmiEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       DeviceIdSize;

  EfiInitializeLock (&mLock, TPL_NOTIFY);

  ZeroMem (&mTransport, sizeof (mTransport));
  mTransport.Revision = OPEN_OOB_IPMI_TRANSPORT_REVISION;
  mTransport.Execute  = OobIpmiExecute;

  DeviceIdSize = sizeof (mTransport.DeviceId);
  Status       = IpmiKcsProbe (mTransport.DeviceId, &DeviceIdSize);
  if (!EFI_ERROR (Status)) {
    mTransport.BmcResponsive = TRUE;
    mTransport.DeviceIdSize  = (UINT8)DeviceIdSize;
    DEBUG ((
      DEBUG_INFO,
      "OobIpmi: BMC responsive, device ID rev %02x fw %02x.%02x\n",
      mTransport.DeviceId[1],
      mTransport.DeviceId[2],
      mTransport.DeviceId[3]
      ));
  } else {
    DEBUG ((DEBUG_WARN, "OobIpmi: BMC did not answer Get Device ID - %r\n", Status));
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gOpenOobIpmiTransportProtocolGuid,
                  &mTransport,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = InstallAmiTransport ();

  //
  // Visibility only, and only once the BMC has proven responsive -- there is no
  // point issuing a Chassis command to something that did not answer Get Device
  // ID. Read-only: Bds performs the parameter-4 acknowledge, and doing it here
  // too would risk clearing Boot Flags before Bds reads them.
  //
  if (mTransport.BmcResponsive) {
    EFI_EVENT  ReadyToBoot;

    OobIpmiLogBootFlags ();

    //
    // Late cleanup for a one-time override Bds did not consume. Best effort: if
    // the event never fires we simply do not clear, which is exactly today's
    // behaviour, so this can only ever add correctness.
    //
    ReadyToBoot = NULL;
    gBS->CreateEventEx (
           EVT_NOTIFY_SIGNAL,
           TPL_CALLBACK,
           OobIpmiClearOneTimeBootFlags,
           NULL,
           &gEfiEventReadyToBootGuid,
           &ReadyToBoot
           );
  }

  return Status;
}
