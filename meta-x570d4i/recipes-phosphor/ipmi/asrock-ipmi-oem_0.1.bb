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
    file://src/biosnvram.cpp \
    file://src/biosnvram.hpp \
    file://src/biosnvramtool.cpp \
    file://src/biosvarstore.cpp \
    file://src/biosvarstore.hpp \
    file://src/biosxml.hpp \
    file://src/redfishhicommands.cpp \
    file://src/types.hpp \
    file://files/asrock-bios-flash-sync.sh \
    file://files/asrock-bios-flash-monitor.sh \
    file://files/asrock-bios-flash-monitor.service \
    "

S = "${UNPACKDIR}"
DEPENDS = " \
    boost \
    libtinyxml2 \
    nlohmann-json \
    phosphor-ipmi-host \
    phosphor-logging \
    sdbusplus \
    xz \
    "

inherit meson pkgconfig obmc-phosphor-ipmiprovider-symlink systemd

# The BIOS flash is only reachable while the host is off, so the sync runs off a
# D-Bus power-state watcher rather than a boot-time oneshot.
SYSTEMD_SERVICE:${PN} += "asrock-bios-flash-monitor.service"

# Shipped DISABLED (2026-08-03). The monitor runs the sync in "auto" mode, which
# rewrites the *entire* 32 MiB host BIOS image with flashcp whenever
# PendingAttributes is non-empty -- unattended, triggered only by a chassis
# power-off signal. Three things make that unsafe on this board:
#
#   1. Heavy 32 MiB MTD I/O has repeatedly crashed the BMC (again 2026-08-03
#      05:13:50). A crash during the read is harmless; during the flashcp it
#      leaves a partially erased BIOS -- a brick.
#   2. The unit is Restart=always, and systemd kills the whole cgroup on
#      restart, so an in-flight flashcp can be killed mid-write. The mkdir lock
#      does not help: the script rmdir's it unconditionally at startup.
#   3. flashcp writes back a *cached snapshot*, so it would roll back the APOB
#      (AGESA memory-training output, 0x1509000) and APCB (PSP config block,
#      0x1796000) that the BIOS legitimately rewrites on every POST.
#
# It also yanks the BIOS SPI mux for ~20 s per run and re-fires on repeated
# PowerState.Off signals (6 runs in one BMC boot were observed), which is a
# boot-killer if the host powers on inside that window.
#
# Re-enable deliberately (systemctl enable --now asrock-bios-flash-monitor), or
# better, change the monitor's "$SYNC" auto invocation to "$SYNC" sync so the
# read/publish path stays automatic and the flash-write path stays manual.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_install:append() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/files/asrock-bios-flash-sync.sh \
        ${D}${libexecdir}/asrock-bios-flash-sync.sh
    install -m 0755 ${UNPACKDIR}/files/asrock-bios-flash-monitor.sh \
        ${D}${libexecdir}/asrock-bios-flash-monitor.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/files/asrock-bios-flash-monitor.service \
        ${D}${systemd_system_unitdir}/asrock-bios-flash-monitor.service
}

# Library name must match the library() target in meson.build
LIBRARY_NAMES = "libzasrockoemcmds.so"

# Register the provider for the host-side IPMI daemon (phosphor-ipmi-host)
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"
NETIPMI_PROVIDER_LIBRARY  += "${LIBRARY_NAMES}"

FILES:${PN}:append = " \
    ${libdir}/ipmid-providers/lib*${SOLIBS} \
    ${libdir}/host-ipmid/lib*${SOLIBS} \
    ${libdir}/net-ipmid/lib*${SOLIBS} \
    ${libexecdir}/asrock-bios-nvram \
    ${libexecdir}/asrock-bios-flash-sync.sh \
    ${libexecdir}/asrock-bios-flash-monitor.sh \
    "
FILES:${PN}-dev:append = " ${libdir}/ipmid-providers/lib*${SOLIBSDEV}"
