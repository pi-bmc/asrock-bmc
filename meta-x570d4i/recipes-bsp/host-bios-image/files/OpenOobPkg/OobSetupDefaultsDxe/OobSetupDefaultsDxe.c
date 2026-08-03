/** @file
  OobSetupDefaultsDxe — enforce Setup values the platform must boot with.

  Some BIOS knobs are not preferences on this board; the machine does not come
  up without them. This driver reasserts that small set on every boot, so a
  Setup "load defaults", an NVRAM clear, or a BMC-side write cannot silently
  reintroduce a configuration that hangs POST.

  ---------------------------------------------------------------------------
  What is enforced and why
  ---------------------------------------------------------------------------
  Above 4G Decoding = Enabled (PCI_COMMON offset 3, "PCID001")

    The stock default is 0x00. With a large-BAR card installed — the Tesla K80
    this board is built around is dual-GK210 with a ~12 GiB prefetchable BAR per
    GPU — the BIOS cannot fit the apertures below 4 GiB and wedges just after
    PCI resource assignment, terminal at POST 0x99 with no console. Enabling it
    is what cleared that hang, and it is also what finally let the host
    enumerate the USB gadget the Redfish Host Interface depends on.

  SR-IOV Support = Disabled (PCI_COMMON offset 5, "PCID002")

    Stock default is already 0x00 and nothing here needs it. It is enforced
    rather than assumed because it shares the Above-4G resource path: SR-IOV
    makes each capable function request its full VF BAR window up front, which
    is exactly the pressure that produced the hang above. Pinning it off keeps
    the failure from returning by a second route.

  Offsets and encodings come from the shipped HII decompilation
  (x570d4i2t-bios-knobs.xml), not from guesswork:

    <knob name="PCID001" varstoreIndex="2" prompt="Above 4G Decoding"
          setupType="checkbox" size="1" offset="0x0003" default="0x00"/>
    <knob name="PCID002" varstoreIndex="2" prompt="SR-IOV Support"
          setupType="checkbox" size="1" offset="0x0005" default="0x00"/>

  varstoreIndex 2 is PCI_COMMON, GUID ACA9F304-21E2-4852-9875-7FF4881D67A5 (see
  BiosCfgOobDxe/BiosCfgOobVarstores.h — the GUID matters, since two varstores on
  this board share the name "Setup" and differ only by GUID). setupType
  "checkbox" means 0x00 unchecked / 0x01 checked.

  ---------------------------------------------------------------------------
  Why this is a separate driver, and why it resets
  ---------------------------------------------------------------------------
  BiosCfgOobDxe does its varstore work at EndOfDxe, which is far too late: PCI
  resource assignment has long since happened. This driver instead DEPEXes on
  the variable architectural protocols, so it dispatches as soon as variables
  are readable and writable — the earliest point a DXE module can act.

  Even so, a same-boot effect is NOT relied upon. On this AMD platform the
  above-4G window can be established before DXE runs at all, so a value written
  here may not be observed until the firmware restarts. Therefore: when a value
  actually had to be changed, and the change is confirmed to have persisted,
  this driver resets the platform. The corrected value is then in place from the
  reset vector, exactly as if it had been patched into the NVAR store.

  The reset cannot loop. It is gated on a read-back that proves the new value
  survived: on the following boot the values already match, nothing is written,
  and no reset is requested. If the read-back shows the write did NOT stick, the
  variable store is broken and resetting would not help — so this logs and
  continues rather than rebooting forever on a board whose only console is the
  KVM we are still fixing.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/IpmiKcsLib.h>

///
/// PCI_COMMON, varstore index 2. Name alone is not a safe key on this board.
///
STATIC CONST CHAR16  mPciCommonName[] = L"PCI_COMMON";
STATIC CONST EFI_GUID  mPciCommonGuid = {
  0xACA9F304, 0x21E2, 0x4852, { 0x98, 0x75, 0x7F, 0xF4, 0x88, 0x1D, 0x67, 0xA5 }
};

typedef struct {
  UINTN           Offset;
  UINT8           Value;
  CONST CHAR8     *Knob;
  CONST CHAR8     *Prompt;
} OOB_REQUIRED_VALUE;

STATIC CONST OOB_REQUIRED_VALUE  mRequired[] = {
  { 0x0003, 0x01, "PCID001", "Above 4G Decoding = Enabled"  },
  { 0x0005, 0x00, "PCID002", "SR-IOV Support = Disabled"    },
};

#define OOB_REQUIRED_COUNT  (sizeof (mRequired) / sizeof (mRequired[0]))

///
/// POST codes, so this driver is observable on a board with no console. The BMC
/// snoops port 0x80.
///
#define OOB_DEFAULTS_POST_ENTRY      0xD0
#define OOB_DEFAULTS_POST_OK         0xD1  // already correct, nothing to do
#define OOB_DEFAULTS_POST_WROTE      0xD2  // changed and verified, resetting
#define OOB_DEFAULTS_POST_FAILED     0xDE  // could not read or could not persist

/**
  Read PCI_COMMON into an allocated buffer.

  @param[out]  Buffer  Receives the allocated contents; caller frees.
  @param[out]  Size    Receives the length.
  @param[out]  Attr    Receives the variable attributes, so the write-back
                       preserves them rather than inventing a new set.
**/
STATIC
EFI_STATUS
ReadPciCommon (
  OUT UINT8   **Buffer,
  OUT UINTN   *Size,
  OUT UINT32  *Attr
  )
{
  EFI_STATUS  Status;
  EFI_GUID    Guid;
  UINT8       *Buf;
  UINTN       Len;

  Guid = mPciCommonGuid;
  Len  = 0;

  Status = gRT->GetVariable ((CHAR16 *)mPciCommonName, &Guid, Attr, &Len, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    //
    // EFI_SUCCESS with a zero length would be just as wrong as an error here:
    // either way there is no varstore to correct.
    //
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  Buf = AllocateZeroPool (Len);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Guid   = mPciCommonGuid;
  Status = gRT->GetVariable ((CHAR16 *)mPciCommonName, &Guid, Attr, &Len, Buf);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }

  *Buffer = Buf;
  *Size   = Len;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
OobSetupDefaultsEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_GUID    Guid;
  UINT8       *Buf;
  UINTN       Size;
  UINT32      Attr;
  UINTN       Index;
  BOOLEAN     Changed;

  OobPostCode (OOB_DEFAULTS_POST_ENTRY);

  Buf    = NULL;
  Size   = 0;
  Attr   = 0;
  Status = ReadPciCommon (&Buf, &Size, &Attr);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OobDefaults: PCI_COMMON unreadable - %r\n", Status));
    OobPostCode (OOB_DEFAULTS_POST_FAILED);
    return EFI_SUCCESS; // never fail dispatch; a stranded driver helps nobody
  }

  Changed = FALSE;
  for (Index = 0; Index < OOB_REQUIRED_COUNT; Index++) {
    CONST OOB_REQUIRED_VALUE  *Req = &mRequired[Index];

    //
    // The varstore is 7 bytes on 2.59C/2.59F, but a future BIOS could shrink
    // it. Writing past the end would corrupt an unrelated setting, so skip
    // rather than clamp.
    //
    if (Req->Offset >= Size) {
      DEBUG ((
        DEBUG_ERROR,
        "OobDefaults: %a offset 0x%x beyond PCI_COMMON (%u bytes) - skipped\n",
        Req->Knob,
        (UINT32)Req->Offset,
        (UINT32)Size
        ));
      continue;
    }

    if (Buf[Req->Offset] != Req->Value) {
      DEBUG ((
        DEBUG_WARN,
        "OobDefaults: %a (%a): 0x%02x -> 0x%02x\n",
        Req->Knob,
        Req->Prompt,
        Buf[Req->Offset],
        Req->Value
        ));
      Buf[Req->Offset] = Req->Value;
      Changed          = TRUE;
    }
  }

  if (!Changed) {
    DEBUG ((DEBUG_INFO, "OobDefaults: PCI_COMMON already correct\n"));
    FreePool (Buf);
    OobPostCode (OOB_DEFAULTS_POST_OK);
    return EFI_SUCCESS;
  }

  Guid   = mPciCommonGuid;
  Status = gRT->SetVariable (
                  (CHAR16 *)mPciCommonName,
                  &Guid,
                  Attr,
                  Size,
                  Buf
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OobDefaults: SetVariable failed - %r\n", Status));
    FreePool (Buf);
    OobPostCode (OOB_DEFAULTS_POST_FAILED);
    return EFI_SUCCESS;
  }

  //
  // Verify before resetting. This read-back is the entire loop guard: we only
  // reboot when the new value is proven to be in the store, which means the
  // next boot will find it already correct and will not reset again.
  //
  ZeroMem (Buf, Size);
  Guid   = mPciCommonGuid;
  Status = gRT->GetVariable ((CHAR16 *)mPciCommonName, &Guid, &Attr, &Size, Buf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OobDefaults: verify read failed - %r\n", Status));
    FreePool (Buf);
    OobPostCode (OOB_DEFAULTS_POST_FAILED);
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < OOB_REQUIRED_COUNT; Index++) {
    CONST OOB_REQUIRED_VALUE  *Req = &mRequired[Index];

    if ((Req->Offset < Size) && (Buf[Req->Offset] != Req->Value)) {
      //
      // The write reported success but did not stick. Resetting now would
      // reboot forever, so stop here and leave the board bootable.
      //
      DEBUG ((
        DEBUG_ERROR,
        "OobDefaults: %a did not persist (still 0x%02x) - NOT resetting\n",
        Req->Knob,
        Buf[Req->Offset]
        ));
      FreePool (Buf);
      OobPostCode (OOB_DEFAULTS_POST_FAILED);
      return EFI_SUCCESS;
    }
  }

  FreePool (Buf);

  DEBUG ((DEBUG_WARN, "OobDefaults: values corrected and verified, resetting\n"));
  OobPostCode (OOB_DEFAULTS_POST_WROTE);

  //
  // Warm reset re-runs from the reset vector, so the corrected values are in
  // effect before any phase that consumes them. Cold reset is not used: it
  // drops the host for longer than the BMC's power-control expects and, on this
  // board, risks tripping the PSU protection latch that needs a ~120 s cooldown.
  //
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  //
  // Not reached. If the platform ignores the reset request, fall through rather
  // than spin — the values are already correct in NVRAM and the next boot for
  // any reason will pick them up.
  //
  return EFI_SUCCESS;
}
