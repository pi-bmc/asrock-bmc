project(
    'asrock-ipmi-oem',
    'cpp',
    version: '0.1',
    meson_version: '>=1.1.1',
    default_options: [
        'warning_level=3',
        'werror=false',
        'cpp_std=c++23',
    ],
)

# Boost and ASIO are header-only in phosphor; suppress thread noise
add_project_arguments(
    [
        '-DBOOST_ERROR_CODE_HEADER_ONLY',
        '-DBOOST_SYSTEM_NO_DEPRECATED',
        '-DBOOST_ALL_NO_LIB',
        '-DBOOST_NO_RTTI',
        '-DBOOST_NO_TYPEID',
        '-DBOOST_ASIO_DISABLE_THREADS',
        '-DBOOST_COROUTINES_NO_DEPRECATION_WARNING',
        # Silence PSABI/pedantic warnings that fire for packed structs
        '-Wno-psabi',
        '-Wno-pedantic',
    ],
    language: 'cpp',
)

# -----------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------

phosphor_logging_dep     = dependency('phosphor-logging')
sdbusplus_dep            = dependency('sdbusplus')
ipmid_dep                = dependency('libipmid')
nlohmann_json_dep        = dependency('nlohmann_json')
liblzma_dep              = dependency('liblzma')

# The DSP0270 bootstrap handler calls into phosphor-ipmi-host's user and channel
# layers. These are NOT exported by the ipmid executable -- they live in their
# own shared libraries, so relying on the provider's b_lundef=false to resolve
# them at dlopen time fails with "undefined symbol: ipmi::ipmiSetSpecialUserPassword".
# They must be linked explicitly.
userlayer_dep            = dependency('libuserlayer')
channellayer_dep         = dependency('libchannellayer')

tinyxml_dep = dependency(
    'tinyxml2',
    default_options: ['tests=false'],
    include_type: 'system',
)

asrock_oem_deps = [
    ipmid_dep,
    phosphor_logging_dep,
    sdbusplus_dep,
    nlohmann_json_dep,
    liblzma_dep,
    tinyxml_dep,
    userlayer_dep,
    channellayer_dep,
]

# -----------------------------------------------------------------------
# Shared library – loaded by phosphor-ipmi-host at runtime
# -----------------------------------------------------------------------

# Two components:
#   * kcsmonitor.cpp   — pass-through filter that logs every inbound IPMI
#     request (incl. unhandled ones); a blank-screen / POST sanity check.
#   * biosconfigcommands.cpp — the OpenBMC "BIOS OOB config" IPMI command set
#     (NetFn 0x30, cmd 0xD3-0xD8 — the Intel path the AMI BIOS actually drives):
#     the host pushes its attribute registry as JSON over KCS and the BMC
#     populates xyz.openbmc_project.BIOSConfig.Manager (BaseBIOSTable), backing
#     the stock bmcweb /redfish/v1/Systems/system/Bios endpoints.
#
# SMBIOS still flows host -> BMC via the standard smbios-ipmi-blob (/smbios)
# receiver (see smbios-mdr_%.bbappend) fed by the injected SmbiosBmcPushDxe;
# standard phosphor providers handle everything else.
#   * biosvarstore.cpp — the two payload formats that carry live values:
#     type 2 "ASVS" (host -> BMC, the backing UEFI variables as the firmware
#     read them) and type 3 "ASPS" (BMC -> host, PendingAttributes encoded as
#     varstore writes).
#   * redfishhicommands.cpp — DMTF DSP0270 credential bootstrapping over KCS
#     (NetFn 0x2C, group extension 0x52, cmd 0x02 "Get Bootstrap Account
#     Credentials"). Upstream phosphor-ipmi-host registers no 0x2C group beyond
#     DCMI/SBMR, so a DSP0270-conformant BIOS otherwise gets 0xC1 and has no way
#     to authenticate to the Redfish host interface on usb0.
#   * amioemcommands.cpp — accept-and-log responder for the AMI OEM handshake
#     the stock BIOS drives during POST (NetFn 0x3A: BD BE B5 B2 F3 A1 AB;
#     NetFn 0x32: 72 5D 3D). Unanswered, the BIOS retries each ten times and
#     aborts before it ever reaches the NetFn 0x30 config transfer above.
#
# ---------------------------------------------------------------------------
# NO DIRECT FLASH ACCESS (refactored 2026-08-03)
# ---------------------------------------------------------------------------
# This package used to ship asrock-bios-nvram plus flash-sync/flash-monitor
# scripts, which pulled the BIOS SPI mux, bound spi1.0 and read/wrote /dev/mtd6
# to source BIOS attribute values. That is removed. It was actively harmful:
# leaving spi-nor bound starves the host of its own firmware (it powers on and
# executes nothing — no POST codes, no IPMI), and the monitor could rewrite the
# entire 32 MiB host BIOS unattended off a D-Bus power-state signal, on a BMC
# that has crashed under heavy MTD I/O.
#
# Attribute values now come from the BIOS itself over IPMI: the host pushes its
# registry and live varstore values via NetFn 0x30 (biosconfigcommands.cpp +
# biosvarstore.cpp) and the BMC publishes them to BaseBIOSTable. The BIOS is the
# authority on its own configuration; reading the flash behind its back was
# always a workaround for that path not completing.
asrockoemcmds_src = [
    'src/kcsmonitor.cpp',
    'src/biosconfigcommands.cpp',
    'src/biosvarstore.cpp',
    'src/redfishhicommands.cpp',
    'src/amioemcommands.cpp',
]

library(
    'zasrockoemcmds',
    sources: asrockoemcmds_src,
    include_directories: include_directories('.'),
    implicit_include_directories: false,
    dependencies: asrock_oem_deps,
    version: meson.project_version(),
    # Allow unresolved symbols – resolved by the ipmid host at load time
    override_options: ['b_lundef=false'],
    install: true,
    install_dir: get_option('libdir') / 'ipmid-providers',
)
