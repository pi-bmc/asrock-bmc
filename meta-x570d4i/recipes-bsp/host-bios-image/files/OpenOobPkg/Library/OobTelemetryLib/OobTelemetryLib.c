/** @file
  OobTelemetryLib implementation.

  Two constraints shape this. First, every write lands in AMI's NVAR store in
  SPI flash, and AMI appends rather than rewrites in place — so writes are a
  budgeted resource, not free. Second, the interesting failure modes are ones
  where the driver dies partway, so the record has to be useful even if the last
  write never happens. Hence: write on state change, cap the total, and never
  block or fail a caller because telemetry could not be persisted.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/OobTelemetryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

//
// Hard ceiling on flash writes. Each one appends an NVAR entry; a runaway
// driver must not be able to fill the store. Twelve is comfortably more than
// the number of distinct milestones any of our drivers can hit.
//
#define OOB_TLM_MAX_WRITES  12

STATIC OOB_TELEMETRY_RECORD  mRecord;
STATIC CONST CHAR16          *mName    = NULL;
STATIC BOOLEAN               mInited   = FALSE;
STATIC UINT32                mLastSaved = 0xFFFFFFFFu;

STATIC
VOID
Persist (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!mInited || (mName == NULL)) {
    return;
  }

  if (mRecord.Writes >= OOB_TLM_MAX_WRITES) {
    return;
  }

  if ((gRT == NULL) || (gRT->SetVariable == NULL)) {
    return;
  }

  //
  // Count the write before issuing it. If SetVariable half-succeeds or the
  // machine dies inside it, the budget must still have been spent — otherwise a
  // failing write path could loop forever burning flash.
  //
  mRecord.Writes++;

  Status = gRT->SetVariable (
                  (CHAR16 *)mName,
                  &mRecord.CallerId,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (mRecord),
                  &mRecord
                  );

  if (!EFI_ERROR (Status)) {
    mLastSaved = mRecord.Flags;
  }
}

VOID
EFIAPI
OobTelemetryInit (
  IN CONST CHAR16  *VariableName
  )
{
  if (mInited) {
    return;
  }

  ZeroMem (&mRecord, sizeof (mRecord));
  mRecord.Magic    = OOB_TLM_MAGIC;
  mRecord.EntryTsc = AsmReadTsc ();
  CopyGuid (&mRecord.CallerId, &gEfiCallerIdGuid);

  mName   = VariableName;
  mInited = TRUE;
}

VOID
EFIAPI
OobTelemetryFlag (
  IN UINT32  Flags
  )
{
  if (!mInited) {
    return;
  }

  mRecord.Flags |= Flags;

  //
  // Only spend a write when the flag set actually moved. The timer path calls
  // in on every tick with the same bits and must stay free.
  //
  if (mRecord.Flags != mLastSaved) {
    Persist ();
  }
}

VOID
EFIAPI
OobTelemetryDetail (
  IN UINT32      DataLen,
  IN EFI_STATUS  LastStatus,
  IN UINT8       LastCc
  )
{
  if (!mInited) {
    return;
  }

  mRecord.DataLen    = DataLen;
  mRecord.LastStatus = (UINT32)(UINTN)LastStatus;
  mRecord.LastCc     = LastCc;
}

VOID
EFIAPI
OobTelemetryTick (
  VOID
  )
{
  if (!mInited) {
    return;
  }

  mRecord.Ticks++;
}
