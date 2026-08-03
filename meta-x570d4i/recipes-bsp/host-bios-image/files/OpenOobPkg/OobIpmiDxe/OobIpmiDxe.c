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

STATIC OPEN_OOB_IPMI_TRANSPORT_PROTOCOL  mTransport;
STATIC EFI_LOCK                          mLock;

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

/**
  Install AMI's DXE IPMI transport GUID as a bare presence marker.

  DxeIpmiBmcInitialize (6372357a) is the sole producer of
  4A1D0E66-5271-4E22-83FE-90921B748213, and fifteen modules DEPEX on it. Three of
  those are NOT part of the OOB stack being removed and must keep dispatching:

    DefaultOverride   (a647e4e4) — applies Setup default overrides. Never
                                   references the protocol in code at all; it
                                   uses the DEPEX purely to order itself after
                                   BMC init, so a bare marker fully satisfies it.
    PcieInfoJudgeDxe  (b603a688) — reports PCIe topology to the BMC.
    SerialMuxControl  (129f6aa7) — COM port / SOL mux switching.

  So this single install is what makes DxeIpmiBmcInitialize removable at all.

  IMPORTANT — the interface pointer is NULL, i.e. presence only. That is correct
  for DefaultOverride, which never dereferences it. The other two DO call through
  AMI's transport vtable, whose layout we have not reproduced, so they will fault
  or misbehave if they are retained AND actually reach that code path. Strip them
  alongside DxeIpmiBmcInitialize, or implement the AMI vtable before relying on
  them. This is recorded in tools/profiles/strip-oob.yaml as a hard pairing.
**/
STATIC
EFI_STATUS
InstallAmiTransportShim (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gAmiDxeIpmiTransportProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  DEBUG ((
    DEBUG_INFO,
    "OobIpmi: installed AMI transport presence marker 4A1D0E66 - %r\n",
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

  return InstallAmiTransportShim ();
}
