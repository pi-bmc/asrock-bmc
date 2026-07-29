/** @file
  Varstore table for BiosCfgOobDxe - the UEFI variables that back the BIOS
  Setup knobs described by x570d4i2t-bios-knobs.xml.

  GENERATED, DO NOT EDIT BY HAND. Source: the HII IFR decompilation of the
  stock ASRock X570D4I-2T 2.59C image (edk2-x570d4i2t/efi-vars.json, the
  "Variables" array). The XML's varstoreIndex is the IFR VarStoreId, so the
  two line up 1:1 - verified against 431 knobs, 425 of which agree exactly on
  (varstoreIndex, offset, size).

  The GUID matters and cannot be inferred from the name: varstore 1 and
  varstore 13 are BOTH named "Setup" and are distinguished only by GUID. Any
  name-only lookup (such as reading the AMI NVAR store out of the SPI flash)
  cannot tell them apart, which is precisely why the values are read here,
  in the firmware, through gRT->GetVariable.

  Copyright (c) 2024, ASRockRack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef BIOS_CFG_OOB_VARSTORES_H_
#define BIOS_CFG_OOB_VARSTORES_H_

#include <Uefi.h>

typedef struct {
  UINT8           Index;      /* XML varstoreIndex / IFR VarStoreId */
  CONST CHAR16    *Name;      /* UEFI variable name */
  EFI_GUID        Guid;       /* vendor GUID - NOT derivable from Name */
} OOB_VARSTORE;

STATIC CONST OOB_VARSTORE  mOobVarstores[] = {
  {  0, L"PlatformLang",          { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } } },   /*   1 knobs, span    2 B */
  {  1, L"Setup",                 { 0xEC87D643, 0xEBA4, 0x4BB5, { 0xA1, 0xE5, 0x3F, 0x3E, 0x36, 0xB2, 0x0D, 0xA9 } } },   /* 308 knobs, span  509 B */
  {  2, L"PCI_COMMON",            { 0xACA9F304, 0x21E2, 0x4852, { 0x98, 0x75, 0x7F, 0xF4, 0x88, 0x1D, 0x67, 0xA5 } } },   /*   7 knobs, span    7 B */
  {  3, L"PNP0501_0_NV",          { 0x560BF58A, 0x1E0D, 0x4D7E, { 0x95, 0x3F, 0x29, 0x80, 0xA2, 0x61, 0xE0, 0x31 } } },   /*   2 knobs, span    2 B */
  {  4, L"PNP0501_1_NV",          { 0x560BF58A, 0x1E0D, 0x4D7E, { 0x95, 0x3F, 0x29, 0x80, 0xA2, 0x61, 0xE0, 0x31 } } },   /*   2 knobs, span    2 B */
  {  5, L"SioSetupData",          { 0x6B0CC1BC, 0x910F, 0x411E, { 0xB6, 0xCB, 0x0E, 0x31, 0x4D, 0x0B, 0xB8, 0xC1 } } },   /*   1 knobs, span    1 B */
  {  6, L"UsbSupport",            { 0xEC87D643, 0xEBA4, 0x4BB5, { 0xA1, 0xE5, 0x3F, 0x3E, 0x36, 0xB2, 0x0D, 0xA9 } } },   /*  45 knobs, span   48 B */
  {  7, L"NetworkStackVar",       { 0xD1405D16, 0x7AFC, 0x4695, { 0xBB, 0x12, 0x41, 0x45, 0x9D, 0x36, 0x95, 0xA2 } } },   /*   7 knobs, span    8 B */
  {  8, L"AMITSESetup",           { 0xC811FA38, 0x42C8, 0x4579, { 0xA9, 0xBB, 0x60, 0xE9, 0x4E, 0xDD, 0xFB, 0x34 } } },   /*   6 knobs, span   66 B */
  {  9, L"SecureBootSetup",       { 0x7B59104A, 0xC00D, 0x4158, { 0x87, 0xFF, 0xF0, 0x4D, 0x63, 0x96, 0xA9, 0x15 } } },   /*   3 knobs, span    3 B */
  { 10, L"Timeout",               { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } } },   /*   2 knobs, span    2 B */
  { 11, L"BootOrder",             { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } } },   /*   2 knobs, span    2 B */
  { 12, L"RefreshAttribRegistry", { 0x8E31482A, 0x72EA, 0x4E08, { 0xAE, 0x30, 0x23, 0x24, 0x72, 0xBE, 0x3D, 0xD9 } } },   /*   1 knobs, span    1 B */
  { 13, L"Setup",                 { 0x80E1202E, 0x2697, 0x4264, { 0x9C, 0xC9, 0x80, 0x76, 0x2C, 0x3E, 0x58, 0x63 } } },   /*   2 knobs, span    5 B */
  { 14, L"ServerSetup",           { 0x01239999, 0xFC0E, 0x4B6E, { 0x9E, 0x79, 0xD5, 0x4D, 0x5D, 0xB6, 0xCD, 0x20 } } },   /*  48 knobs, span  739 B */
};

#define OOB_VARSTORE_COUNT  (sizeof (mOobVarstores) / sizeof (mOobVarstores[0]))

#endif /* BIOS_CFG_OOB_VARSTORES_H_ */
