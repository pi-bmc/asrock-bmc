# host-bios-image — patched host BIOS with SmbiosBmcPushDxe

This recipe produces the **host BIOS SPI image** (32 MiB) that the BMC writes to
the AMD host's BIOS flash, with our own DXE driver injected. It exists because
the stock AMI Redfish-Host-Interface (RHI / USB-RNDIS / Redfish) stack could not
be driven from the OpenBMC side (the AMI `UsbLan`/`UsbRndis`/credential drivers
never bring the link up — see `../../RHI-HANDOFF.md`). Instead of fighting that
closed driver chain, we inject a small, fully self-contained DXE module that does
the BMC exchange over a transport that already works: **KCS + phosphor-ipmi-blobs**.

## What gets injected

`SmbiosBmcPushDxe` (`SmbiosBmcPushDxe.c` / `.inf`, prebuilt `SmbiosBmcPushDxe.efi`):

- `MODULE_TYPE = DXE_DRIVER`, `DEPEX = TRUE` → the AMI Aptio DXE dispatcher runs
  it like any other driver.
- Hooks **EndOfDxe** (after all SMBIOS producers, *before* BDS/OS — the pre-boot
  point), plus a 2 s periodic timer (covers sitting at the boot manager) and
  **ReadyToBoot** (belt-and-suspenders).
- Reads the host SMBIOS table via `EFI_SMBIOS_PROTOCOL` and pushes it to the BMC
  over **KCS** (LPC I/O 0xCA2/0xCA3) using the OpenBMC **phosphor-ipmi-blobs**
  protocol, blob id `"/smbios"` (NetFn 0x2E / Cmd 0x80, OEN CF C2 00).
- Port-0x80 POST markers trace progress: `0x58` entry, `0x5B` smbios-found,
  `0xB8` push-ok, `0xB9` push-fail, `0xBA` ReadyToBoot, `0xBC` EndOfDxe.

No RNDIS, no Redfish, no AMI RHI — only KCS, which the BIOS and BMC already speak.

## How injection works (and why it stays 32 MiB)

`patch_rom.py` **replaces** the AMI `SendInfoBmcIpmiDxe` module
(FILE_GUID `9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24`) inside the **dispatched** upper
`FVMAIN_COMPACT` (ROM offset `0x1AC1000`, 4 MiB). That module is already
dispatched by the DXE core for BMC info exchange, so replacing it does not
disturb dispatch order or per-module integrity expectations the way *adding* a
brand-new FFS would (the lower 8 MiB `FVMAIN` copy at `0x069F000` is **not**
dispatched — do not patch it).

The inner FV is recompressed (LZMA) to absorb the size delta, so the output ROM
is **exactly 33554432 bytes (32 MiB)** — the recipe asserts this and fails the
build otherwise. The image is never grown past the SPI part size.

## Rebuilding `SmbiosBmcPushDxe.efi` (the "minimal EDK2 build")

The committed `.efi` (~4 KB, MdePkg-only) is deterministic. To rebuild after
editing `SmbiosBmcPushDxe.c`, use any EDK2 tree:

```sh
# from an edk2 checkout with BaseTools built and edksetup sourced:
#   the package lives at Platform/ASRockRack/X570D4I2TPkg/ in edk2-x570d4i2t
build -p Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPush.dsc \
      -m Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf \
      -a X64 -b RELEASE -t GCC5
# then copy Build/.../X64/SmbiosBmcPushDxe.efi over files/SmbiosBmcPushDxe.efi
```

A shell-testable user app (`SmbiosBmcPushApp`) that exercises the exact blob wire
protocol from the UEFI shell lives alongside the DXE in `edk2-x570d4i2t`.

## Flashing the result

The recipe deploys `host-bios-<machine>-2.59C-smbiospush.rom` (and a
`host-bios-<machine>.rom` symlink) to `DEPLOY_DIR_IMAGE`. Write it to the host
BIOS SPI by either:

- the BMC host-BIOS update path — Redfish `UpdateService` image →
  `bios-update.sh` → GPIOJ1 SPI mux + `spi-aspeed-smc` (mtd); or
- an external SPI/EEPROM programmer.

## Source of truth

Patcher and DXE module are vendored from `edk2-x570d4i2t`:
`patch_rom.py`, `Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/`. Keep them in
sync when iterating there.
