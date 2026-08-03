/** @file
  OobTelemetryLib — record what an injected driver actually did, somewhere the
  BMC can read it back.

  This exists because every in-band signal on this board turned out to be
  unusable. IPMI request counts are irreproducible across boots of a
  byte-identical image (141 on one boot, 0 on the next two). POST codes are
  worse: AMI uses the whole 0x00-0xFF range, and a driver that provably ran had
  its deliberate port-80 signature never reach the BMC snoop at all.

  What does work is a non-volatile UEFI variable. AMI's NVAR store lives in the
  same SPI flash we can take over through the BMC's mux, so after a boot we pull
  the mux, read the flash, and decode the record — no IPMI, no port 80, and it
  survives the board's stall at POST 0x99. See tools/probecheck.py and
  .claude/host-bios-driver-injection-results.md.

  Usage is deliberately blunt: mark milestones as they happen, and let the
  library decide when to spend a flash write.

    OobTelemetryInit (L"OobSmbios");
    OobTelemetryFlag (OOB_TLM_ENTRY);
    ...
    OobTelemetryFlag (OOB_TLM_PUSH_OK);

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OOB_TELEMETRY_LIB_H_
#define OOB_TELEMETRY_LIB_H_

#include <Uefi.h>

#define OOB_TLM_MAGIC  0x304D4C54424F4FULL   // "OOBTLM0"

//
// Generic milestones. Drivers share the low bits so one decoder handles all of
// them; anything driver-specific goes in the high half.
//
#define OOB_TLM_ENTRY            0x00000001u  // entry point ran
#define OOB_TLM_KCS_OK           0x00000002u  // a KCS transaction completed
#define OOB_TLM_KCS_FAIL         0x00000004u  // a KCS transaction failed
#define OOB_TLM_ENDOFDXE         0x00000008u  // EndOfDxe group fired
#define OOB_TLM_READYTOBOOT      0x00000010u  // ReadyToBoot group fired
#define OOB_TLM_TIMER            0x00000020u  // periodic timer fired at least once
#define OOB_TLM_DATA_FOUND       0x00000040u  // the driver found its source data
#define OOB_TLM_DATA_MISSING     0x00000080u  // ...or did not
#define OOB_TLM_XFER_OK          0x00000100u  // payload delivered to the BMC
#define OOB_TLM_XFER_FAIL        0x00000200u  // delivery attempted and failed
#define OOB_TLM_XFER_SKIP        0x00000400u  // delivery unnecessary (BMC current)
#define OOB_TLM_PROTO_READY      0x00000800u  // a protocol the driver waited on appeared
//
// Set when SetTimer returns success. Distinguishes "the timer was never armed"
// from "it was armed and never fired" — on the 2026-07-30 run a driver that
// provably ran reported Ticks == 0 after five minutes of wall clock, and those
// two explanations call for completely different fixes.
//
#define OOB_TLM_TIMER_ARMED      0x00001000u

#pragma pack(1)
typedef struct {
  UINT64      Magic;
  UINT32      Flags;
  UINT32      Writes;      // flash writes spent, so a truncated run is obvious
  UINT32      Ticks;       // timer ticks observed
  UINT32      DataLen;     // payload size, driver-defined
  UINT32      LastStatus;  // low 32 bits of the last interesting EFI_STATUS
  UINT8       LastCc;      // last IPMI completion code
  UINT8       Reserved[3];
  UINT64      EntryTsc;    // distinguishes one boot's record from another's
  EFI_GUID    CallerId;    // which module wrote this
} OOB_TELEMETRY_RECORD;
#pragma pack()

/**
  Name the variable and stamp the record. Safe to call before variable services
  exist; nothing is written until there is something to say.
**/
VOID
EFIAPI
OobTelemetryInit (
  IN CONST CHAR16  *VariableName
  );

/**
  Set milestone bits. Writes to flash only when this actually changes the record,
  so a 600-tick timer loop costs one write, not six hundred.
**/
VOID
EFIAPI
OobTelemetryFlag (
  IN UINT32  Flags
  );

/**
  Record a payload size / status / completion code alongside the flags. Does not
  itself trigger a write — pair it with OobTelemetryFlag.
**/
VOID
EFIAPI
OobTelemetryDetail (
  IN UINT32      DataLen,
  IN EFI_STATUS  LastStatus,
  IN UINT8       LastCc
  );

/**
  Note a timer tick. Cheap: the count rides along with the next real write.
**/
VOID
EFIAPI
OobTelemetryTick (
  VOID
  );

#endif // OOB_TELEMETRY_LIB_H_
