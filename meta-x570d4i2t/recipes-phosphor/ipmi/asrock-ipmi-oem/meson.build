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
    tinyxml_dep,
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
asrockoemcmds_src = [
    'src/kcsmonitor.cpp',
    'src/biosconfigcommands.cpp',
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
