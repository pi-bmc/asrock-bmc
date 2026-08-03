/** @file
  VideoRouteDxe — per-boot selection of the console display controller.

  WHY THIS EXISTS
  ---------------
  The stock BIOS expresses video policy only as static Setup questions in the
  `Setup` NVRAM variable, evaluated long before anyone knows what is actually
  plugged into the PCIe slot. On a headless, BMC-managed board that is the wrong
  shape: the right answer depends on what is present this boot.

  THE GATE
  --------
  Aspeed2500UefiDriver (d88a9618), the only ASPEED UEFI GOP driver in the image,
  has a single-GUID DXE DEPEX: 794E15D9-BF1B-4568-99AC-DCE207C022E4. AMI's
  SlotOpRomDXE (31508ac1) is the only producer of that GUID anywhere in the ROM,
  and it installs it only when Setup[433] == 0 (CSM disabled) or Setup[442] == 1
  (Launch Video OpROM Policy == UEFI only).

  The shipped ROM has Setup[433] = 2 (CSM Custom) and Setup[442] = 2 (Legacy
  only). Neither condition holds, so on a stock board the ASPEED UEFI GOP driver
  is NEVER DISPATCHED — the only video path that exists is the legacy ASPEED
  VBIOS wrapped back into a GOP by CsmVideo's INT10h shim.

  Installing that GUID ourselves makes the DXE core re-evaluate pending DEPEXes
  and dispatch the real GOP driver. No NVRAM write, no reset, no ordering race
  against SlotOpRomDXE — a duplicate install on a separate handle is harmless.

  POLICY: DEFAULT TO ONBOARD
  --------------------------
  Selection is driven by what is KNOWN to work, not by what is merely present:

    * The onboard ASPEED is always known-good. This firmware image demonstrably
      contains a native ASPEED GOP driver and an ASPEED legacy VBIOS, so if the
      device is on the bus, something here can drive it. It is also the only path
      the BMC KVM can capture. It is therefore the default.

    * A discrete GPU is known-good only with positive evidence: it must carry its
      own expansion ROM (so a driver ships with the card) AND sit on the bridge
      path the platform marked VGA-routed. Absent that, selecting it would be a
      guess, and a wrong guess on a headless board means no console at all.

  The ASPEED gate is opened whenever an ASPEED device is present, INDEPENDENT of
  which display is primary. That is what makes "default to onboard" robust: even
  when a discrete GPU is chosen as primary, the KVM path stays alive, and
  ConSplitter is happy to drive both consoles.

  DO NOT ASSUME THIS FIXES THE BLANK KVM. The equivalent test via NVRAM (writing
  CSM=0 and VideoOpROM=1 straight to flash) was run on this board on 2026-07-27
  and video stayed black, with SCU50/54/58 still reading zero afterwards. So
  either the GOP driver does dispatch and the black framebuffer has a later
  cause, or the decompiled gate condition is incomplete. What this driver buys is
  a cleaner discriminating experiment — it perturbs exactly one thing instead of
  disabling the whole CSM path — plus GOP-arrival confirmation over POST codes,
  which is the only feedback channel available with no display.

  WHAT IT DELIBERATELY DOES NOT DO
  --------------------------------
  Turning the ASPEED GOP *off* is not symmetric: a protocol installation cannot
  be retracted, so suppression would mean beating SlotOpRomDXE to the Setup
  variable from an apriori slot. Not implemented.

  Choosing legacy VBIOS over UEFI GOP is also not implemented, because the
  premise is not knowable here: BDS has not built the boot-option list when a
  DEPEX=TRUE driver runs, and BootOrder at this point reflects the PREVIOUS
  boot's enumeration. Setup[439] (CSM006 "Boot option filter") is the only
  authoritative pre-video signal and is read for reporting only.

  See docs/VIDEO-ROUTING.md.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PciCf8Lib.h>
#include <Library/PcdLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/OobVideoRoute.h>

//
// PCI enumeration bounds and config-space offsets.
//
#define PCI_MAX_BUS       255
#define PCI_MAX_DEVICE    31
#define PCI_MAX_FUNCTION  7

#define PCI_OFF_VENDOR_ID    0x00
#define PCI_OFF_COMMAND      0x04
#define PCI_OFF_CLASS_REV    0x08
#define PCI_OFF_HEADER_TYPE  0x0E
#define PCI_OFF_SECONDARY    0x19
#define PCI_OFF_SUBORDINATE  0x1A
#define PCI_OFF_ROM_BAR      0x30
#define PCI_OFF_BRIDGE_CTL   0x3E

#define PCI_HEADER_MULTIFUNCTION  0x80

#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_BRIDGE         0x06
#define PCI_SUBCLASS_P2P_BRIDGE  0x04

#define PCI_CMD_IO_SPACE      BIT0
#define PCI_CMD_MEMORY_SPACE  BIT1

#define PCI_BRIDGE_CTL_VGA_ENABLE  BIT3

//
// The expansion ROM BAR's address bits live in 31:11; bit 0 is the enable bit.
//
#define PCI_ROM_ADDRESS_MASK  0xFFFFF800

//
// POST breadcrumbs. The BMC snoops port 0x80, so these are visible out-of-band
// even with no display — which is the entire point on this board.
//
#define POST_VIDEOROUTE_ENTRY         0xE0
#define POST_VIDEOROUTE_NO_DISPLAY    0xE1
#define POST_VIDEOROUTE_GATE_OPENED   0xE2
#define POST_VIDEOROUTE_GATE_ALREADY  0xE3
#define POST_VIDEOROUTE_NO_ASPEED     0xE4
#define POST_VIDEOROUTE_GOP_SEEN      0xE5

STATIC OPEN_OOB_VIDEO_ROUTE_PROTOCOL  mRoute;
STATIC EFI_EVENT                      mGopEvent;
STATIC VOID                           *mGopRegistration;

//
// Bridges discovered during the scan, used to decide which display devices sit
// on the platform's VGA-routed path.
//
#define MAX_BRIDGES  64

typedef struct {
  UINT8      Secondary;
  UINT8      Subordinate;
  BOOLEAN    VgaEnable;
} PCI_BRIDGE_INFO;

STATIC PCI_BRIDGE_INFO  mBridges[MAX_BRIDGES];
STATIC UINTN            mBridgeCount;

/**
  Emit a POST breadcrumb.
**/
STATIC
VOID
PostCode (
  IN UINT8  Value
  )
{
  IoWrite8 (FixedPcdGet16 (PcdOobPostCodePort), Value);
}

