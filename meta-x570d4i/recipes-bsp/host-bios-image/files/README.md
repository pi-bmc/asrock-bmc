# host-bios-image — patched host BIOS with the BMC push driver

This recipe produces the **host BIOS SPI image** (32 MiB) that the BMC writes to
the AMD host's BIOS flash, with our own DXE driver injected. It exists because
the stock AMI Redfish-Host-Interface (RHI / USB-RNDIS / Redfish) stack could not
be driven from the OpenBMC side (the AMI `UsbLan`/`UsbRndis`/credential drivers
never bring the link up — see `../../RHI-HANDOFF.md`). Instead of fighting that
closed driver chain, we inject a small, fully self-contained DXE module that does
the BMC exchange over a transport that already works: **KCS**.

## Layout

Sources are an EDK2 package, one subdirectory per driver, built together from
`OpenOobPkg/OpenOobPkg.dsc`:

```
OpenOobPkg/
  OpenOobPkg.dec              package GUIDs, PCDs, protocol declarations
  OpenOobPkg.dsc              builds every driver below for X64
  Include/Library/            IpmiKcsLib.h, OobIntelOemLib.h, OobTelemetryLib.h
  Include/Protocol/           OobIpmiTransport.h, OobVideoRoute.h
  Library/IpmiKcsLib/         THE IPMI-over-KCS transport — one implementation
  Library/OobIntelOemLib/     host side of openbmc/intel-ipmi-oem (see below)
  Library/OobTelemetryLib/    liveness/outcome recording to an NV variable
  SmbiosBmcPushDxe/           SMBIOS push only          (displaces an AMI slot)
  BiosCfgOobDxe/              BIOS-config OOB producer  (new FFS)
  OobIpmiDxe/                 transport protocol + AMI compat marker  (new FFS)
  VideoRouteDxe/              per-boot video path selection            (new FFS)
  OobSetupDefaultsDxe/        reasserts Above 4G Decoding              (new FFS)
tools/
  romsurgeon.py               strip AMI modules / graft in new FFS files
  uefifv.py                   FV / FFS / LZMA primitives
  profiles/inject-only.yaml   minimal removal to make room, then inject
  profiles/strip-oob.yaml     full OOB removal, tiered by confidence
```

One driver per subdirectory, and no cross-module coupling. `SmbiosBmcPushDxe` and
`BiosCfgOobDxe` used to be a single PE32 in which `SmbiosBmcPushDxe.c` defined the
`KcsTxn`/`Post` primitives that `BiosCfgOobDxe.c` borrowed *and* drove its
`Init`/`Run`/`IsDone` lifecycle from its own event hooks. Both directions are gone:
the transport is `IpmiKcsLib` (`IpmiKcsRawTransaction`, `OobPostCode`) and each
driver arms its own EndOfDxe / timer / ReadyToBoot events.

### Two injection mechanisms, and why it matters

- **`SmbiosBmcPushDxe` REPLACES** the AMI `SendInfoBmcIpmiDxe` FFS slot
  (`9DF02DFD`) via UEFIReplace. This is the path that has actually run on
  hardware, and it needs no free space. Because the slot is *reused* rather than
  vacated, neither profile may list `9DF02DFD` — by the time romsurgeon runs, that
  GUID is *our* driver. Note UEFIReplace swaps only the PE32 and DEPEX sections,
  so the FFS keeps AMI's UI name: inventories still show `SendInfoBmcIpmiDxe`.
- **The other three are ADDED** as new FFS files by `tools/romsurgeon.py`. The
  dispatched firmware volume ships with only **2,416 bytes free**, so this
  requires removing something first — hence the profiles.

### Consequence of the split you need to know about

`BiosCfgOobDxe` is no longer a passenger in the replaced slot, so **with
`HOST_BIOS_STRIP_OOB = "0"` (the default) it does not reach the flash**, and
`/redfish/v1/Systems/system/Bios` reverts to a defaults listing. `do_compile`
emits a `bbwarn` saying so rather than letting it pass silently.

To include it, pick a profile with `HOST_BIOS_OOB_PROFILE`:

