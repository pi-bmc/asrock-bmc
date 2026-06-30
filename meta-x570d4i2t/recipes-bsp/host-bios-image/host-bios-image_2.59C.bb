SUMMARY = "Patched host BIOS image (ASRock X570D4I-2T 2.59C + SmbiosBmcPushDxe)"
DESCRIPTION = "\
Fetches upstream EDK2, builds the local SmbiosBmcPushDxe module as an X64 \
DXE_DRIVER, downloads the stock ASRock X570D4I-2T 2.59C AMI Aptio BIOS image, \
and injects the built module with LongSoft UEFIReplace.  The patch replaces the \
PE32 and DXE dependency sections of AMI SendInfoBmcIpmiDxe \
(FILE_GUID 9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24) in all duplicate firmware \
volume copies.  The finished ROM is asserted to remain exactly 32 MiB.\
"

# The deliverable is the proprietary ASRock/AMI Aptio BIOS image (redistribution
# governed by ASRock) with our BSD-2-Clause-Patent SmbiosBmcPushDxe injected;
# treat the resulting firmware blob as CLOSED.
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "x570d4i2t"
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit deploy

SRC_URI = " \
    gitsm://github.com/tianocore/edk2.git;branch=master;protocol=https;name=edk2;destsuffix=edk2 \
    https://github.com/LongSoft/UEFITool/releases/download/0.28.0/UEFIReplace_0.28.0_linux_x86_64.zip;name=uefireplace;downloadfilename=UEFIReplace_0.28.0_linux_x86_64.zip \
    https://download.asrock.com/BIOS/Server/X570D4I-2T(2.59C)ROM.zip;name=bios;downloadfilename=x570d4i2t-bios-2.59C.zip \
    file://SmbiosBmcPushDxe.c \
    file://SmbiosBmcPushDxe.inf \
"

# edk2-stable202602, matching the OpenBMC meta-arm edk2-basetools-native pin.
SRCREV_edk2 ?= "b7a715f7c03c45c6b4575bf88596bfd79658b8ce"
SRCREV_FORMAT = "edk2"

SRC_URI[bios.sha256sum] = "712fa89eda6334f9e1563217be1fe83d2d66a72b7958bb6984dce70221ac1175"
SRC_URI[uefireplace.sha256sum] = "3b8df98d9f3d10be2c33c9cbcd03237a99727fdf5eb6988bce23f7da8b39f432"

DEPENDS = "python3-native util-linux-native nasm-native"

S = "${UNPACKDIR}/edk2"
B = "${WORKDIR}/build-edk2"

export WORKSPACE = "${B}"
export PACKAGES_PATH = "${B}:${S}"
export EDK_TOOLS_PATH = "${S}/BaseTools"
export CONF_PATH = "${B}/Conf"
export PYTHON_COMMAND = "python3"
export GCC5_X64_PREFIX = ""

BTOOLS_PATH = "${EDK_TOOLS_PATH}/BinWrappers/PosixLike"

# EDK2 drives its own compiler/linker flags.  OE target flags are for the BMC
# rootfs toolchain and are not meaningful for this host-built X64 EFI binary.
LDFLAGS[unexport] = "1"
CFLAGS[unexport] = "1"
CXXFLAGS[unexport] = "1"
CPPFLAGS[unexport] = "1"

ROM_NAME ?= "X574I2T2.59C"
PATCHED_ROM ?= "host-bios-${MACHINE}-2.59C-smbiospush.rom"
HOST_BIOS_SIZE ?= "33554432"
TARGET_FFS_GUID ?= "9df02dfd-8cf7-4fc7-b8ae-cbd9560a3f24"
UEFIREPLACE ?= "${UNPACKDIR}/UEFIReplace"
TRUE_DEPEX ?= "${WORKDIR}/true.depex"

# Produces no rootfs packages; this is a deploy-only firmware artifact.
do_package[noexec] = "1"
do_packagedata[noexec] = "1"
do_package_write_ipk[noexec] = "1"
do_populate_sysroot[noexec] = "1"

