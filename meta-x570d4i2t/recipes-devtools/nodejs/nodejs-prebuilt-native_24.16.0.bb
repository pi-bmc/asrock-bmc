# Provides nodejs-native from the official prebuilt x86-64 binaries on
# nodejs.org instead of meta-oe's from-source build (a ~30 CPU-minute C++
# compile). webui-vue is the only consumer of nodejs-native in this build
# and it only invokes the `node`/`npm` CLIs, so nothing links against node
# and the prebuilt binaries are a drop-in replacement.
#
# Selected via PREFERRED_PROVIDER_nodejs-native (kas.yml / local.conf.sample).
# Keep PV in step with meta-oe's nodejs recipe when bumping the openbmc pin;
# checksums come from https://nodejs.org/dist/v${PV}/SHASUMS256.txt.
SUMMARY = "Prebuilt Node.js binaries for the build host"
HOMEPAGE = "https://nodejs.org"
LICENSE = "MIT & ISC & BSD-2-Clause & BSD-3-Clause & Artistic-2.0 & Apache-2.0 & BlueOak-1.0.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=9f816753e8bdfe4576cb87159a0cd60c"

PROVIDES = "nodejs-native"

SRC_URI = "https://nodejs.org/dist/v${PV}/node-v${PV}-linux-x64.tar.xz"
SRC_URI[sha256sum] = "d804845d34eddc21dc1092b519d643ef40b1f58ec5dec5c22b1f4bd8fabde6c9"

S = "${UNPACKDIR}/node-v${PV}-linux-x64"

inherit native

# The official binaries are x86-64 only and need glibc >= 2.28 on the
# build host.
COMPATIBLE_HOST = "x86_64.*-linux"

# The binaries ship pre-stripped; don't let sysroot staging touch them.
INHIBIT_SYSROOT_STRIP = "1"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${prefix}
    # -a preserves the bin/npm -> ../lib/node_modules/... symlinks; include/
    # carries the node headers so node-gyp doesn't have to download them if
    # an npm dependency compiles a native addon.
    cp -a --no-preserve=ownership ${S}/bin ${S}/include ${S}/lib ${S}/share ${D}${prefix}/
}
