SUMMARY = "ASRock X570D4I-2T IPMI OEM provider (request logger + BIOS config)"
DESCRIPTION = "\
A phosphor-ipmi-host provider for the X570D4I-2T with two components: \
\
1. kcsmonitor — a pass-through IPMI request *filter* that logs every inbound \
message, including unhandled NetFn/Cmd pairs the host BIOS sends over KCS, as a \
POST / blank-screen sanity check. \
\
2. biosconfigcommands — the OpenBMC \"BIOS OOB config\" IPMI command set (NetFn \
0x30, cmd 0xD3-0xD8 — the Intel path the AMI BIOS actually drives): the host \
pushes its BIOS attribute registry as JSON over KCS and the BMC populates \
xyz.openbmc_project.BIOSConfig.Manager (BaseBIOSTable), backing the stock bmcweb \
/redfish/v1/Systems/system/Bios endpoints. It is the in-band replacement for the \
removed USB Redfish Host Interface config path. \
\
SMBIOS still flows host -> BMC over KCS via the STANDARD smbios-ipmi-blob \
(\"/smbios\") receiver (see smbios-mdr_%.bbappend), driven by the injected \
SmbiosBmcPushDxe; standard phosphor providers serve everything else. \
"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://meson.build \
    file://meson.options \
    file://src/kcsmonitor.cpp \
    file://src/biosconfigcommands.cpp \
    file://src/biosconfigcommands.hpp \
    "

S = "${UNPACKDIR}"
DEPENDS = " \
    boost \
    nlohmann-json \
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