// ---------------------------------------------------------------------------
// PCI scan
// ---------------------------------------------------------------------------

/**
  Probe whether a device implements an expansion ROM BAR.

  Writes all-ones to the address bits, reads back the implemented mask, then
  restores the original value. A device with no ROM BAR returns 0. The enable bit
  (bit 0) is preserved throughout so this never turns a ROM decode on.
**/
STATIC
BOOLEAN
HasExpansionRom (
  IN UINTN  Bus,
  IN UINTN  Device,
  IN UINTN  Function
  )
{
  UINTN   Address;
  UINT32  Original;
  UINT32  Mask;

  Address  = PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_ROM_BAR);
  Original = PciCf8Read32 (Address);

  PciCf8Write32 (Address, PCI_ROM_ADDRESS_MASK | (Original & BIT0));
  Mask = PciCf8Read32 (Address);
  PciCf8Write32 (Address, Original);

  return (BOOLEAN)((Mask & PCI_ROM_ADDRESS_MASK) != 0);
}

/**
  Record a PCI-to-PCI bridge and whether legacy VGA decode is routed through it.
**/
STATIC
VOID
RecordBridge (
  IN UINTN  Bus,
  IN UINTN  Device,
  IN UINTN  Function
  )
{
  UINT16  Control;

  if (mBridgeCount >= MAX_BRIDGES) {
    DEBUG ((DEBUG_WARN, "VideoRoute: more than %d bridges; VGA routing may be wrong\n", MAX_BRIDGES));
    return;
  }

  Control = PciCf8Read16 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_BRIDGE_CTL));

  mBridges[mBridgeCount].Secondary   = PciCf8Read8 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_SECONDARY));
  mBridges[mBridgeCount].Subordinate = PciCf8Read8 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_SUBORDINATE));
  mBridges[mBridgeCount].VgaEnable   = (BOOLEAN)((Control & PCI_BRIDGE_CTL_VGA_ENABLE) != 0);
  mBridgeCount++;
}

