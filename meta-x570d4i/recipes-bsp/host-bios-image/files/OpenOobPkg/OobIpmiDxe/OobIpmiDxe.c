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

  return InstallAmiTransport ();
}