do_configure[cleandirs] += "${B}"
do_configure() {
    install -d "${B}/Conf"
    install -d "${B}/Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe"

    install -m 0644 "${UNPACKDIR}/SmbiosBmcPushDxe.c" \
        "${B}/Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.c"
    install -m 0644 "${UNPACKDIR}/SmbiosBmcPushDxe.inf" \
        "${B}/Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf"

    cat > "${B}/Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPush.dsc" <<'DSCEOF'
[Defines]
  PLATFORM_NAME           = SmbiosBmcPush
  PLATFORM_GUID           = 11111111-2222-3333-4444-555555555555
  PLATFORM_VERSION        = 1.0
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/SmbiosBmcPush
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = RELEASE
  SKUID_IDENTIFIER        = DEFAULT

[LibraryClasses]
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf

[Components]
  Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf
DSCEOF

    cp "${EDK_TOOLS_PATH}/Conf/build_rule.template" "${CONF_PATH}/build_rule.txt"
    cp "${EDK_TOOLS_PATH}/Conf/tools_def.template" "${CONF_PATH}/tools_def.txt"
    cp "${EDK_TOOLS_PATH}/Conf/target.template" "${CONF_PATH}/target.txt"
}

do_compile() {
    STOCK="${UNPACKDIR}/${ROM_NAME}"
    EFI="${B}/Build/SmbiosBmcPush/RELEASE_GCC5/X64/SmbiosBmcPushDxe.efi"
    PE32_ROM="${WORKDIR}/${ROM_NAME}.uefireplace-pe32"
    OUT="${WORKDIR}/${PATCHED_ROM}"

    [ -f "${STOCK}" ] || bbfatal "stock ROM not found after unpack: ${STOCK}"
    [ -f "${UEFIREPLACE}" ] || bbfatal "UEFIReplace not found after unpack: ${UEFIREPLACE}"
    chmod 0755 "${UEFIREPLACE}"

    [ "$(uname -m)" = "x86_64" ] || \
        bbfatal "UEFIReplace_0.28.0_linux_x86_64 requires an x86_64 build host"

    SSZ="$(stat -c%s "${STOCK}")"
    [ "${SSZ}" = "${HOST_BIOS_SIZE}" ] || \
        bbfatal "stock ROM is ${SSZ} bytes, expected ${HOST_BIOS_SIZE} (32 MiB)"

    if ! grep -q '\${BUILD_CFLAGS}' "${EDK_TOOLS_PATH}/Source/C/Makefiles/header.makefile"; then
        sed -i -e 's:-I \.\.:-I \.\. ${BUILD_CFLAGS} :' \
            "${EDK_TOOLS_PATH}/Source/C/Makefiles/header.makefile"
    fi
    for makefile in "${EDK_TOOLS_PATH}"/Source/C/*/GNUmakefile; do
        if ! grep -q '\${BUILD_LDFLAGS}' "${makefile}"; then
            sed -i -e 's: -luuid: -luuid ${BUILD_LDFLAGS}:g' "${makefile}"
        fi
    done

    oe_runmake -C "${EDK_TOOLS_PATH}" \
        CC="${BUILD_CC}" \
        CXX="${BUILD_CXX}" \
        AS="${BUILD_AS}" \
        AR="${BUILD_AR}" \
        LD="${BUILD_LD}"

    PATH="${BTOOLS_PATH}:$PATH" \
    build \
        -p Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPush.dsc \
        -m Platform/ASRockRack/X570D4I2TPkg/SmbiosBmcPushDxe/SmbiosBmcPushDxe.inf \
        -a X64 \
        -b RELEASE \
        -t GCC5 \
        ${@oe.utils.parallel_make_argument(d, "-n %d")}

    [ -f "${EFI}" ] || bbfatal "EDK2 did not produce ${EFI}"
    printf '\006\010' > "${TRUE_DEPEX}"

    rm -f "${PE32_ROM}" "${OUT}"
    "${UEFIREPLACE}" "${STOCK}" "${TARGET_FFS_GUID}" 10 "${EFI}" \
        -o "${PE32_ROM}" -all || \
        bbfatal "UEFIReplace failed to replace the PE32 section"
    "${UEFIREPLACE}" "${PE32_ROM}" "${TARGET_FFS_GUID}" 13 "${TRUE_DEPEX}" \
        -o "${OUT}" -all || \
        bbfatal "UEFIReplace failed to replace the DXE dependency section"

    OSZ="$(stat -c%s "${OUT}")"
    [ "${OSZ}" = "${HOST_BIOS_SIZE}" ] || \
        bbfatal "patched ROM is ${OSZ} bytes, must be exactly ${HOST_BIOS_SIZE} (32 MiB)"

    python3 - "${OUT}" "${EFI}" <<'PYEOF' || \
        bbfatal "UEFIReplace verification failed"
import hashlib
import lzma
import struct
import sys

rom_path, efi_path = sys.argv[1], sys.argv[2]
rom = open(rom_path, "rb").read()
efi_hash = hashlib.sha256(open(efi_path, "rb").read()).digest()

outer_guid = bytes.fromhex("93fd219e729c154c8c4be77f1db2d792")
target_guid = bytes.fromhex("fd2df09df78cc74fb8aecbd9560a3f24")

def u24(buf, off):
    return buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16)

def parse_fv(fv):
    if fv[0x28:0x2c] != b"_FVH":
        raise RuntimeError("not an FV")
    return struct.unpack_from("<H", fv, 0x30)[0], struct.unpack_from("<Q", fv, 0x20)[0]

def find_ffs(fv, guid):
    _, fv_len = parse_fv(fv)
    off = 0
    while True:
        off = fv.find(guid, off)
        if off < 0 or off + 24 > fv_len:
            return None, None
        if off % 8 == 0:
            size = u24(fv, off + 20)
            if 24 <= size <= fv_len:
                return off, size
        off += 1

def inner_fv_from_outer(fv):
    off, size = find_ffs(fv, outer_guid)
    if off is None:
        raise RuntimeError("outer compressed FFS not found")
    ffs = fv[off:off + size]
    sec = 24
    while sec + 4 <= size:
        sec_size = u24(ffs, sec)
        if ffs[sec + 3] == 0x02:
            data_off = struct.unpack_from("<H", ffs, sec + 20)[0]
            return lzma.decompress(ffs[sec + data_off:sec + sec_size])[0x10:]
        sec += (sec_size + 3) & ~3
    raise RuntimeError("GUID-defined LZMA section not found")

for base, size in ((0x0069f000, 0x808000), (0x01ac1000, 0x3e6000)):
    inner = inner_fv_from_outer(rom[base:base + size])
    off, ffs_size = find_ffs(inner, target_guid)
    if off is None:
        raise RuntimeError("target FFS not found in FV at 0x%x" % base)
    ffs = inner[off:off + ffs_size]
    sec = 24
    found_pe32 = False
    found_true_depex = False
    while sec + 4 <= ffs_size:
        sec_size = u24(ffs, sec)
        sec_type = ffs[sec + 3]
        body = ffs[sec + 4:sec + sec_size]
        if sec_type == 0x10 and hashlib.sha256(body).digest() == efi_hash:
            found_pe32 = True
        if sec_type == 0x13 and body == b"\x06\x08":
            found_true_depex = True
        sec += (sec_size + 3) & ~3
    if not found_pe32 or not found_true_depex:
        raise RuntimeError("patched PE32/DEPEX missing in FV at 0x%x" % base)
PYEOF

    bbnote "Built SmbiosBmcPushDxe from upstream EDK2 and injected it with UEFIReplace; ${PATCHED_ROM} = ${OSZ} bytes."
}

do_deploy() {
    install -d "${DEPLOYDIR}"
    install -m 0644 "${WORKDIR}/${PATCHED_ROM}" "${DEPLOYDIR}/${PATCHED_ROM}"
    ln -sf "${PATCHED_ROM}" "${DEPLOYDIR}/host-bios-${MACHINE}.rom"

    bbnote "[OK] Patched host BIOS: ${DEPLOYDIR}/${PATCHED_ROM} (32 MiB)"
    bbnote "     Stable symlink: ${DEPLOYDIR}/host-bios-${MACHINE}.rom"
}
addtask do_deploy after do_compile before do_build
