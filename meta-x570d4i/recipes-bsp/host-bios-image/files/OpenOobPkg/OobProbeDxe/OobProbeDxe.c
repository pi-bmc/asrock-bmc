/** @file
  OobProbeDxe — answers one question only: "did the AMI DXE core dispatch a module
  we injected?"

  Every liveness signal we tried before was unusable on this board:

    - POST codes: AMI uses the entire 0x00-0xFF range (a stock boot emits 0xE0,
      0xE1, all of 0x70-0x79 and all of 0x60-0x6A), so no single byte proves
      anything.
    - IPMI request count: the *identical, byte-verified* stock image logged 141
      requests on one boot and 0 on the two following boots, with the KCS bridge
      active throughout. The metric is simply not reproducible.
    - Reaching ReadyToBoot: the board has a pre-existing stall at POST 0x99, so
      late-boot events may never fire at all.

  So this module writes a non-volatile UEFI variable instead. AMI's NVAR store
  lives in the same SPI flash we program over the BMC's mux, and it records
  variable names as plain ASCII — so after the run we pull the mux, read the
  flash back, and grep for OOBPROBE_VAR_NAME. That signal is persistent, is
  captured long before the 0x99 stall, and does not depend on IPMI, on POST
  codes, or on the host completing boot.

  The variable's payload doubles as a progress trace: Phase records the furthest
  milestone reached, so one readback distinguishes "never dispatched" from
  "dispatched but died before variable services" from "ran to ReadyToBoot".

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Protocol/VariableWrite.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/PciIo.h>
#include <Guid/EventGroup.h>

//
// The name is deliberately ASCII-representable and unlike any AMI variable.
// AMI's NVAR store keeps variable names as plain ASCII, while this module's own
// copy of the name sits in its PE32 as UTF-16 — so searching the flash readback
// for the *ASCII* bytes matches only a variable that was really written, never
// the driver image itself. (Verify that before each flash: the candidate image
// must contain zero ASCII occurrences.)
//
#define OOBPROBE_VAR_NAME   L"OobProbe"
#define OOBPROBE_MAGIC      0x45424F52504F4F42ULL   // "BOOPROBE" little-endian

//
// Phase values: highest reached wins. The gap between DISPATCHED and VARWRITE
// is the interesting one — it separates "our code ran" from "our code ran and
// the platform let it persist evidence".
//
#define OOBPROBE_PHASE_DISPATCHED    1   // entry point executed
#define OOBPROBE_PHASE_VARWRITE      2   // variable write services available
#define OOBPROBE_PHASE_ENDOFDXE      3   // EndOfDxe fired
#define OOBPROBE_PHASE_READYTOBOOT   4   // ReadyToBoot fired

#pragma pack(1)
typedef struct {
  UINT64      Magic;
  UINT32      Phase;
  UINT32      Writes;        // how many times we updated it this boot
  UINT64      EntryTsc;      // TSC at entry, so two boots are distinguishable
  EFI_GUID    CallerId;      // which injected copy wrote this

  //
  // Console census. BDS on this board never reaches the console-connect
  // checkpoint, never signals ReadyToBoot, and has never created ConOut /
  // ConIn / ErrOut / BootOrder — none of those four exist anywhere in the 32 MiB
  // flash. That fits two very different stories: either the video stack never
  // produces anything for BDS to connect, or it does and BDS dies on the way.
  // Counting handles distinguishes them without needing a display.
  //
  UINT16      GopHandles;      // EFI_GRAPHICS_OUTPUT_PROTOCOL producers
  UINT16      TxtOutHandles;   // SIMPLE_TEXT_OUTPUT producers
  UINT16      UgaHandles;      // legacy UGA_DRAW producers
  UINT16      PciIoHandles;    // PCI_IO producers (did enumeration finish?)
  UINT16      ConOutSet;       // gST->ConOut non-NULL
  UINT16      ConOutHandleSet; // gST->ConsoleOutHandle non-NULL
  UINT16      GopMaxMode;      // mode count on the first GOP, 0 if none
  UINT16      CensusPass;      // which hook took the census last
} OOBPROBE_RECORD;
#pragma pack()

STATIC OOBPROBE_RECORD  mRecord;
STATIC BOOLEAN          mVariablesUsable = FALSE;

/**
  Emit a multi-byte POST signature on port 0x80.

  A single byte proves nothing here, but a specific consecutive *sequence* is a
  different matter — AMI would have to emit exactly this run to false-positive.
  gBS->Stall between bytes keeps the AST2500's port-80 snoop FIFO from
  coalescing them; without the delay a fast burst can be captured as one byte.

  This is the secondary signal. The variable is the one to trust.
**/
STATIC
VOID
ProbePostSignature (
  IN UINT8  Tag
  )
{
  STATIC CONST UINT8  Sig[] = { 0x5A, 0xA5, 0x5A, 0xA5 };
  UINTN               Index;

  for (Index = 0; Index < sizeof (Sig); Index++) {
    IoWrite8 (0x80, Sig[Index]);
    if (gBS != NULL) {
      gBS->Stall (1000);
    }
  }

  IoWrite8 (0x80, Tag);
  if (gBS != NULL) {
    gBS->Stall (1000);
  }
}

