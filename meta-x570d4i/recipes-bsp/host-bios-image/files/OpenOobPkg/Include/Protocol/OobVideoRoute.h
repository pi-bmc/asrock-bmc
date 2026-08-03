/** @file
  OpenOob video-route protocol — reports which video paths this boot found, which
  of them are KNOWN to be usable, and which one was selected.

  Installed by VideoRouteDxe early in DXE, before PciBus connects display
  controllers, so later drivers (and our BMC push) can observe and act on the
  decision rather than re-deriving it.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPEN_OOB_VIDEO_ROUTE_H_
#define OPEN_OOB_VIDEO_ROUTE_H_

#define OPEN_OOB_VIDEO_ROUTE_PROTOCOL_GUID \
  { 0x8b5f0d2a, 0x1c74, 0x4a96, { 0xb3, 0x8e, 0x25, 0xf1, 0x60, 0xcd, 0x47, 0x39 }}

///
/// ASPEED AST2500 onboard VGA. This is the controller the BMC's video-capture
/// engine taps, so it is the only path that can produce a KVM stream.
///
#define OOB_VIDEO_VENDOR_ASPEED   0x1A03
#define OOB_VIDEO_DEVICE_AST2500  0x2000

typedef enum {
  ///
  /// No display controller found at all. Headless; console is serial (SOL) only.
  ///
  OobVideoPathNone = 0,
  ///
  /// Onboard ASPEED VGA drives the console. Required for BMC KVM, and the
  /// default whenever an ASPEED device is present.
  ///
  OobVideoPathOnboardAspeed,
  ///
  /// A discrete PCIe GPU drives the console. The BMC KVM cannot see this.
  ///
  OobVideoPathDiscreteGpu,
  OobVideoPathMax
} OOB_VIDEO_PATH;

typedef enum {
  ///
  /// Console via a UEFI GOP driver.
  ///
  OobVideoModeUefiGop = 0,
  ///
  /// Console via a legacy VBIOS option ROM under CSM.
  ///
  OobVideoModeLegacyVbios,
  OobVideoModeMax
} OOB_VIDEO_MODE;

///
/// How confident we are that a given device can actually produce a console.
///
/// The distinction that matters is between "a display device exists here" and "we
/// know something can drive it". Selecting a path on the former is how a headless
/// board ends up with no console at all.
///
typedef enum {
  ///
  /// Not assessed.
  ///
  OobVideoAvailUnknown = 0,
  ///
  /// Positive evidence a driver exists for this device. For the onboard ASPEED
  /// that evidence is build-time: this ROM contains a native ASPEED GOP driver
  /// (Aspeed2500UefiDriver) and an ASPEED legacy VBIOS. For a discrete card it
  /// means the card carries its own expansion ROM and sits on the bridge path the
  /// platform marked VGA-routed.
  ///
  OobVideoAvailKnownGood,
  ///
  /// The device is present, but nothing here indicates a driver for it: no
  /// expansion ROM of its own, and no driver for it in this firmware image.
  /// Choosing it would be a guess.
  ///
  OobVideoAvailNoKnownDriver,
  ///
  /// Present and possibly driveable, but the platform has not routed legacy VGA
  /// decode to it, so it is not the elected primary display.
  ///
  OobVideoAvailNotVgaRouted,
  OobVideoAvailMax
} OOB_VIDEO_AVAILABILITY;

typedef struct {
  UINT16                    Segment;
  UINT8                     Bus;
  UINT8                     Device;
  UINT8                     Function;
  UINT16                    VendorId;
  UINT16                    DeviceId;
  ///
  /// PCI class 0x03 subclass and programming interface. Subclass 0x00 with
  /// ProgIf 0x00 is a VGA-compatible controller.
  ///
  UINT8                     SubClass;
  UINT8                     ProgIf;
  ///
  /// PCI command register. Bits 0/1 (I/O and memory decode) indicate the
  /// platform has already initialised this device.
  ///
  UINT16                    Command;
  ///
  /// TRUE when the device implements an expansion ROM BAR (offset 0x30 has
  /// writable address bits). This reports only that a ROM BAR exists — its
  /// contents cannot be read this early, before BAR assignment.
  ///
  BOOLEAN                   HasExpansionRom;
  ///
  /// TRUE when every PCI-to-PCI bridge whose bus range contains this device has
  /// VGA Enable set (bridge control offset 0x3E bit 3), i.e. the platform routed
  /// legacy VGA decode here. Devices on bus 0 need no bridge and are always
  /// considered routed.
  ///
  BOOLEAN                   OnVgaRoutedPath;
  ///
  /// TRUE when this is the onboard AST2500.
  ///
  BOOLEAN                   IsOnboardBmcVga;
  OOB_VIDEO_AVAILABILITY    Availability;
} OOB_VIDEO_DEVICE;

#define OOB_VIDEO_MAX_DEVICES  8

typedef struct {
  UINT32              Revision;
  OOB_VIDEO_PATH      Path;
  OOB_VIDEO_MODE      Mode;
  ///
  /// Index into Devices[] of the controller chosen to own the console, or -1
  /// when Path is OobVideoPathNone.
  ///
  INT32               PrimaryIndex;
  UINT32              DeviceCount;
  OOB_VIDEO_DEVICE    Devices[OOB_VIDEO_MAX_DEVICES];

  ///
  /// Raw video-policy bytes read from the AMI `Setup` variable, valid only when
  /// SetupPolicyValid is TRUE.
  ///
  BOOLEAN             SetupPolicyValid;
  UINT8               SetupCsm;         ///< CSM005 @433: 0=Disabled 1=Enabled 2=Custom
  UINT8               SetupBootFilter;  ///< CSM006 @439: 0=UEFI+Legacy 1=Legacy 2=UEFI
  UINT8               SetupVideoOpRom;  ///< CSM009 @442: 0=none 1=UEFI only 2=Legacy only

  ///
  /// TRUE when the stock firmware policy would NOT have opened the ASPEED GOP
  /// dispatch gate on its own, i.e. our install is what makes the native GOP
  /// driver run this boot.
  ///
  BOOLEAN             GateWasShut;
  ///
  /// TRUE once we have installed the ASPEED GOP dispatch gate GUID.
  ///
  BOOLEAN             GateOpened;
  ///
  /// Number of EFI_GRAPHICS_OUTPUT_PROTOCOL instances observed after the gate was
  /// opened. Populated asynchronously by a protocol notify, so it is 0 at the
  /// moment this protocol is installed and grows as GOP producers start. This is
  /// the only positive confirmation available on a board with no display.
  ///
  UINT32              GopInstancesSeen;
} OPEN_OOB_VIDEO_ROUTE_PROTOCOL;

#define OPEN_OOB_VIDEO_ROUTE_REVISION  0x00010001

extern EFI_GUID  gOpenOobVideoRouteProtocolGuid;

#endif // OPEN_OOB_VIDEO_ROUTE_H_
