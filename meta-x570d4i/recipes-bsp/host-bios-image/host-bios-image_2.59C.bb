SUMMARY = "Patched host BIOS image (ASRock X570D4I-2T 2.59C + BMC push driver)"
DESCRIPTION = "\
Fetches upstream EDK2, builds the local host->BMC push driver as an X64 \
DXE_DRIVER, downloads the stock ASRock X570D4I-2T 2.59C AMI Aptio BIOS image, \
and injects the built module with LongSoft UEFIReplace.  The patch replaces the \
PE32 and DXE dependency sections of AMI SendInfoBmcIpmiDxe \
(FILE_GUID 9DF02DFD-8CF7-4FC7-B8AE-CBD9560A3F24) in all duplicate firmware \
volume copies.  The finished ROM is asserted to remain exactly 32 MiB.\
\
The injected driver has two halves: SmbiosBmcPushDxe (SMBIOS over \
phosphor-ipmi-blobs) and BiosCfgOobDxe, the host side of the OpenBMC \
BIOS-config OOB command set (NetFn 0x30, cmds 0xD3-0xD6).  The stock BIOS has \
no BIOS-config producer of its own -- verified by decompiling both 2.59C and \
2.59F: stock SendInfoBmcIpmiDxe is 3 KB and never issues 0xD3/0xD5, and no \
module in either image contains the XML the BMC receives.  BiosCfgOobDxe \
supplies it, and unlike the earlier ad-hoc build it also reports the LIVE knob \
values read with gRT->GetVariable and applies settings staged from Redfish.\
\
Sources live in files/OpenOobPkg, one subdirectory per driver, built together \
from OpenOobPkg.dsc.  Besides the module above it produces two standalone \
drivers that are grafted in as NEW FFS files rather than displacing an AMI \
slot: OobIpmiDxe (an IPMI-over-KCS transport that also publishes AMI's \
transport GUID 4A1D0E66 as a compatibility marker, so DxeIpmiBmcInitialize \
becomes removable without stranding the 15 modules that DEPEX on it) and \
VideoRouteDxe (per-boot video path selection; defaults to the onboard ASPEED \
and opens the ASPEED GOP dispatch gate 794E15D9 that the stock Setup policy \
leaves shut).\
\
Setting HOST_BIOS_STRIP_OOB = \"1\" additionally runs tools/romsurgeon.py to \
delete AMI's Redfish, PLDM and SMM-BMC modules and inject those two drivers.  \
It is off by default: that image has never been booted, and a stranded DEPEX \
fails silently.  See edk2-x570d4i2t/docs/STRIP-PLAN.md.\
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
    file://OpenOobPkg \
    file://tools \
    file://x570d4i2t-bios-knobs.xml \
    file://gen-schema-header.py \
"

# edk2-stable202602, matching the OpenBMC meta-arm edk2-basetools-native pin.
SRCREV_edk2 ?= "b7a715f7c03c45c6b4575bf88596bfd79658b8ce"
SRCREV_FORMAT = "edk2"

SRC_URI[bios.sha256sum] = "712fa89eda6334f9e1563217be1fe83d2d66a72b7958bb6984dce70221ac1175"
SRC_URI[uefireplace.sha256sum] = "3b8df98d9f3d10be2c33c9cbcd03237a99727fdf5eb6988bce23f7da8b39f432"

# python3-pyyaml-native: romsurgeon.py reads the strip/inject profile. Only needed
# when HOST_BIOS_STRIP_OOB is enabled, but keeping it unconditional avoids a
# confusing late failure after the EDK2 build has already run.
DEPENDS = "python3-native python3-pyyaml-native util-linux-native nasm-native"

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

