# host-bios-image — patched host BIOS with the BMC push driver

This recipe produces the **host BIOS SPI image** (32 MiB) that the BMC writes to
the AMD host's BIOS flash, with our own DXE driver injected. It exists because
the stock AMI Redfish-Host-Interface (RHI / USB-RNDIS / Redfish) stack could not
be driven from the OpenBMC side (the AMI `UsbLan`/`UsbRndis`/credential drivers
never bring the link up — see `../../RHI-HANDOFF.md`). Instead of fighting that
closed driver chain, we inject a small, fully self-contained DXE module that does
the BMC exchange over a transport that already works: **KCS**.

## What gets injected

One PE32 with two halves, because the ROM patch replaces exactly one FFS file
and adding a second would disturb AMI's dispatch order:

### `SmbiosBmcPushDxe.c` — SMBIOS

- `MODULE_TYPE = DXE_DRIVER`, `DEPEX = TRUE` → the AMI Aptio DXE dispatcher runs
  it like any other driver.
- Hooks **EndOfDxe**, a 2 s periodic timer, and **ReadyToBoot**.
- Reads the host SMBIOS table via `EFI_SMBIOS_PROTOCOL` and pushes it to the BMC
  over **KCS** (LPC I/O 0xCA2/0xCA3) using **phosphor-ipmi-blobs**, blob id
  `"/smbios"` (NetFn 0x2E / Cmd 0x80, OEN CF C2 00).
- Skips the push when the BMC's stat metadata already reports a matching CRC.
- POST markers on port 0x80: `0x70`–`0x7B`.

### `BiosCfgOobDxe.c` — BIOS configuration

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

```sh
# from an edk2 checkout with BaseTools built and edksetup sourced:
#   the package lives at Platform/ASRockRack/X570D4I2TPkg/ in edk2-x570d4i2t
python3 gen-schema-header.py x570d4i2t-bios-knobs.xml BiosCfgOobSchema.h
build -p Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPush.dsc \
      -m Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf \
      -a X64 -b RELEASE -t GCC5
```

A shell-testable user app (`SmbiosBmcPushApp`) that exercises the blob wire
protocol from the UEFI shell lives alongside the DXE in `edk2-x570d4i2t`.

## BMC counterpart

`meta-x570d4i2t/recipes-phosphor/ipmi/asrock-ipmi-oem` — `biosconfigcommands.cpp`
(the 0xD3–0xD8 handlers) and `biosvarstore.cpp` (the type 2/3 formats). Keep the
payload layouts in `BiosCfgOobDxe.c` and `biosvarstore.hpp` in sync; they are the
same structs written twice.