/**
  Count producers of a protocol. Returns 0 on any failure, which is the answer
  we want anyway — "nothing is producing this".
**/
STATIC
UINT16
CountHandles (
  IN EFI_GUID  *Protocol
  )
{
  EFI_STATUS  Status;
  UINTN       Count  = 0;
  EFI_HANDLE  *Buf   = NULL;

  Status = gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Buf);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  if (Buf != NULL) {
    gBS->FreePool (Buf);
  }

  return (UINT16)Count;
}

/**
  Take a census of the console/graphics stack.

  Cheap enough to repeat at every hook, and repeating is the point: the counts
  at our entry point versus at late-DXE tell us whether the video stack comes up
  and BDS still fails to use it, or whether nothing ever produces a GOP for BDS
  to connect in the first place.
**/
STATIC
VOID
ProbeCensus (
  IN UINT16  Pass
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

  mRecord.GopHandles      = CountHandles (&gEfiGraphicsOutputProtocolGuid);
  mRecord.TxtOutHandles   = CountHandles (&gEfiSimpleTextOutProtocolGuid);
  mRecord.UgaHandles      = CountHandles (&gEfiUgaDrawProtocolGuid);
  mRecord.PciIoHandles    = CountHandles (&gEfiPciIoProtocolGuid);
  mRecord.ConOutSet       = (UINT16)((gST != NULL) && (gST->ConOut != NULL));
  mRecord.ConOutHandleSet = (UINT16)((gST != NULL) && (gST->ConsoleOutHandle != NULL));
  mRecord.CensusPass      = Pass;

  mRecord.GopMaxMode = 0;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && (Gop != NULL) && (Gop->Mode != NULL)) {
    mRecord.GopMaxMode = (UINT16)Gop->Mode->MaxMode;
  }
}

/**
  Persist the record, promoting Phase to the furthest milestone seen.

  Deliberately tolerant: a failure here is itself data (it means we ran but the
  platform refused the write), so it is recorded in Phase rather than fataled on.
**/
STATIC
VOID
ProbeWriteVariable (
  IN UINT32  Phase
  )
{
  EFI_STATUS  Status;

  if (Phase > mRecord.Phase) {
    mRecord.Phase = Phase;
  }

  mRecord.Writes++;

  if ((gRT == NULL) || (gRT->SetVariable == NULL)) {
    return;
  }

  Status = gRT->SetVariable (
                  OOBPROBE_VAR_NAME,
                  &gEfiCallerIdGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (mRecord),
                  &mRecord
                  );

  if (!EFI_ERROR (Status)) {
    mVariablesUsable = TRUE;
  }
}

STATIC
VOID
EFIAPI
OnVariableWriteReady (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  VOID        *Interface;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gEfiVariableWriteArchProtocolGuid, NULL, &Interface);
  if (EFI_ERROR (Status)) {
    return;
  }

  ProbeWriteVariable (OOBPROBE_PHASE_VARWRITE);
  ProbePostSignature (OOBPROBE_PHASE_VARWRITE);

  gBS->CloseEvent (Event);
}

//
// Late-DXE protocols. EndOfDxe and ReadyToBoot never fire here and timers never
// tick, but protocol notifies are signalled synchronously inside
// InstallProtocolInterface, so these are the only late hooks that work.
//
STATIC EFI_GUID  mPciEnumCompleteGuid = {
  0x30CFE3E7, 0x3DE1, 0x4586,
  { 0xBE, 0x20, 0xDE, 0xAB, 0xA1, 0xB3, 0xB7, 0x93 }
};
STATIC EFI_GUID  mDxeSmmReadyToLockGuid = {
  0x60FF8964, 0xE906, 0x41D0,
  { 0xAF, 0xED, 0xF2, 0x41, 0xE9, 0x74, 0xE0, 0x8E }
};

//
// One watch per protocol. The Context carries the GUID so the callback can
// check the protocol is REALLY there.
//
// That check is the whole point. EfiCreateProtocolNotifyEvent signals its
// callback once immediately at registration, whether or not the protocol
// exists — so the naive version of this ran all its "late" censuses back to
// back inside the entry point and reported identical zeros, which looked like a
// finding and was an artifact. Bail out until LocateProtocol actually succeeds,
// then take the census and stop watching.
//
typedef struct {
  EFI_GUID  *Protocol;
  UINT16    Pass;
} PROBE_WATCH;

STATIC PROBE_WATCH  mWatches[6];
STATIC UINTN        mWatchCount = 0;