| Profile | Removes | Frees | Use when |
|---|---|---|---|
| `inject-only` (default) | AMI's Redfish/REST stack only — 11 leaf modules, zero dependents | ~384 KiB | You want our drivers and nothing else disturbed |
| `strip-oob` | the above plus PLDM, the SMM BMC island and assorted BMC feature drivers — 27 modules | ~788 KiB | You want the full OOB replacement |

Both are verified against 2.59F to produce a structurally valid, re-parseable
32 MiB ROM with all changes confined to the dispatched firmware volume. **Neither
has been flashed or booted.** A stranded DEPEX fails silently — the dependent
simply never dispatches — so do not enable either unless you can reflash over the
BIOS SPI mux. Per-module reasoning is in `edk2-x570d4i2t/docs/STRIP-PLAN.md`.

## The driver in the replaced AMI slot

### `SmbiosBmcPushDxe/` — SMBIOS

- `MODULE_TYPE = DXE_DRIVER`, `DEPEX = TRUE` → the AMI Aptio DXE dispatcher runs
  it like any other driver.
- Hooks **EndOfDxe**, a 2 s periodic timer, and **ReadyToBoot**.
- Reads the host SMBIOS table via `EFI_SMBIOS_PROTOCOL` and pushes it to the BMC
  over **KCS** (LPC I/O 0xCA2/0xCA3) using **phosphor-ipmi-blobs**, blob id
  `"/smbios"` (NetFn 0x2E / Cmd 0x80, OEN CF C2 00).
- Skips the push when the BMC's stat metadata already reports a matching CRC.
- POST markers on port 0x80: `0x70`–`0x7B`.

## The standalone drivers (added as new FFS, not replacing an AMI slot)

### `BiosCfgOobDxe/` — BIOS configuration

The host side of the OpenBMC "BIOS OOB config" command set (NetFn 0x30, cmds
0xD3–0xD6), i.e. the mirror image of
[`intel-ipmi-oem/src/biosconfigcommands.cpp`][intel]. It is what makes
`/redfish/v1/Systems/system/Bios` real rather than a defaults listing.

[intel]: https://github.com/openbmc/intel-ipmi-oem/blob/master/src/biosconfigcommands.cpp

| Payload | Direction | Contents |
|---|---|---|
| type 1 | host → BMC | knob schema XML, LZMA "alone" encoded |
| type 2 | host → BMC | `"ASVS"` — the backing UEFI variables, as read |
| type 3 | BMC → host | `"ASPS"` — staged Redfish writes to apply |

Per boot it: announces capabilities (0xD3); fetches and applies staged settings
with `gRT->SetVariable`, verifying the readback and cold-resetting **once** if
anything changed; sends the schema *only if* the BMC does not already report
that exact CRC32; then sends a fresh snapshot of the live varstores read with
`gRT->GetVariable`. POST markers `0x60`–`0x6D`.

Steady state is therefore two short `GetPayload` commands plus a ~2 KB snapshot,
instead of a 287-chunk KCS transfer on every POST. Note this gate can only live
here: the BIOS ignores IPMI completion codes, so a BMC-side refusal could never
stop a transfer, only skip the parse afterwards.

### Why this driver has to supply the schema at all

**The stock ASRock BIOS contains no BIOS-config producer.** Decompiling both
2.59C and 2.59F (walk every FV, decompress every LZMA/GUID-defined section,
disassemble all 871 modules) shows:

- stock `SendInfoBmcIpmiDxe` is a 3 KB module that never issues cmd 0xD3 or
  0xD5 and references no KCS port;
- the strings any XML generator would need — `biosknobs`, `setupType`,
  `checkbox`, `varstoreIndex` — appear **nowhere** in either 32 MiB image,
  compressed or not.

What had been feeding the BMC was an **earlier build of this very driver**, with
a 13719-byte LZMA blob baked in as a constant and pushed verbatim every POST.
That explains every oddity the BMC side had to work around: every knob's
`CurrentVal` equal to its own default, a payload whose CRC32 never moved, and
`CSM009`/`SLOTOPROM022` both describing Setup byte `0x01BA` yet reporting
different "current" values. It was a defaults template, not a reading.

The schema half is unchanged in kind — a knob registry genuinely *is* static
firmware metadata — but it is now a reviewable XML file in this directory rather
than an opaque constant, and the **values** come from the real varstores.

### `Library/OobIntelOemLib/` — host side of intel-ipmi-oem

