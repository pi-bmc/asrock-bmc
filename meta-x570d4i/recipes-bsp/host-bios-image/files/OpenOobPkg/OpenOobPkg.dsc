## @file
#  OpenOobPkg — the host-side EDK2 drivers we inject into the ASRock X570D4I-2T
#  AMI Aptio BIOS, replacing its proprietary out-of-band management stack.
#
#  This DSC does NOT describe a whole platform. It exists to produce valid,
#  individually injectable DXE drivers. The flash image is still the stock ASRock
#  ROM with AMI modules stripped and these grafted in; see tools/romsurgeon.py.
#
#  Built by recipes-bsp/host-bios-image against the pinned upstream EDK2. Every
#  library below is Base*/Uefi* on purpose: these drivers are dispatched by a
#  foreign (AMI) DXE core and can only rely on the interfaces it publishes, never
#  on a platform library or a populated PCD database.
#
#  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = OpenOobPkg
  PLATFORM_GUID                  = 3d8b1e07-52af-4c60-b9d3-71e8a4c05f26
  PLATFORM_VERSION               = 0.10
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/OpenOobPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  PciCf8Lib|MdePkg/Library/BasePciCf8Lib/BasePciCf8Lib.inf

  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

  # Null instances: an injected driver runs under a DXE core that never set up the
  # stack-cookie machinery these expect, so the checks must compile away rather
  # than call into absent support code.
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  StackCheckFailureHookLib|MdePkg/Library/StackCheckFailureHookLibNull/StackCheckFailureHookLibNull.inf

  # DebugLibNull unconditionally: DEBUG() output has nowhere to go inside the AMI
  # core, and a serial DebugLib would fight the firmware for the UART that the
  # AST2500 bridges to SOL. Progress is reported with POST codes instead, which the
  # BMC's port-80 snoop can read out-of-band.
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

  # Ours
  IpmiKcsLib|OpenOobPkg/Library/IpmiKcsLib/IpmiKcsLib.inf

  # Liveness/outcome recording. Not debug scaffolding — on this board it is the
  # only trustworthy way to find out what an injected driver did, because the
  # BMC's IPMI journal is irreproducible across boots and port-80 writes from an
  # injected DXE driver do not reach the snoop at all.
  OobTelemetryLib|OpenOobPkg/Library/OobTelemetryLib/OobTelemetryLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x00

[PcdsFeatureFlag]
  # Onboard ASPEED stays primary. It is the only display the BMC KVM can capture
  # and the only one whose driver we know ships in this firmware image.
  gOpenOobPkgTokenSpaceGuid.PcdVideoRoutePreferDiscreteGpu|FALSE
  gOpenOobPkgTokenSpaceGuid.PcdVideoRouteForceAspeedGate|FALSE

[Components]
  #
  # Shared transport.
  #
  OpenOobPkg/Library/IpmiKcsLib/IpmiKcsLib.inf
  OpenOobPkg/Library/OobTelemetryLib/OobTelemetryLib.inf

  #
  # SmbiosBmcPushDxe — SMBIOS push over phosphor-ipmi-blobs. This is the one
  # module the proven injection path REPLACES the AMI SendInfoBmcIpmiDxe FFS slot
  # (9DF02DFD) with, so it lands in the flash image without needing free space.
  #
  OpenOobPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf

  #
  # Added as new FFS files by tools/romsurgeon.py. Unlike the module above these
  # do not displace an AMI slot, so each needs free space in the dispatched
  # firmware volume — which means a strip profile has to run first.
  #
  OpenOobPkg/BiosCfgOobDxe/BiosCfgOobDxe.inf # split out of SmbiosBmcPushDxe
  OpenOobPkg/OobIpmiDxe/OobIpmiDxe.inf       # displaces KcsControlDxe / DxeIpmiBmcInitialize
  OpenOobPkg/VideoRouteDxe/VideoRouteDxe.inf # new: per-boot video path selection

  #
  # Instrumentation, not product. OobProbeDxe exists to establish whether the AMI
  # DXE core dispatches anything we inject at all — a question no signal we had
  # (POST codes, IPMI counts) could answer. It writes an NV variable we can read
  # straight out of the SPI flash afterwards. Drop it once dispatch is proven.
  #
  OpenOobPkg/OobProbeDxe/OobProbeDxe.inf
  OpenOobPkg/OobProbeDxe/OobProbe2Dxe.inf   # position control; see its INF

[BuildOptions]
  # -fno-jump-tables: an injected driver is relocated by a foreign DXE core.
  # GenFw handles the absolute relocs a jump table produces, but keeping them out
  # has proven more robust on this board.
  GCC:*_*_X64_CC_FLAGS = -fno-jump-tables -mno-red-zone
