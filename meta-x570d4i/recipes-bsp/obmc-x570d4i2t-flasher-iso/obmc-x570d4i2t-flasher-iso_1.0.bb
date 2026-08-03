SUMMARY = "OpenBMC BMC Flasher ISO for ASRock X570D4I-2T"
DESCRIPTION = "\
BIOS-bootable ISO and companion USB/virtual-media images that flash the \
X570D4I-2T 64 MiB BMC SPI NOR from the host CPU via the AST2500 P2A \
(PCIe-to-AHB) bridge using ASPEED SocFlash (DOS edition).  Packages \
FreeDOS 1.3 + isolinux/memdisk.  CSM must be enabled in BIOS Setup before \
booting the ISO.  See .github/x570d4i2t-port/openbmc-flasher-rebuild.sh \
for the equivalent developer-side shell script.\
"

# FreeDOS kernel source files (boot.asm, magic.mac) are GPL-2.0-only.
# socflash.EXE is a proprietary ASPEED binary — CLOSED license.
LICENSE = "GPL-2.0-only & CLOSED"

# LIC_FILES_CHKSUM covers the FDOS kernel GPL-2.0 COPYING file.
# The FreeDOS zip does not contribute code to the image; only its pre-built
# KERNEL.SYS / COMMAND.COM binaries are shipped as-is (GPL-2.0-only).
LIC_FILES_CHKSUM = "file://${UNPACKDIR}/fdos-kernel/COPYING;md5=524e91734a113e304866a9a151b3d6fb"

# Only meaningful for the ASRock X570D4I-2T machine (AST2500, 64 MiB NOR)
COMPATIBLE_MACHINE = "x570d4i2t"

# ---------------------------------------------------------------------------
# Flash parameters — override in build/conf/local.conf if needed.
#
#   FLASHER_MODE = "WIPE"     erase + write the full 64 MiB NOR (default)
#   FLASHER_MODE = "REFRESH"  skip first 64 KiB (preserve u-boot env)
#   FLASHER_CHIP = "0"        CS0 — confirmed by socflash "Boot CS is 0"
#   FLASHER_CHIP = "1"        CS1 — socflash "Can't Find Flash Chip #1"
# ---------------------------------------------------------------------------
FLASHER_MODE ?= "WIPE"
FLASHER_CHIP ?= "0"

# ---------------------------------------------------------------------------
# SocFlash: proprietary ASPEED DOS utility — fetched automatically from
# ASRock's public download server (download.asrock.com).  Redistributing
# the binary requires a separate agreement with ASPEED Technology.
# ---------------------------------------------------------------------------
SOCFLASH_ZIP_URL = "https://download.asrock.com/TSD/socflash%20v1.20.00.zip"

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# FreeDOS 1.3 Floppy Edition — provides KERNEL.SYS and COMMAND.COM.
# To obtain the sha256sum run:
#   curl -fsSL https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip | sha256sum
FREEDOS_URL = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip"

# FreeDOS kernel source — only boot/boot.asm + boot/magic.mac are used (FAT16
# boot sector compilation).  Pin to a reproducible commit; to update run:
#   git ls-remote https://github.com/FDOS/kernel.git refs/heads/master
FDOS_KERNEL_SRCREV ?= "AUTOINC"
# Replace "AUTOINC" above with a fixed SHA-1 for reproducible builds, e.g.:
#   FDOS_KERNEL_SRCREV = "3a2b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b"

SRC_URI = " \
    ${FREEDOS_URL};name=freedos;subdir=freedos-zip \
    git://github.com/FDOS/kernel.git;protocol=https;branch=master;name=fdos-kernel;destsuffix=fdos-kernel \
    ${SOCFLASH_ZIP_URL};name=socflash;downloadfilename=socflash-v1.20.00.zip \
"

SRC_URI[freedos.sha256sum] = "75a4e11a7fce6f124e20927b3022b4b715a2a3f7324c5f5bfea42d90d80eb072"
# sha256sum of https://download.asrock.com/TSD/socflash%20v1.20.00.zip
SRC_URI[socflash.sha256sum] = "5c185080bc4454d47b940f3cf74cc65c6f18d62be067ac192f2c3b2166316541"