Not a driver: a leaf library over `IpmiKcsLib` that any injected module can link
to issue one command. `openbmc/intel-ipmi-oem` is a BMC-side ipmid provider, so
every command it registers is one the *host firmware* is expected to send. This
is the missing other half, with request and response layouts transcribed from
its handler signatures and each function citing the source it came from.

Twenty commands, chosen by one rule: the BIOS actually originates them.

| NetFn | Commands |
|---|---|
| 0x30 | `01` GetBmcVersionString, `02` RestoreConfiguration, `27` GetOemDeviceInfo, `44` SendEmbeddedFwUpdStatus, `57` SetFaultIndication, `66` GetBufferSize, `8E`/`8F` Set/GetDimmOffset, `93` ReadBaseBoardProductId, `9A`/`9B` Get/SetProcessorErrConfig, `B3`/`B4` Get/SetSecurityMode, `D7`/`D8` Set/GetBiosPwdHash, `E5`/`ED` Get/SetNmiSource, `EA`/`EB` Set/GetEfiBootOptions |
| 0x04 | `02` Platform Event |
| 0x0A | `44` Add SEL Entry |

Most of intel-ipmi-oem's other 79 handlers serve a *remote* client — Get Chassis
Status, the SEL and FRU readers, fan control, the firmware-update state machine.
Those have no host side, so a "reversed" version would be inventing traffic
rather than mirroring a protocol. `D3`–`D6` are host-initiated but live in
`BiosCfgOobDxe`, which already implements the chunked SetPayload state machine.