# ---------------------------------------------------------------------------
# FFS slots we DISPLACE rather than add to (driver:slot-guid pairs)
# ---------------------------------------------------------------------------
# Replacing an existing FFS beats adding a new one whenever a suitable donor
# exists. The file GUID is preserved, so anything that already refers to it --
# most importantly the DXE apriori -- keeps resolving, and the replacement
# dispatches at exactly the point the original did. It also needs no free space
# in the volume.
#
#   SmbiosBmcPushDxe -> SendInfoBmcIpmiDxe (9df02dfd)
#       The original arrangement. Note this one GROWS the slot (6,400 B into a
#       3,266 B file), which is fine: UEFIReplace rebuilds the FFS.
#
#   OobIpmiDxe -> DxeIpmiBmcInitialize (6372357a)
#       The stock producer of the AMI DXE IPMI transport 4A1D0E66, and entry 7
#       of the 15-entry DXE apriori -- i.e. dispatched before ANY DEPEX is
#       evaluated. Injecting OobIpmiDxe as a new FFS instead gave it DEPEX=TRUE,
#       which is only eligible in the first ordinary round, strictly later than
#       the apriori. Taking the slot restores the stock ordering exactly, and
#       shrinks the file 23,552 -> ~3,840 bytes into the bargain.
#
#       Consequence: DxeIpmiBmcInitialize must NOT appear in any strip profile,
#       for the same reason SendInfoBmcIpmiDxe must not -- by the time romsurgeon
#       runs, that GUID is OUR driver.
OOB_REPLACE_SLOTS ?= "\
    SmbiosBmcPushDxe:9df02dfd-8cf7-4fc7-b8ae-cbd9560a3f24 \
    OobIpmiDxe:6372357a-06d7-43ef-b55c-1964f3dd6916 \
"

# Every driver OpenOobPkg.dsc produces.
OOB_DRIVERS ?= "SmbiosBmcPushDxe BiosCfgOobDxe OobIpmiDxe VideoRouteDxe OobSetupDefaultsDxe"

# Drivers grafted in as NEW FFS files. Everything in OOB_REPLACE_SLOTS is
# excluded, because those reach the flash by displacing an existing AMI FFS
# rather than being appended: SmbiosBmcPushDxe and OobIpmiDxe.
# Only used when HOST_BIOS_STRIP_OOB is enabled.
OOB_INJECT_DRIVERS ?= "BiosCfgOobDxe VideoRouteDxe OobSetupDefaultsDxe"

# Run tools/romsurgeon.py to free space in the dispatched firmware volume and
# graft in OOB_INJECT_DRIVERS.
#
# ON BY DEFAULT. This was off while no profile had ever been booted, but the
# inject-only image has since run on hardware (see the 2026-08-05 boot profile:
# 37.8s to OS, all four injected drivers accounted for), and the SPI-mux recovery
# path makes a bad image cheap to undo. The remaining hazard is unchanged — a
# stranded DEPEX fails silently, and on a board whose only console is the KVM
# that reads as a blind hang — so keep changes to the profile itself conservative.
#
# Turning this back OFF is a real functional regression, not just fewer features:
#   - BiosCfgOobDxe is its own FFS file since it was split out of
#     SmbiosBmcPushDxe, so /redfish/v1/Systems/system/Bios reverts to a defaults
#     listing instead of live values (do_compile warns about this explicitly);
#   - OobSetupDefaultsDxe stops reasserting Above 4G Decoding, which is what
#     cleared the terminal POST 0x99 hang with the Tesla K80 installed.
# Only SmbiosBmcPushDxe (SMBIOS + the P2A PCIe/NVMe inventory push) survives with
# this disabled, because it reaches flash by displacing an AMI slot instead.
HOST_BIOS_STRIP_OOB ?= "1"

# Which profile to use when enabled:
#   inject-only  strips ONLY AMI's Redfish/REST stack (~384 KiB of leaf modules,
#                zero dependents) to make room, then injects. Lowest risk.
#   strip-oob    additionally removes PLDM, the SMM BMC island and assorted BMC
#                feature drivers. Bigger win, more blast radius.
HOST_BIOS_OOB_PROFILE ?= "strip-oob"
STRIP_PROFILE ?= "${UNPACKDIR}/tools/profiles/${HOST_BIOS_OOB_PROFILE}.yaml"
STRIPPED_ROM ?= "host-bios-${MACHINE}-2.59C-openoob.rom"