SRCREV_fdos-kernel = "${FDOS_KERNEL_SRCREV}"
SRCREV_FORMAT = "fdos_kernel"

# ---------------------------------------------------------------------------
# Build-host (native) tool dependencies
# ---------------------------------------------------------------------------
DEPENDS = " \
    nasm-native \
    mtools-native \
    util-linux-native \
    libisoburn-native \
    syslinux-native \
    file-native \
"

# The syslinux_%.bbappend in this layer extends syslinux-native to also
# install the precompiled BIOS files (isolinux.bin, memdisk, mbr.bin,
# ldlinux.c32) into ${STAGING_DATADIR_NATIVE}/syslinux/bios/.
# See recipes-devtools/syslinux/syslinux_%.bbappend.

# ---------------------------------------------------------------------------
# Firmware dependency: ensure obmc-phosphor-image is fully built first.
# ---------------------------------------------------------------------------
do_compile[depends] += "obmc-phosphor-image:do_image_complete"

inherit deploy

# This recipe produces no rootfs packages.
do_package[noexec]     = "1"
do_packagedata[noexec] = "1"

# ---------------------------------------------------------------------------
# Pre-parse validation of FLASHER_MODE / FLASHER_CHIP.
# ---------------------------------------------------------------------------
python do_check_prerequisites() {
    mode = d.getVar('FLASHER_MODE') or ''
    if mode not in ('WIPE', 'REFRESH'):
        bb.fatal("FLASHER_MODE must be 'WIPE' or 'REFRESH', got: %s" % mode)

    chip = d.getVar('FLASHER_CHIP') or ''
    if chip not in ('0', '1'):
        bb.fatal("FLASHER_CHIP must be '0' or '1', got: %s" % chip)
}
addtask do_check_prerequisites before do_fetch