Two marshalling details from ipmid shape the whole file and are easy to get
wrong: sub-byte response fields are packed LSB-first and **coalesced**, so
`RspType<bool,bool,bool,uint5_t,...>` is one byte and not four (Get Processor
Error Config's fourteen fields are seven wire bytes); and `std::string` is
UCSD-Pascal, a length byte then raw characters with **no NUL**.

**MDR II (NetFn 0x3E, `30`–`3D`) is deliberately absent.** It looks like the
natural host-side SMBIOS push and it cannot work here. Those commands carry no
payload — `mdr2SendDataBlock` takes only agent, lock handle, offset, length and
checksum — because the bytes travel through a shared-memory aperture the BMC
reads via `/dev/vgasharedmem`. This board has no such aperture: the device node
does not exist, the aspeed VGA shared-memory driver is not built, and there is
no device-tree node for it. Nor is there a responder, since `smbios-mdr`
registers no ipmid handlers at all. `SmbiosBmcPushDxe` uses phosphor-ipmi-blobs
instead, which carries the table inline over KCS — the right transport for this
hardware, not a workaround.

The one caller so far is `BiosCfgOobDxe`, which emits a Platform Event
(sensor type `0Fh` System Firmware Progress, offset `00h` System Firmware Error,
OEM reason in event data 2) on each of its three transfer-failure paths. Those
previously left only a POST code, which is gone the moment the board moves on.
It is **not** called from the KCS-not-ready path, where reporting the failure
would fail the same way.

`GetBufferSize` is provided but not wired into the transfer loop on purpose.
The upstream handler returns a hardcoded 63/4, with its own comment admitting
the host's limit is unknowable from the BMC side — clamping our proven 48-byte
chunk down to that number would be a regression justified by a constant that
does not describe this BMC.

### `OobIpmiDxe/` — IPMI transport, and the removal enabler

Publishes `gOpenOobIpmiTransportProtocol`: a serialised IPMI-over-KCS channel
built on `Library/IpmiKcsLib`, so several injected drivers can share one KCS
channel instead of racing the firmware's own IPMI traffic. It installs the
protocol even when the BMC does not answer — the BMC is frequently still booting
during DXE, and refusing to install would permanently deny the protocol to
consumers that run at ReadyToBoot, by which time it is invariably up. Liveness is
reported in `BmcResponsive` instead.

It also installs AMI's DXE IPMI transport GUID `4A1D0E66-5271-4E22-83FE-90921B748213`
as a bare presence marker. That one install is what makes AMI's
`DxeIpmiBmcInitialize` removable at all: it is the sole producer of that GUID, and
**15 modules DEPEX on it** — three of which are not part of the OOB stack
(`DefaultOverride`, `PcieInfoJudgeDxe`, `SerialMuxControl`).

The marker's interface pointer is **NULL**, i.e. presence only. That fully
satisfies `DefaultOverride`, which never dereferences it and uses the DEPEX purely
to order itself after BMC init. It does **not** satisfy `PcieInfoJudgeDxe` or
`SerialMuxControl`, which really do call through AMI's transport vtable — a layout
we have not reproduced. `strip-oob.yaml` records that as a hard pairing: strip
those two alongside `DxeIpmiBmcInitialize`, or implement the vtable first.

### `VideoRouteDxe/` — per-boot video path selection

`DEPEX = TRUE`, so it runs in the first dispatch round, before `PciBus` publishes
`EFI_PCI_IO_PROTOCOL` and starts launching option ROMs.

It enumerates PCI class-0x03 devices with raw CF8/CFC config cycles (MMIO ECAM is
not usable this early in a foreign DXE core, whereas CF8/CFC always is), walks the
PCI-to-PCI bridges to see which ones have Bridge Control VGA Enable set, and
classifies each display by whether anything is **known** to drive it:

- the onboard AST2500 (`1A03:2000`) is always known-good — this ROM demonstrably
  contains a native ASPEED GOP driver and an ASPEED legacy VBIOS;
- a discrete GPU is known-good only if it carries its own expansion ROM **and**
  sits on the platform's VGA-routed bridge path.

**It defaults to the onboard video.** `PcdVideoRoutePreferDiscreteGpu` is FALSE,
so the ASPEED stays primary even when a known-good GPU is present — it is the only
display the BMC KVM can capture. Crucially the ASPEED GOP gate is opened whenever
an ASPEED device is present, *regardless* of which display is primary, so the KVM
path survives even on a GPU-primary boot (ConSplitter drives both).

The gate itself: `Aspeed2500UefiDriver` (`D88A9618`) has a single-GUID DEPEX,
`794E15D9-BF1B-4568-99AC-DCE207C022E4`. AMI's `SlotOpRomDXE` is its only producer
anywhere in the ROM and installs it only when `Setup[433] == 0` (CSM disabled) or
`Setup[442] == 1` (Video OpROM Policy = UEFI only). The shipped ROM has 433=2 and
442=2, so **on a stock board the ASPEED UEFI GOP driver is never dispatched** and
the only video path is the legacy VBIOS behind CsmVideo's INT10h shim. Installing
that GUID makes the DXE core dispatch the real driver — no NVRAM write, no reset.

Progress is reported on POST port 0x80, which the BMC's port-80 snoop reads
out-of-band — the only feedback channel that works when there is no display:

| Code | Meaning |
|---|---|
| `0xE0` | entry |
| `0xE1` | no display controller found at all |
| `0xE2` | ASPEED GOP gate opened (stock policy had it shut) |
| `0xE3` | gate opened redundantly (stock policy would have too) |
| `0xE4` | display present but no onboard ASPEED — KVM will not work |
| `0xE5` | first `EFI_GRAPHICS_OUTPUT_PROTOCOL` instance observed |

`0xE5` is the one that matters: a protocol notify counts GOP arrivals, which
distinguishes "we installed a marker" from "a GOP driver actually started".

**This is not expected to fix the blank KVM.** The equivalent test via NVRAM
(writing `CSM=0` and `VideoOpROM=1` straight to flash) was done on 2026-07-27 and
video stayed black with `SCU50/54/58` still reading zero. So either the GOP driver
does dispatch and the black framebuffer has a later cause, or the decompiled gate
condition is incomplete. What this driver buys is a *cleaner discriminator*: it
changes exactly one thing instead of disabling the whole CSM path. See
`edk2-x570d4i2t/docs/VIDEO-ROUTING.md`.

## Schema provenance and regeneration

`x570d4i2t-bios-knobs.xml` (183877 B, 431 knobs) is the schema, extracted from
the driver currently in the flash. `gen-schema-header.py` compresses it and
emits `BiosCfgOobSchema.h` at build time; the result is byte-identical to the
payload observed on the wire (13719 B, CRC32 `0x66BCCFCC`), which is what
confirms the extraction was faithful.

`BiosCfgOobVarstores.h` maps `varstoreIndex` → (variable name, vendor GUID) and
is generated from the HII IFR decompilation of the stock image
(`edk2-x570d4i2t/efi-vars.json`). 425 of the 431 knobs agree exactly with the
XML on `(varstoreIndex, offset, size)`.

**The GUID is load-bearing**: varstores 1 and 13 are *both* named `Setup` and
differ only by vendor GUID. That is why reading the AMI NVAR store out of the
SPI flash by name (`asrock-bios-nvram`) cannot always resolve a varstore, while
the firmware — which indexes them directly — always can.

## How injection works (and why it stays 32 MiB)

The recipe **replaces** the AMI `SendInfoBmcIpmiDxe` module (FILE_GUID
`9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24`) with LongSoft `UEFIReplace`, patching
the PE32 (section type 0x10) and DXE dependency (0x13) sections in all duplicate
firmware volume copies. That module is already dispatched by the DXE core for
BMC info exchange, so replacing it does not disturb dispatch order or per-module
integrity expectations the way *adding* a brand-new FFS would. (The lower 8 MiB
`FVMAIN` copy at `0x069F000` is not dispatched; the live one is `FVMAIN_COMPACT`
at `0x1AC1000`.)

The inner FV is recompressed (LZMA) to absorb the size delta, so the output ROM
is **exactly 33554432 bytes (32 MiB)** — the recipe asserts this and fails the
build otherwise.

## Flashing the result

The recipe deploys `host-bios-<machine>-2.59C-smbiospush.rom` (and a
`host-bios-<machine>.rom` symlink) to `DEPLOY_DIR_IMAGE`. Write it to the host
BIOS SPI by either:

- the BMC host-BIOS update path — Redfish `UpdateService` image →
  `bios-update.sh` → GPIOJ1 SPI mux + `spi-aspeed-smc` (mtd); or
- an external SPI/EEPROM programmer.

The SPI mux is only safe to move while the host is **powered off** — see
`asrock-bios-flash-sync.sh` on the BMC, which refuses to run otherwise.

## Rebuilding the driver standalone

Point an EDK2 workspace at a copy of `OpenOobPkg/` and build the DSC. The schema
header is a generated build input, so it has to exist before the build runs.

```sh
export WORKSPACE=$PWD
export PACKAGES_PATH=<edk2>:$PWD
export EDK_TOOLS_PATH=<edk2>/BaseTools
source <edk2>/edksetup.sh BaseTools

cp -R <this-dir>/OpenOobPkg ./OpenOobPkg
python3 <this-dir>/gen-schema-header.py \
        <this-dir>/x570d4i2t-bios-knobs.xml \
        ./OpenOobPkg/BiosCfgOobDxe/BiosCfgOobSchema.h

build -p OpenOobPkg/OpenOobPkg.dsc -a X64 -b RELEASE -t GCC5
# -> Build/OpenOobPkg/RELEASE_GCC5/X64/{SmbiosBmcPushDxe,OobIpmiDxe,VideoRouteDxe}.efi
```

Add `-m OpenOobPkg/<Driver>/<Driver>.inf` to build just one module.

Two BaseTools notes if you are building against a fresh edk2 checkout: modern GCC
trips over BaseTools' own `-Werror` (pass `EXTRA_OPTFLAGS=-Wno-error`), and
`MdePkg` needs `git submodule update --init MdePkg/Library/MipiSysTLib/mipisyst`.
`BrotliCompress` fails for a missing submodule and nothing here uses it.

To graft a driver into a ROM without a full Yocto build:

```sh
python3 tools/romsurgeon.py inspect --rom <rom>          # list the live DXE volume
python3 tools/romsurgeon.py apply --rom <rom> \
        --profile tools/profiles/strip-oob.yaml \
        --efi-dir Build/OpenOobPkg/RELEASE_GCC5/X64 --dry-run
```

A shell-testable user app (`SmbiosBmcPushApp`) that exercises the blob wire
protocol from the UEFI shell lives alongside the DXE in `edk2-x570d4i2t`.

## BMC counterpart

`meta-x570d4i/recipes-phosphor/ipmi/asrock-ipmi-oem` — `biosconfigcommands.cpp`
(the 0xD3–0xD8 handlers) and `biosvarstore.cpp` (the type 2/3 formats). Keep the
payload layouts in `BiosCfgOobDxe.c` and `biosvarstore.hpp` in sync; they are the
same structs written twice.