# ---------------------------------------------------------------------------
# Setup defaults baked into the NVAR store  (NAME:PAYLOAD_LEN:OFFSET:VALUE)
# ---------------------------------------------------------------------------
# These are boot requirements, not preferences, so they are written into the
# firmware's *defaults* rather than enforced at runtime. Two reasons that
# matters:
#
#   * Timing. A DXE driver is already too late for anything consumed earlier --
#     PCI resource assignment reads Above 4G Decoding before our code runs, so
#     correcting it at runtime costs a reboot to take effect.
#   * Durability. A shipped ROM contains no live Setup variables at all; the
#     only NVAR record is StdDefaults, and the firmware materialises every
#     varstore from it. Reflashing the BIOS therefore wipes the live NVAR
#     region back to exactly these defaults -- which is why runtime-only fixes
#     had to be re-applied after every flash.
#
# PAYLOAD_LEN disambiguates: two nested defaults are both named "Setup" --
# varstore 1 (511-byte payload, 308 knobs) and varstore 13 (7-byte payload) --
# and the nested GUID indices do not resolve against the store's GUID table, so
# the length is the only usable key. nvarsurgeon refuses an ambiguous or
# out-of-range write rather than guessing.
#
#   Setup[0x0008] = 0x01  PTT005  Network Stack Driver Support = Enabled.
#                         Gates the whole UEFI network stack: "If Disabled,
#                         NetWork Stack Driver will be skipped". Without it
#                         there is no PXE boot entry for an IPMI boot override
#                         to select.
#   Setup[0x01EE] = 0x02  BFOL000/BFOL001  Boot From Onboard LAN(X550) =
#                         "Onboard LAN UEFI PXE" (0x00 Disabled, 0x04 LAN1 PXE,
#                         0x05 LAN2 PXE, 0x02 UEFI PXE). Both knob names share
#                         this byte; they are the CSM and UEFI presentations of
#                         the same setting.
#   PCI_COMMON[3] = 0x01  Above 4G Decoding = Enabled. The Tesla K80's ~12 GiB
#                         prefetchable BARs do not fit below 4 GiB; without this
#                         the board wedges at POST 0x99 with no console.
#   PCI_COMMON[5] = 0x00  SR-IOV = Disabled. Already the default; pinned because
#                         it feeds the same resource pressure as Above 4G.
#
# NetworkStackVar already defaults to 01/01/01 (stack, IPv4 PXE, IPv6 PXE), so
# it needs nothing -- the entries below are asserted as a regression check and
# report "already" rather than changing anything.
HOST_BIOS_NVAR_DEFAULTS ?= "\
    Setup:511:0x0008:0x01 \
    Setup:511:0x01EE:0x02 \
    PCI_COMMON:8:3:0x01 \
    PCI_COMMON:8:5:0x00 \
    NetworkStackVar:8:0:0x01 \
    NetworkStackVar:8:1:0x01 \
"

# Produces no rootfs packages; this is a deploy-only firmware artifact.
do_package[noexec] = "1"
do_packagedata[noexec] = "1"
do_package_write_ipk[noexec] = "1"
do_populate_sysroot[noexec] = "1"

do_configure[cleandirs] += "${B}"
do_configure() {
    install -d "${B}/Conf"

    # Lay the package out as an EDK2 package inside the workspace. PACKAGES_PATH
    # includes ${B}, so INF/DEC references resolve as OpenOobPkg/... exactly as
    # they are written in OpenOobPkg.dsc.
    [ -d "${UNPACKDIR}/OpenOobPkg" ] || \
        bbfatal "OpenOobPkg not unpacked to ${UNPACKDIR}; check the file://OpenOobPkg SRC_URI entry"
    [ -f "${UNPACKDIR}/OpenOobPkg/OpenOobPkg.dsc" ] || \
        bbfatal "OpenOobPkg.dsc missing from ${UNPACKDIR}/OpenOobPkg"
    [ -f "${UNPACKDIR}/tools/romsurgeon.py" ] || \
        bbfatal "tools/ not unpacked to ${UNPACKDIR}; check the file://tools SRC_URI entry"

    cp -R "${UNPACKDIR}/OpenOobPkg" "${B}/OpenOobPkg"
    chmod -R u+w "${B}/OpenOobPkg"

    # The knob schema is a build input, not a checked-in binary: the readable XML
    # is compressed and turned into a C array here.  Reproducibility is
    # load-bearing -- the DXE quotes the resulting length and CRC32 to the BMC so
    # it can skip re-sending a payload the BMC already holds.
    python3 "${UNPACKDIR}/gen-schema-header.py" \
        "${UNPACKDIR}/x570d4i2t-bios-knobs.xml" \
        "${B}/OpenOobPkg/BiosCfgOobDxe/BiosCfgOobSchema.h" || \
        bbfatal "failed to generate BiosCfgOobSchema.h"

    cp "${EDK_TOOLS_PATH}/Conf/build_rule.template" "${CONF_PATH}/build_rule.txt"
    cp "${EDK_TOOLS_PATH}/Conf/tools_def.template" "${CONF_PATH}/tools_def.txt"
    cp "${EDK_TOOLS_PATH}/Conf/target.template" "${CONF_PATH}/target.txt"
}