# ---------------------------------------------------------------------------
do_compile() {
    WORK="${WORKDIR}/flasher-work"
    rm -rf "${WORK}"
    mkdir -p "${WORK}/iso/isolinux"
    mkdir -p "${WORK}/iso/boot"
    mkdir -p "${WORK}/bmc-iso"

    # The ASRock zip extracts to a directory with a space: "socflash v1.20.00/"
    SOCFLASH="${UNPACKDIR}/socflash v1.20.00/socflash.EXE"
    # FreeDOS zip is extracted into freedos-zip/; the floppy image lives at:
    FD_BOOT="${UNPACKDIR}/freedos-zip/144m/x86BOOT.img"
    # FreeDOS kernel git checkout (only boot/ sub-tree is used)
    FDOS_BOOT="${UNPACKDIR}/fdos-kernel/boot"
    # syslinux BIOS files installed by syslinux_%.bbappend
    SYSLINUX_BIOS="${STAGING_DATADIR_NATIVE}/syslinux/bios"
    # Completed firmware image from the standard OpenBMC deploy directory
    FIRMWARE="${DEPLOY_DIR_IMAGE}/obmc-phosphor-image-${MACHINE}.static.mtd"

    # -----------------------------------------------------------------------
    # Validate inputs
    # -----------------------------------------------------------------------
    if ! file "${SOCFLASH}" | grep -q "MS-DOS"; then
        bbfatal "socflash.EXE is not a DOS executable: $(file "${SOCFLASH}")"
    fi
    if [ ! -f "${FIRMWARE}" ]; then
        bbfatal "Firmware not found at:\n  ${FIRMWARE}\nRun 'bitbake obmc-phosphor-image' first."
    fi
    for f in isolinux.bin memdisk mbr.bin ldlinux.c32; do
        if [ ! -f "${SYSLINUX_BIOS}/${f}" ]; then
            bbfatal "syslinux BIOS file not found: ${SYSLINUX_BIOS}/${f}\n" \
                "Check that recipes-devtools/syslinux/syslinux_%.bbappend is in the layer."
        fi
    done

    # -----------------------------------------------------------------------
    # PHASE 1: extract FreeDOS system files from the floppy image
    # -----------------------------------------------------------------------
    mcopy -n -o -i "${FD_BOOT}" ::/KERNEL.SYS              "${WORK}/KERNEL.SYS"
    mcopy -n -o -i "${FD_BOOT}" ::/freedos/bin/command.com "${WORK}/COMMAND.COM"

    # -----------------------------------------------------------------------
    # PHASE 1b: compile the FreeDOS FAT16 boot sector
    #
    # The FreeDOS floppy carries a FAT12-compiled boot sector; our HDD
    # partition is FAT16.  The FAT-walk routines diverge (12-bit vs 16-bit
    # entries), so loading KERNEL.SYS from a FAT16 partition with the FAT12
    # boot sector produces "Error!" and halts.  We recompile from source with
    # -DISFAT16=1.
    # -----------------------------------------------------------------------
    nasm -DISFAT16=1 -I "${FDOS_BOOT}/" -f bin \
        "${FDOS_BOOT}/boot.asm" -o "${WORK}/fat16-boot.bin"

    # Bytes 0x3E – 0x1FF of the compiled binary = the loader code + 0xAA55
    # signature (450 bytes).  These are patched over mformat's generic boot
    # code while preserving mformat's extended BPB at offsets 0x00 – 0x3D.
    dd if="${WORK}/fat16-boot.bin" of="${WORK}/fd-bootcode.bin" \
        bs=1 skip=62 count=450 status=none

    # -----------------------------------------------------------------------
    # PHASE 1c: generate DOS configuration files
    # -----------------------------------------------------------------------
    cat > "${WORK}/FDCONFIG.SYS" <<'CFGEOF'
FILES=20
BUFFERS=10
LASTDRIVE=Z
SHELL=C:\COMMAND.COM C:\ /E:1024 /P=C:\AUTOEXEC.BAT
CFGEOF

    SCMD="SOCFLASH if=FW.IMG cs=${FLASHER_CHIP}"
    if [ "${FLASHER_MODE}" = "REFRESH" ]; then
        # Skip the first 64 KiB to preserve u-boot env / vendor config block
        SCMD="${SCMD} skip=0x10000 offset=0x10000"
    fi
    if [ "${FLASHER_CHIP}" = "0" ]; then ALT_CHIP="1"; else ALT_CHIP="0"; fi

    FW_MB="$(du -m "${FIRMWARE}" | cut -f1)"

    cat > "${WORK}/FDAUTO.BAT" <<BATEOF
@ECHO OFF
CLS
ECHO ===============================================
ECHO   OpenBMC BMC Flasher -- ASRock X570D4I-2T
ECHO ===============================================
ECHO.
ECHO   Mode  : ${FLASHER_MODE}
ECHO   Chip  : ${FLASHER_CHIP}   (socflash cs=${FLASHER_CHIP})
ECHO   Image : C:\\FW.IMG  (${FW_MB} MB OpenBMC)
ECHO.
ECHO   Press Ctrl-C now to abort, or any key to begin.
ECHO   Flash takes ~2-3 minutes. DO NOT power off.
ECHO.
PAUSE
ECHO.
C:
CD \\
${SCMD}
IF ERRORLEVEL 1 GOTO FAIL
ECHO.
ECHO ===============================================
ECHO   [OK] Flash succeeded.
ECHO   Eject the virtual CD, then power-cycle host.
ECHO ===============================================
GOTO END
:FAIL
ECHO.
ECHO ===============================================
ECHO   [FAIL] socflash returned an error.
ECHO   Retry on alternate chip-select:
ECHO     SOCFLASH if=FW.IMG cs=${ALT_CHIP}
ECHO ===============================================
:END
BATEOF

    cp "${SOCFLASH}" "${WORK}/SOCFLASH.EXE"

    # -----------------------------------------------------------------------
    # PHASE 2: build the FAT16 hard-disk image
    #
    # Size = FLASH_SIZE KiB (firmware) + 16 MiB (FreeDOS + overhead).
    # FLASH_SIZE comes from conf/machine/x570d4i2t.conf (= 65536 KiB = 64 MiB).
    # -----------------------------------------------------------------------
    FLASH_MiB=$(expr ${FLASH_SIZE} / 1024)
    HDD_MiB=$(expr ${FLASH_MiB} + 16)
    HDD="${WORK}/iso/boot/freedos.img"

    bbnote "Building ${HDD_MiB} MiB FAT16 hard-disk image (${FLASH_MiB} MiB firmware + 16 MiB FreeDOS)..."
    truncate -s "${HDD_MiB}M" "${HDD}"

    # One active FAT16 primary partition starting at sector 2048 (1 MiB alignment)
    printf 'label: dos\nunit: sectors\nstart=2048, type=06, bootable\n' \
        | sfdisk --quiet "${HDD}"

    # syslinux MBR: 440-byte code that reads the partition table and loads
    # the VBR of the active partition
    dd if="${SYSLINUX_BIOS}/mbr.bin" of="${HDD}" \
        bs=440 count=1 conv=notrunc status=none

    # Format the FAT16 partition.
    # -H 2048 sets BPB.HiddenSectors so FreeDOS can translate FAT-relative
    # sector numbers to absolute disk LBA.  mformat's @@offset does not set
    # this automatically.
    PART_OFFSET=$(expr 2048 \* 512)
    mformat -H 2048 -i "${HDD}@@${PART_OFFSET}" -v "OBMCFLASH" ::

    # Patch the FreeDOS FAT16 boot code into the VBR (bytes 62 – 511),
    # leaving mformat's BPB (bytes 0 – 61) intact.
    dd if="${WORK}/fd-bootcode.bin" of="${HDD}" \
        bs=1 seek=$(expr ${PART_OFFSET} + 62) count=450 conv=notrunc status=none

    # Populate the FAT16 partition.
    # KERNEL.SYS must be the first file written (minimises fragmentation).
    mcopy -o    -i "${HDD}@@${PART_OFFSET}" "${WORK}/KERNEL.SYS"   ::/
    mcopy -o    -i "${HDD}@@${PART_OFFSET}" "${WORK}/COMMAND.COM"  ::/
    # Ship under both names so the kernel picks one up regardless of version
    mcopy -o -t -i "${HDD}@@${PART_OFFSET}" "${WORK}/FDCONFIG.SYS" ::/FDCONFIG.SYS
    mcopy -o -t -i "${HDD}@@${PART_OFFSET}" "${WORK}/FDCONFIG.SYS" ::/CONFIG.SYS
    mcopy -o -t -i "${HDD}@@${PART_OFFSET}" "${WORK}/FDAUTO.BAT"   ::/AUTOEXEC.BAT
    mcopy -o    -i "${HDD}@@${PART_OFFSET}" "${WORK}/SOCFLASH.EXE" ::/
    mcopy -o    -i "${HDD}@@${PART_OFFSET}" "${FIRMWARE}"          ::/FW.IMG

    bbnote "FAT16 partition directory:"
    mdir -i "${HDD}@@${PART_OFFSET}" :: | sed 's/^/  /' | while IFS= read -r line; do bbnote "${line}"; done

    # -----------------------------------------------------------------------
    # PHASE 3: assemble the ISO9660 image (isolinux + memdisk)
    # -----------------------------------------------------------------------
    bbnote "Installing isolinux + memdisk..."
    install -m 0644 "${SYSLINUX_BIOS}/isolinux.bin" "${WORK}/iso/isolinux/"
    install -m 0644 "${SYSLINUX_BIOS}/memdisk"      "${WORK}/iso/isolinux/"
    install -m 0644 "${SYSLINUX_BIOS}/ldlinux.c32"  "${WORK}/iso/isolinux/"

    cat > "${WORK}/iso/isolinux/isolinux.cfg" <<'CFGEOF'
DEFAULT flash
PROMPT 1
TIMEOUT 50

LABEL flash
  MENU LABEL OpenBMC BMC Flasher (FreeDOS + SocFlash)
  KERNEL /isolinux/memdisk
  INITRD /boot/freedos.img
  APPEND harddisk

LABEL shell
  MENU LABEL Drop to DOS shell (skip auto-flash)
  KERNEL /isolinux/memdisk
  INITRD /boot/freedos.img
  APPEND harddisk safeint
CFGEOF

    # BMC.IMA on the ISO9660 root — 8.3-clean filename for in-BIOS scanners
    # (ASRock / AMI BIOS firmware update utilities look for *.IMA on mounted media)
    cp -f "${FIRMWARE}" "${WORK}/iso/BMC.IMA"

    bbnote "Building ISO..."
    xorriso -as mkisofs \
        -R -J -V "OBMC-FLASHER" \
        -iso-level 3 \
        -b isolinux/isolinux.bin \
        -c isolinux/boot.cat \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        "${WORK}/iso" \
        -o "${WORK}/obmc-flasher.iso" 2>&1 | tail -3

    # -----------------------------------------------------------------------
    # PHASE 4: stage companion artifacts
    # -----------------------------------------------------------------------

    # Standalone .ima — rename of the raw firmware for in-BIOS update paths
    # (BIOS Setup -> Server Mgmt -> BMC firmware update).  Note: MegaRAC's
    # update receiver may still signature-check this; treat as best-effort.
    cp -f "${FIRMWARE}" "${WORK}/obmc-x570d4i2t.ima"

    # FAT32 USB disk image — dd this directly to a USB stick for in-BIOS
    # BMC update without needing a virtual CD.
    USB="${WORK}/obmc-bmc-fat32.img"
    USB_MiB=$(expr ${FLASH_MiB} + 32)
    truncate -s "${USB_MiB}M" "${USB}"
    printf 'label: dos\nunit: sectors\nstart=2048, type=0c, bootable\n' \
        | sfdisk --quiet "${USB}"
    dd if="${SYSLINUX_BIOS}/mbr.bin" of="${USB}" \
        bs=440 count=1 conv=notrunc status=none
    USB_PART_OFFSET=$(expr 2048 \* 512)
    mformat -F -H 2048 -i "${USB}@@${USB_PART_OFFSET}" -v "BMC-FLASH" ::
    mcopy -o -i "${USB}@@${USB_PART_OFFSET}" "${FIRMWARE}" ::/BMC.IMA

    # BMC-only data ISO — mount via BMC virtual-media (no boot record needed;
    # the in-BIOS flasher walks the filesystem looking for BMC.IMA).
    cp -f "${FIRMWARE}" "${WORK}/bmc-iso/BMC.IMA"
    xorriso -as mkisofs \
        -V "BMC-FLASH" \
        -J -R -iso-level 3 \
        "${WORK}/bmc-iso" \
        -o "${WORK}/obmc-bmc-fat32.iso" 2>&1 | tail -2
}