/**
  Record a display controller.
**/
STATIC
VOID
RecordDisplay (
  IN UINTN   Bus,
  IN UINTN   Device,
  IN UINTN   Function,
  IN UINT32  Id,
  IN UINT32  ClassRev
  )
{
  OOB_VIDEO_DEVICE  *Entry;

  if (mRoute.DeviceCount >= OOB_VIDEO_MAX_DEVICES) {
    DEBUG ((
      DEBUG_WARN,
      "VideoRoute: more than %d display devices; ignoring the rest\n",
      OOB_VIDEO_MAX_DEVICES
      ));
    return;
  }

  Entry = &mRoute.Devices[mRoute.DeviceCount];
  ZeroMem (Entry, sizeof (*Entry));

  Entry->Segment         = 0;
  Entry->Bus             = (UINT8)Bus;
  Entry->Device          = (UINT8)Device;
  Entry->Function        = (UINT8)Function;
  Entry->VendorId        = (UINT16)Id;
  Entry->DeviceId        = (UINT16)(Id >> 16);
  Entry->ProgIf          = (UINT8)(ClassRev >> 8);
  Entry->SubClass        = (UINT8)(ClassRev >> 16);
  Entry->Command         = PciCf8Read16 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_COMMAND));
  Entry->HasExpansionRom = HasExpansionRom (Bus, Device, Function);
  Entry->IsOnboardBmcVga = (BOOLEAN)(Entry->VendorId == OOB_VIDEO_VENDOR_ASPEED);

  mRoute.DeviceCount++;
}

/**
  Single pass over PCI config space, collecting both bridges and display devices.

  Uses PciCf8Lib (legacy 0xCF8/0xCFC config access) rather than MMIO ECAM: at
  DEPEX=TRUE the PCI Express base address PCD is not reliably populated in a
  foreign DXE core, whereas CF8/CFC is architecturally always available on x86.
  The cost is being limited to segment 0, which is fine on a single-socket AM4
  board.

  Multi-function devices are probed only when function 0's header type reports
  multi-function, to avoid aliasing reads on single-function devices.
**/
STATIC
VOID
ScanPciDevices (
  VOID
  )
{
  UINTN   Bus;
  UINTN   Device;
  UINTN   Function;
  UINTN   MaxFunction;
  UINT32  Id;
  UINT32  ClassRev;
  UINT8   HeaderType;
  UINT8   BaseClass;
  UINT8   SubClass;

  mRoute.DeviceCount = 0;
  mBridgeCount       = 0;

  for (Bus = 0; Bus <= PCI_MAX_BUS; Bus++) {
    for (Device = 0; Device <= PCI_MAX_DEVICE; Device++) {
      MaxFunction = 0;

      for (Function = 0; Function <= MaxFunction; Function++) {
        Id = PciCf8Read32 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_VENDOR_ID));
        if (((UINT16)Id == 0xFFFF) || ((UINT16)Id == 0x0000)) {
          if (Function == 0) {
            break;      // no function 0 means no device here at all
          }

          continue;
        }

        if (Function == 0) {
          HeaderType = PciCf8Read8 (PCI_CF8_LIB_ADDRESS (Bus, Device, 0, PCI_OFF_HEADER_TYPE));
          if ((HeaderType & PCI_HEADER_MULTIFUNCTION) != 0) {
            MaxFunction = PCI_MAX_FUNCTION;
          }
        }

        ClassRev  = PciCf8Read32 (PCI_CF8_LIB_ADDRESS (Bus, Device, Function, PCI_OFF_CLASS_REV));
        BaseClass = (UINT8)(ClassRev >> 24);
        SubClass  = (UINT8)(ClassRev >> 16);

        if ((BaseClass == PCI_CLASS_BRIDGE) && (SubClass == PCI_SUBCLASS_P2P_BRIDGE)) {
          RecordBridge (Bus, Device, Function);
        } else if (BaseClass == PCI_CLASS_DISPLAY) {
          RecordDisplay (Bus, Device, Function, Id, ClassRev);
        }
      }
    }
  }
}

/**
  Decide whether a bus number is reachable through VGA-enabled bridges only.

  The platform sets Bridge Control VGA Enable on every bridge along the path to
  the display it elected as primary, so this is real hardware evidence rather
  than inference. Bus 0 needs no bridge.

  A non-zero bus with no covering bridge is reported as NOT routed: that should be
  impossible, so treating it as routed would mean trusting a scan we know is
  incomplete.
**/
STATIC
BOOLEAN
IsBusVgaRouted (
  IN UINT8  Bus
  )
{
  UINTN    Index;
  BOOLEAN  Covered;

  if (Bus == 0) {
    return TRUE;
  }

  Covered = FALSE;
  for (Index = 0; Index < mBridgeCount; Index++) {
    if ((Bus >= mBridges[Index].Secondary) && (Bus <= mBridges[Index].Subordinate)) {
      Covered = TRUE;
      if (!mBridges[Index].VgaEnable) {
        return FALSE;
      }
    }
  }

  return Covered;
}

