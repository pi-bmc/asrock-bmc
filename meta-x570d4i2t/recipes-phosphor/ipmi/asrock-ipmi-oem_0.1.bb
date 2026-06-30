SUMMARY = "ASRock X570D4I-2T IPMI request logger"
DESCRIPTION = "\
A minimal phosphor-ipmi-host provider for the X570D4I-2T that registers a \
pass-through IPMI request *filter* (kcsmonitor): it logs every inbound IPMI \
message — including unhandled NetFn/Cmd pairs the host BIOS sends over KCS — to \
the journal, tagged with the channel, as a POST / blank-screen sanity check. \
\
All prior custom OEM command and SMBIOS-synthesis handlers (and the abandoned \
Redfish-Host-Interface code) were removed.  The host now pushes its full SMBIOS \
table to the BMC over KCS via the STANDARD smbios-ipmi-blob (\"/smbios\") receiver \
(see smbios-mdr_%.bbappend), driven by the injected SmbiosBmcPushDxe \
(recipes-bsp/host-bios-image); standard phosphor providers serve everything else. \
"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://meson.build \
    file://meson.options \
    file://src/kcsmonitor.cpp \
    "

S = "${UNPACKDIR}"
DEPENDS = " \
    boost \
    phosphor-ipmi-host \
    phosphor-logging \
    sdbusplus \
    "

inherit meson pkgconfig obmc-phosphor-ipmiprovider-symlink systemd

# Library name must match the library() target in meson.build
LIBRARY_NAMES = "libzasrockoemcmds.so"

# Register the provider for the host-side IPMI daemon (phosphor-ipmi-host)
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"
NETIPMI_PROVIDER_LIBRARY  += "${LIBRARY_NAMES}"

FILES:${PN}:append = " \
    ${libdir}/ipmid-providers/lib*${SOLIBS} \
    ${libdir}/host-ipmid/lib*${SOLIBS} \
    ${libdir}/net-ipmid/lib*${SOLIBS} \
    "
FILES:${PN}-dev:append = " ${libdir}/ipmid-providers/lib*${SOLIBSDEV}"