do_deploy() {
    WORK="${WORKDIR}/flasher-work"

    install -d "${DEPLOYDIR}"
    install -m 0644 "${WORK}/obmc-flasher.iso"    "${DEPLOYDIR}/"
    install -m 0644 "${WORK}/obmc-x570d4i2t.ima"  "${DEPLOYDIR}/"
    install -m 0644 "${WORK}/obmc-bmc-fat32.img"  "${DEPLOYDIR}/"
    install -m 0644 "${WORK}/obmc-bmc-fat32.iso"  "${DEPLOYDIR}/"

    bbnote ""
    bbnote "[OK] Flasher artifacts: ${DEPLOYDIR}"
    bbnote "  obmc-flasher.iso    BIOS-bootable ISO (FreeDOS + SocFlash; CSM required)"
    bbnote "  obmc-x570d4i2t.ima  Raw firmware (in-BIOS update path)"
    bbnote "  obmc-bmc-fat32.img  FAT32 disk image — dd to USB stick"
    bbnote "  obmc-bmc-fat32.iso  Data ISO     — mount via BMC virtual-media"
    bbnote ""
    bbnote "Flash mode:  ${FLASHER_MODE}   chip-select: ${FLASHER_CHIP}"
    bbnote "Firmware:    obmc-phosphor-image-${MACHINE}.static.mtd"
}

addtask do_deploy after do_compile before do_build