/**
  Classify how confident we are that each display device can produce a console.
**/
STATIC
VOID
AssessAvailability (
  VOID
  )
{
  UINT32            Index;
  OOB_VIDEO_DEVICE  *Entry;

  for (Index = 0; Index < mRoute.DeviceCount; Index++) {
    Entry                  = &mRoute.Devices[Index];
    Entry->OnVgaRoutedPath = IsBusVgaRouted (Entry->Bus);

    if (Entry->IsOnboardBmcVga) {
      //
      // Build-time knowledge, not a runtime probe: this firmware image contains
      // Aspeed2500UefiDriver (a native ASPEED GOP driver) and an ASPEED legacy
      // VBIOS. If the device is on the bus, we have something that drives it.
      // Deliberately NOT conditioned on OnVgaRoutedPath — on this board the
      // AST2500 sits several bridges deep, and the stock ConOut device path
      // confirms the firmware targets it regardless.
      //
      Entry->Availability = OobVideoAvailKnownGood;
    } else if (!Entry->HasExpansionRom) {
      //
      // No ROM of its own, and this firmware ships GOP/VBIOS images only for the
      // AMD integrated graphics and the ASPEED. Nothing known drives this.
      //
      Entry->Availability = OobVideoAvailNoKnownDriver;
    } else if (!Entry->OnVgaRoutedPath) {
      Entry->Availability = OobVideoAvailNotVgaRouted;
    } else {
      Entry->Availability = OobVideoAvailKnownGood;
    }

    DEBUG ((
      DEBUG_INFO,
      "VideoRoute: %02x:%02x.%x %04x:%04x class 03:%02x:%02x cmd %04x rom=%d vga-routed=%d avail=%d%a\n",
      Entry->Bus,
      Entry->Device,
      Entry->Function,
      Entry->VendorId,
      Entry->DeviceId,
      Entry->SubClass,
      Entry->ProgIf,
      Entry->Command,
      Entry->HasExpansionRom,
      Entry->OnVgaRoutedPath,
      Entry->Availability,
      Entry->IsOnboardBmcVga ? " (onboard BMC VGA)" : ""
      ));
  }
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

/**
  Find the first device matching an onboard/discrete role at a given confidence.

  @param  WantOnboard  TRUE to match the onboard ASPEED, FALSE for discrete.
  @param  RequireKnown TRUE to accept only OobVideoAvailKnownGood.

  @return Index into mRoute.Devices[], or -1.
**/
STATIC
INT32
FindDevice (
  IN BOOLEAN  WantOnboard,
  IN BOOLEAN  RequireKnown
  )
{
  UINT32  Index;

  for (Index = 0; Index < mRoute.DeviceCount; Index++) {
    if (mRoute.Devices[Index].IsOnboardBmcVga != WantOnboard) {
      continue;
    }

    if (RequireKnown && (mRoute.Devices[Index].Availability != OobVideoAvailKnownGood)) {
      continue;
    }

    return (INT32)Index;
  }

  return -1;
}

/**
  Choose the primary console display.

  Order of preference:
    1. A known-good discrete GPU, but only if PcdVideoRoutePreferDiscreteGpu is
       set. Off by default.
    2. The onboard ASPEED — the default, and known-good whenever present.
    3. A known-good discrete GPU, when there is no onboard ASPEED at all.
    4. Any display device we found, as a last resort, with its (lower)
       availability recorded so consumers know the choice was not evidence-based.
    5. Nothing: headless, serial console only.
**/
STATIC
VOID
SelectPath (
  VOID
  )
{
  INT32  Onboard;
  INT32  Discrete;

  Onboard  = FindDevice (TRUE, TRUE);
  Discrete = FindDevice (FALSE, TRUE);

  if ((Discrete >= 0) && FeaturePcdGet (PcdVideoRoutePreferDiscreteGpu)) {
    mRoute.Path         = OobVideoPathDiscreteGpu;
    mRoute.PrimaryIndex = Discrete;
  } else if (Onboard >= 0) {
    mRoute.Path         = OobVideoPathOnboardAspeed;
    mRoute.PrimaryIndex = Onboard;
  } else if (Discrete >= 0) {
    mRoute.Path         = OobVideoPathDiscreteGpu;
    mRoute.PrimaryIndex = Discrete;
  } else if (mRoute.DeviceCount > 0) {
    //
    // Something is there but nothing is known-good. Report it rather than
    // claiming the board is headless, and let the recorded Availability say why
    // this is a guess.
    //
    mRoute.PrimaryIndex = 0;
    mRoute.Path         = mRoute.Devices[0].IsOnboardBmcVga
                          ? OobVideoPathOnboardAspeed
                          : OobVideoPathDiscreteGpu;
    DEBUG ((DEBUG_WARN, "VideoRoute: no known-good display; falling back to device 0\n"));
  } else {
    mRoute.Path         = OobVideoPathNone;
    mRoute.PrimaryIndex = -1;
  }
}

//
// AMI's `Setup` variable and the byte offsets that gate video. Offsets verified
// two independent ways: against the extracted HII database, and against
// SlotOpRomDXE's own machine code, which indexes Setup+433 and Setup+442.
//
STATIC CONST CHAR16  mSetupVarName[] = L"Setup";

STATIC EFI_GUID  mAmiSetupVarGuid = {
  0xEC87D643, 0xEBA4, 0x4BB5, { 0xA1, 0xE5, 0x3F, 0x3E, 0x36, 0xB2, 0x0D, 0xA9 }
};

#define SETUP_OFF_CSM          433    // CSM005: 0=Disabled 1=Enabled 2=Custom
#define SETUP_OFF_BOOT_FILTER  439    // CSM006: 0=UEFI+Legacy 1=Legacy only 2=UEFI only
#define SETUP_OFF_VIDEO_OPROM  442    // CSM009: 0=none 1=UEFI only 2=Legacy only
#define SETUP_READ_SIZE        0x1FF  // what SlotOpRomDXE itself asks for

/**
  Read the video-policy bytes from the AMI `Setup` variable and work out whether
  the stock firmware would open the ASPEED GOP gate on its own.

  A missing or short variable is not an error: it happens after an NVRAM clear,
  and SlotOpRomDXE has its own fallback for it. In that case we assume the gate
  is shut, which is the safe assumption — it makes us install the marker, and a
  redundant install costs nothing.
**/
STATIC
VOID
ReadSetupPolicy (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Buffer[SETUP_READ_SIZE];
  UINTN       Size;

  mRoute.SetupPolicyValid = FALSE;
  mRoute.GateWasShut      = TRUE;
  mRoute.Mode             = OobVideoModeUefiGop;

  Size   = sizeof (Buffer);
  Status = gRT->GetVariable (
                  (CHAR16 *)mSetupVarName,
                  &mAmiSetupVarGuid,
                  NULL,
                  &Size,
                  Buffer
                  );
  if (EFI_ERROR (Status) || (Size <= SETUP_OFF_VIDEO_OPROM)) {
    DEBUG ((
      DEBUG_WARN,
      "VideoRoute: Setup unreadable (%r, %d bytes); assuming the GOP gate is shut\n",
      Status,
      (UINT32)Size
      ));
    return;
  }

  mRoute.SetupPolicyValid = TRUE;
  mRoute.SetupCsm         = Buffer[SETUP_OFF_CSM];
  mRoute.SetupBootFilter  = Buffer[SETUP_OFF_BOOT_FILTER];
  mRoute.SetupVideoOpRom  = Buffer[SETUP_OFF_VIDEO_OPROM];

  mRoute.GateWasShut = (BOOLEAN)!((mRoute.SetupCsm == 0) || (mRoute.SetupVideoOpRom == 1));

  //
  // Report the driver model the stock policy produces. We do not choose it: with
  // the gate shut, only the legacy VBIOS path exists until we open it.
  //
  mRoute.Mode = mRoute.GateWasShut ? OobVideoModeLegacyVbios : OobVideoModeUefiGop;

  DEBUG ((
    DEBUG_INFO,
    "VideoRoute: Setup CSM=%d BootFilter=%d VideoOpRom=%d -> ASPEED GOP gate %a\n",
    mRoute.SetupCsm,
    mRoute.SetupBootFilter,
    mRoute.SetupVideoOpRom,
    mRoute.GateWasShut ? "SHUT (stock firmware will not load the GOP driver)" : "opens by itself"
    ));
}

// ---------------------------------------------------------------------------
// Enforcement and verification
// ---------------------------------------------------------------------------

/**
  Notify callback: count EFI_GRAPHICS_OUTPUT_PROTOCOL instances as they appear.

  This is the only positive confirmation available on a board with no display —
  it proves a GOP producer actually started, rather than just that we installed a
  marker. The first arrival also emits a POST code, which the BMC's port-80 snoop
  can see out-of-band.
**/
STATIC
VOID
EFIAPI
OnGopInstalled (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       HandleSize;

  //
  // Drain every handle the registration has queued; a single signal can cover
  // more than one installation.
  //
  while (TRUE) {
    HandleSize = sizeof (Handle);
    Status     = gBS->LocateHandle (
                        ByRegisterNotify,
                        NULL,
                        mGopRegistration,
                        &HandleSize,
                        &Handle
                        );
    if (EFI_ERROR (Status)) {
      break;
    }

    if (mRoute.GopInstancesSeen == 0) {
      PostCode (POST_VIDEOROUTE_GOP_SEEN);
    }

    mRoute.GopInstancesSeen++;
    DEBUG ((
      DEBUG_INFO,
      "VideoRoute: GraphicsOutputProtocol instance %d observed\n",
      mRoute.GopInstancesSeen
      ));
  }
}

/**
  Open the ASPEED GOP dispatch gate by installing the marker GUID its DEPEX waits
  on, causing the DXE core to dispatch Aspeed2500UefiDriver.

  Called whenever an ASPEED device is present, regardless of which display is
  primary. Keeping the onboard path alive is the point: it is the only console
  the BMC KVM can capture, and ConSplitter will drive it alongside a discrete GPU.
**/
STATIC
EFI_STATUS
OpenAspeedGopGate (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gAspeedGopDispatchGateGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    mRoute.GateOpened = TRUE;
  }

  DEBUG ((
    DEBUG_INFO,
    "VideoRoute: installed ASPEED GOP dispatch gate 794E15D9 (gate was %a) - %r\n",
    mRoute.GateWasShut ? "shut" : "already open",
    Status
    ));
  return Status;
}