do_compile() {
    STOCK="${UNPACKDIR}/${ROM_NAME}"
    EFIDIR="${B}/Build/OpenOobPkg/RELEASE_GCC5/X64"
    EFI="${EFIDIR}/SmbiosBmcPushDxe.efi"
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

    # Drop VfrCompile from the BaseTools tool list.
    #
    # BaseTools builds it unconditionally, and its bundled PCCTS/ANTLR sources do
    # not compile with a modern GCC:
    #
    #   VfrLexer.cpp:1654:1: error: 'ANTLRTokenType' does not name a type;
    #                               did you mean 'ANTLRTokenPtr'?
    #
    # This does not bite on an incremental build, because a previously-built
    # BaseTools is reused -- it only surfaces after a clean, which is how it went
    # unnoticed until now. OpenOobPkg has no .vfr/.uni/.idf sources and no INF
    # references any, so the VFR compiler is never invoked; carrying a patch for
    # a compiler we do not use would be pure liability. It appears exactly once
    # in the makefile, in the APPLICATIONS list.
    sed -i -e '/VfrCompile/d' "${EDK_TOOLS_PATH}/Source/C/GNUmakefile"

    oe_runmake -C "${EDK_TOOLS_PATH}" \
        CC="${BUILD_CC}" \
        CXX="${BUILD_CXX}" \
        AS="${BUILD_AS}" \
        AR="${BUILD_AR}" \
        LD="${BUILD_LD}"

    # Build every component in the DSC, not just one module: OobIpmiDxe and
    # VideoRouteDxe are separate drivers grafted in as their own FFS files.
    PATH="${BTOOLS_PATH}:$PATH" \
    build \
        -p OpenOobPkg/OpenOobPkg.dsc \
        -a X64 \
        -b RELEASE \
        -t GCC5 \
        ${@oe.utils.parallel_make_argument(d, "-n %d")}

    for m in ${OOB_DRIVERS}; do
        [ -f "${EFIDIR}/${m}.efi" ] || bbfatal "EDK2 did not produce ${EFIDIR}/${m}.efi"
        head -c2 "${EFIDIR}/${m}.efi" | grep -q MZ || \
            bbfatal "${m}.efi is not a PE image"
        bbnote "built ${m}.efi ($(stat -c%s "${EFIDIR}/${m}.efi") bytes)"
    done

    printf '\006\010' > "${TRUE_DEPEX}"

    # Displace one FFS slot per OOB_REPLACE_SLOTS entry. Each needs two passes:
    # section type 10 (PE32) then 13 (DXE dependency), the latter forced to a
    # bare TRUE so the replacement never inherits the original's DEPEX -- which
    # would otherwise make our driver wait on protocols only the AMI module it
    # replaced ever cared about.
    rm -f "${PE32_ROM}" "${OUT}"
    SRC="${STOCK}"
    for pair in ${OOB_REPLACE_SLOTS}; do
        drv="${pair%%:*}"
        slot="${pair##*:}"
        src_efi="${EFIDIR}/${drv}.efi"
        [ -f "${src_efi}" ] || bbfatal "OOB_REPLACE_SLOTS names ${drv}, but ${src_efi} was not built"

        "${UEFIREPLACE}" "${SRC}" "${slot}" 10 "${src_efi}" \
            -o "${PE32_ROM}" -all || \
            bbfatal "UEFIReplace failed to replace the PE32 section of ${slot} (${drv})"
        "${UEFIREPLACE}" "${PE32_ROM}" "${slot}" 13 "${TRUE_DEPEX}" \
            -o "${OUT}" -all || \
            bbfatal "UEFIReplace failed to replace the DXE dependency section of ${slot} (${drv})"

        bbnote "displaced FFS ${slot} with ${drv}.efi ($(stat -c%s "${src_efi}") bytes)"
        # Chain: this pass's output is the next pass's input. Copy rather than
        # rename so ${OUT} is always the final artefact.
        cp -f "${OUT}" "${WORKDIR}/.replace-chain.rom"
        SRC="${WORKDIR}/.replace-chain.rom"
        rm -f "${PE32_ROM}"
    done
    rm -f "${WORKDIR}/.replace-chain.rom"

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

    # ---------------------------------------------------------------------
    # Optional second stage: strip AMI's OOB stack and graft in the standalone
    # drivers. Operates on the UEFIReplace output, so the SmbiosBmcPushDxe slot
    # is already ours by this point -- which is exactly why the strip profile
    # must NOT list 9df02dfd. See the note in tools/profiles/strip-oob.yaml.
    # ---------------------------------------------------------------------
    if [ "${HOST_BIOS_STRIP_OOB}" != "1" ]; then
        bbwarn "HOST_BIOS_STRIP_OOB=0: only SmbiosBmcPushDxe is in ${PATCHED_ROM}."
        bbwarn "  BiosCfgOobDxe, OobIpmiDxe and VideoRouteDxe are built and deployed to"
        bbwarn "  openoob-drivers/ but NOT injected — adding FFS files needs free space in"
        bbwarn "  the dispatched firmware volume, which needs a strip profile to run."
        bbwarn "  In particular BIOS-config OOB is absent, so /redfish/v1/Systems/system/Bios"
        bbwarn "  will report defaults rather than live values. Set HOST_BIOS_STRIP_OOB=1"
        bbwarn "  (profile: ${HOST_BIOS_OOB_PROFILE}) to include them."
    fi

    if [ "${HOST_BIOS_STRIP_OOB}" = "1" ]; then
        STRIPPED="${WORKDIR}/${STRIPPED_ROM}"
        rm -f "${STRIPPED}"

        [ -f "${STRIP_PROFILE}" ] || \
            bbfatal "HOST_BIOS_OOB_PROFILE='${HOST_BIOS_OOB_PROFILE}' has no profile at ${STRIP_PROFILE}"

        python3 "${UNPACKDIR}/tools/romsurgeon.py" apply \
            --rom "${OUT}" \
            --profile "${STRIP_PROFILE}" \
            --efi-dir "${EFIDIR}" \
            --out "${STRIPPED}" || \
            bbfatal "romsurgeon failed to strip/inject the OOB stack"

        SSZ2="$(stat -c%s "${STRIPPED}")"
        [ "${SSZ2}" = "${HOST_BIOS_SIZE}" ] || \
            bbfatal "stripped ROM is ${SSZ2} bytes, must be exactly ${HOST_BIOS_SIZE} (32 MiB)"

        # Bake the Setup defaults that are boot requirements into the NVAR store.
        if [ -n "${HOST_BIOS_NVAR_DEFAULTS}" ]; then
            NVAR_ARGS=""
            for spec in ${HOST_BIOS_NVAR_DEFAULTS}; do
                NVAR_ARGS="${NVAR_ARGS} --set ${spec}"
            done
            python3 "${UNPACKDIR}/tools/nvarsurgeon.py" "${STRIPPED}" \
                ${NVAR_ARGS} --out "${STRIPPED}.nvar" || \
                bbfatal "nvarsurgeon failed to apply HOST_BIOS_NVAR_DEFAULTS"
            mv -f "${STRIPPED}.nvar" "${STRIPPED}"

            NSZ="$(stat -c%s "${STRIPPED}")"
            [ "${NSZ}" = "${HOST_BIOS_SIZE}" ] || \
                bbfatal "NVAR-patched ROM is ${NSZ} bytes, must be ${HOST_BIOS_SIZE}"
        fi

        # Confirm each injected driver is present and locatable in the result, and
        # that our SmbiosBmcPushDxe slot survived the strip.
        python3 "${UNPACKDIR}/tools/romsurgeon.py" inspect --rom "${STRIPPED}" \
            > "${WORKDIR}/openoob-inventory.txt" || \
            bbfatal "romsurgeon could not re-parse the stripped ROM"

        for m in ${OOB_INJECT_DRIVERS}; do
            grep -q "  ${m} *$" "${WORKDIR}/openoob-inventory.txt" || \
                bbfatal "${m} missing from the stripped ROM's FFS inventory"
        done

        # The repurposed slot is still LISTED as SendInfoBmcIpmiDxe: UEFIReplace
        # swaps only the PE32 and DEPEX sections and leaves the FFS UI section
        # alone, so the inventory shows AMI's name for what is now our driver.
        # Checking for "SmbiosBmcPush" here would never match.
        grep -q '  SendInfoBmcIpmiDxe *$' "${WORKDIR}/openoob-inventory.txt" || \
            bbfatal "the 9df02dfd slot carrying SmbiosBmcPushDxe did not survive the strip"

        # And prove the payload really is ours rather than the stock 3 KB module.
        # The PE32 lives inside the LZMA-compressed inner FV, so this has to walk
        # the volume rather than search the ROM for the raw image bytes.
        python3 - "${STRIPPED}" "${EFI}" "${TARGET_FFS_GUID}" \
            "${UNPACKDIR}/tools" <<'PYEOF' || \
            bbfatal "the 9df02dfd slot does not contain our SmbiosBmcPushDxe PE32"
import hashlib, sys, uuid
sys.path.insert(0, sys.argv[4])
import uefifv as P

rom = open(sys.argv[1], "rb").read()
want = hashlib.sha256(open(sys.argv[2], "rb").read()).digest()
guid = uuid.UUID(sys.argv[3]).bytes_le

outer = rom[P.FV_OFFSET:P.FV_OFFSET + P.FV_SIZE]
off, size = P.find_ffs_in_fv(outer, P.OUTER_FFS_GUID)
if off is None:
    raise SystemExit("LZMA container FFS not found")
ffs = outer[off:off + size]

pos = 24
inner = None
while pos + 4 <= size:
    sec_len = P.u24(ffs, pos)
    if ffs[pos + 3] == P.SEC_GUID_DEFINED:
        import struct
        data_off = struct.unpack_from("<H", ffs, pos + 20)[0]
        inner = P.lzma_decompress(ffs[pos + data_off:pos + sec_len])[0x10:]
        break
    pos += (sec_len + 3) & ~3
if inner is None:
    raise SystemExit("GUID-defined LZMA section not found")

off, size = P.find_ffs_in_fv(inner, guid)
if off is None:
    raise SystemExit("target FFS %s not present" % sys.argv[3])

pos = off + 24
while pos + 4 <= off + size:
    sec_len = P.u24(inner, pos)
    if sec_len < 4:
        break
    if inner[pos + 3] == P.SEC_PE32:
        if hashlib.sha256(inner[pos + 4:pos + sec_len]).digest() == want:
            print("verified: 9df02dfd carries our SmbiosBmcPushDxe PE32")
            raise SystemExit(0)
        raise SystemExit("PE32 in 9df02dfd does not match the built image")
    pos += (sec_len + 3) & ~3
raise SystemExit("no PE32 section in the 9df02dfd FFS")
PYEOF

        bbnote "Stripped AMI OOB stack and injected ${OOB_INJECT_DRIVERS}; ${STRIPPED_ROM} = ${SSZ2} bytes."
        bbwarn "HOST_BIOS_STRIP_OOB=1: this image has never been booted. Make sure you can reflash over the BIOS SPI mux before powering the host on."
    fi
}

