# Expose pre-compiled BIOS bootloader binaries (isolinux.bin, memdisk, mbr.bin,
# ldlinux.c32) in the native sysroot so the obmc-x570d4i2t-flasher-iso recipe
# can consume them without requiring syslinux to be installed on the build host.
#
# The upstream syslinux recipe's do_compile:class-native only builds the
# Linux-side installer utilities.  We append a step to compile the full BIOS
# firmware as well (real-mode x86 code via NASM + 32-bit GCC for the ELF32
# com32 modules), then install the resulting files into the native sysroot.
#
# Build-host requirements added by this append:
#   - 32-bit multilib GCC   (gcc -m32)  — needed for com32 ELF32 modules
#   - NASM                  (nasm-native already a DEPENDS of syslinux)
#
# Arch Linx one-liner:   sudo pacman -S gcc-multilib
# Debian/Ubuntu one-liner: sudo apt install gcc-multilib

# Build the full BIOS firmware (on top of just the installer)
do_compile:append:class-native() {
    oe_runmake firmware="bios" bios
}

do_install:append:class-native() {
    install -d ${D}${datadir}/syslinux/bios

    # isolinux.bin — ISOLINUX boot loader (written to El Torito boot image)
    install -m 0644 ${B}/bios/core/isolinux.bin \
        ${D}${datadir}/syslinux/bios/isolinux.bin

    # memdisk — boot helper that presents a disk image as a BIOS disk
    install -m 0644 ${B}/bios/memdisk/memdisk \
        ${D}${datadir}/syslinux/bios/memdisk

    # mbr.bin — 440-byte MBR code that loads the active partition's VBR
    install -m 0644 ${B}/bios/mbr/mbr.bin \
        ${D}${datadir}/syslinux/bios/mbr.bin

    # ldlinux.c32 — required COM32 module for isolinux >= 5.x
    install -m 0644 ${B}/bios/com32/elflink/ldlinux/ldlinux.c32 \
        ${D}${datadir}/syslinux/bios/ldlinux.c32
}