STATIC
VOID
EFIAPI
OnLateProtocol (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  PROBE_WATCH  *Watch = (PROBE_WATCH *)Context;
  VOID         *Interface;

  if ((Watch == NULL) ||
      EFI_ERROR (gBS->LocateProtocol (Watch->Protocol, NULL, &Interface)))
  {
    return;   // spurious initial signal, or not installed yet
  }

  ProbeCensus (Watch->Pass);
  ProbeWriteVariable (mRecord.Phase);

  // One record per protocol is enough, and it bounds the flash writes.
  gBS->CloseEvent (Event);
}

STATIC
VOID
ArmProbeNotify (
  IN EFI_GUID  *Protocol,
  IN UINT16    Pass
  )
{
  VOID  *Registration;

  if (mWatchCount >= ARRAY_SIZE (mWatches)) {
    return;
  }

  mWatches[mWatchCount].Protocol = Protocol;
  mWatches[mWatchCount].Pass     = Pass;

  EfiCreateProtocolNotifyEvent (Protocol, TPL_CALLBACK, OnLateProtocol,
                                &mWatches[mWatchCount], &Registration);
  mWatchCount++;
}

STATIC
VOID
EFIAPI
OnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ProbeCensus (8);
  ProbeWriteVariable (OOBPROBE_PHASE_ENDOFDXE);
  ProbePostSignature (OOBPROBE_PHASE_ENDOFDXE);
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ProbeWriteVariable (OOBPROBE_PHASE_READYTOBOOT);
  ProbePostSignature (OOBPROBE_PHASE_READYTOBOOT);
}

EFI_STATUS
EFIAPI
OobProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;
  VOID       *Registration;

  ZeroMem (&mRecord, sizeof (mRecord));
  mRecord.Magic    = OOBPROBE_MAGIC;
  mRecord.Phase    = OOBPROBE_PHASE_DISPATCHED;
  mRecord.EntryTsc = AsmReadTsc ();

  //
  // gEfiCallerIdGuid comes from this module's FILE_GUID, so when the same code
  // is injected into two placements at once each copy stamps its own identity
  // here — and, because it is also the variable's vendor GUID, each gets its own
  // NVAR entry rather than overwriting the other.
  //
  CopyGuid (&mRecord.CallerId, &gEfiCallerIdGuid);

  //
  // Fire the POST signature before touching anything else. If the module is
  // dispatched but something later in this function faults, the port-80 trace
  // is still the proof that dispatch happened.
  //
  ProbePostSignature (OOBPROBE_PHASE_DISPATCHED);

  //
  // Try immediately: on a platform where the variable driver is already up this
  // is all that is needed, and it captures evidence at the earliest instant.
  //
  ProbeCensus (1);
  ProbeWriteVariable (OOBPROBE_PHASE_DISPATCHED);

  //
  // Re-census at the two late-DXE points that do fire here. Comparing pass 1
  // against passes 2 and 3 is the actual experiment: if GopHandles is still 0
  // at DxeSmmReadyToLock then nothing ever produced a GOP and BDS had nothing
  // to connect; if it is non-zero then the video stack came up and BDS failed
  // to use it, which points somewhere completely different.
  //
  ArmProbeNotify (&mPciEnumCompleteGuid, 2);
  ArmProbeNotify (&mDxeSmmReadyToLockGuid, 3);

  //
  // Watch for the first appearance of each thing BDS would need. These fire on
  // the install itself, so a record for pass 4/5/6 is positive proof that the
  // protocol appeared at all — and its absence is equally informative.
  //
  ArmProbeNotify (&gEfiPciIoProtocolGuid, 4);          // PCI enumeration reached the handle DB
  ArmProbeNotify (&gEfiGraphicsOutputProtocolGuid, 5); // a video driver produced a GOP
  ArmProbeNotify (&gEfiSimpleTextOutProtocolGuid, 6);  // a text console exists

  //
  // Otherwise wait for variable write services. This fires early in DXE and,
  // unlike ReadyToBoot, does not depend on the board getting past its 0x99
  // stall.
  //
  if (!mVariablesUsable) {
    EfiCreateProtocolNotifyEvent (
      &gEfiVariableWriteArchProtocolGuid,
      TPL_CALLBACK,
      OnVariableWriteReady,
      NULL,
      &Registration
      );
  }

  gBS->CreateEventEx (
         EVT_NOTIFY_SIGNAL,
         TPL_CALLBACK,
         OnEndOfDxe,
         NULL,
         &gEfiEndOfDxeEventGroupGuid,
         &Event
         );

  gBS->CreateEventEx (
         EVT_NOTIFY_SIGNAL,
         TPL_CALLBACK,
         OnReadyToBoot,
         NULL,
         &gEfiEventReadyToBootGuid,
         &Event
         );

  return EFI_SUCCESS;
}