do_deploy() {
    install -d "${DEPLOYDIR}"
    install -m 0644 "${WORKDIR}/${PATCHED_ROM}" "${DEPLOYDIR}/${PATCHED_ROM}"

    # Ship the individual EFI images too. They are what you need to graft a single
    # driver into a ROM by hand with tools/romsurgeon.py, without a full rebuild.
    install -d "${DEPLOYDIR}/openoob-drivers"
    for m in ${OOB_DRIVERS}; do
        install -m 0644 "${B}/Build/OpenOobPkg/RELEASE_GCC5/X64/${m}.efi" \
            "${DEPLOYDIR}/openoob-drivers/${m}.efi"
    done

    if [ "${HOST_BIOS_STRIP_OOB}" = "1" ]; then
        install -m 0644 "${WORKDIR}/${STRIPPED_ROM}" "${DEPLOYDIR}/${STRIPPED_ROM}"
        install -m 0644 "${WORKDIR}/openoob-inventory.txt" \
            "${DEPLOYDIR}/openoob-drivers/openoob-inventory.txt"
        ln -sf "${STRIPPED_ROM}" "${DEPLOYDIR}/host-bios-${MACHINE}.rom"
        bbnote "[OK] Stripped host BIOS: ${DEPLOYDIR}/${STRIPPED_ROM} (32 MiB)"
    else
        ln -sf "${PATCHED_ROM}" "${DEPLOYDIR}/host-bios-${MACHINE}.rom"
    fi

    bbnote "[OK] Patched host BIOS: ${DEPLOYDIR}/${PATCHED_ROM} (32 MiB)"
    bbnote "     Stable symlink: ${DEPLOYDIR}/host-bios-${MACHINE}.rom"
    bbnote "     Drivers:        ${DEPLOYDIR}/openoob-drivers/"
}
addtask do_deploy after do_compile before do_build