EFI_STATUS
EFIAPI
VideoRouteEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  BOOLEAN     AspeedPresent;

  PostCode (POST_VIDEOROUTE_ENTRY);

  ZeroMem (&mRoute, sizeof (mRoute));
  mRoute.Revision     = OPEN_OOB_VIDEO_ROUTE_REVISION;
  mRoute.PrimaryIndex = -1;

  ScanPciDevices ();
  AssessAvailability ();
  ReadSetupPolicy ();
  SelectPath ();

  DEBUG ((
    DEBUG_INFO,
    "VideoRoute: %d display device(s), %d bridge(s), path=%d mode=%d primary=%d\n",
    mRoute.DeviceCount,
    (UINT32)mBridgeCount,
    mRoute.Path,
    mRoute.Mode,
    mRoute.PrimaryIndex
    ));

  //
  // Watch for GOP arrivals before opening the gate, so the driver we are about to
  // dispatch cannot install its GOP in the window before we are listening.
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnGopInstalled,
                  NULL,
                  &mGopEvent
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->RegisterProtocolNotify (
                    &gEfiGraphicsOutputProtocolGuid,
                    mGopEvent,
                    &mGopRegistration
                    );
    if (EFI_ERROR (Status)) {
      gBS->CloseEvent (mGopEvent);
      mGopEvent = NULL;
      DEBUG ((DEBUG_WARN, "VideoRoute: GOP notify registration failed - %r\n", Status));
    }
  }

  //
  // Default to onboard video: open the gate whenever the ASPEED is there.
  //
  AspeedPresent = (BOOLEAN)(FindDevice (TRUE, FALSE) >= 0);
  if (AspeedPresent || FeaturePcdGet (PcdVideoRouteForceAspeedGate)) {
    PostCode (mRoute.GateWasShut ? POST_VIDEOROUTE_GATE_OPENED : POST_VIDEOROUTE_GATE_ALREADY);
    OpenAspeedGopGate ();
  } else if (mRoute.DeviceCount == 0) {
    PostCode (POST_VIDEOROUTE_NO_DISPLAY);
    DEBUG ((DEBUG_WARN, "VideoRoute: no display controller found; serial console only\n"));
  } else {
    PostCode (POST_VIDEOROUTE_NO_ASPEED);
    DEBUG ((DEBUG_WARN, "VideoRoute: no onboard ASPEED VGA found; BMC KVM will not work\n"));
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gOpenOobVideoRouteProtocolGuid,
                &mRoute,
                NULL
                );
}
