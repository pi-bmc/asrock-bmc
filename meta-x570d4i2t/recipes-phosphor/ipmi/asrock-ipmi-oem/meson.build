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

asrock_oem_deps = [
    ipmid_dep,
    phosphor_logging_dep,
    sdbusplus_dep,
]

# -----------------------------------------------------------------------
# Shared library – loaded by phosphor-ipmi-host at runtime
# -----------------------------------------------------------------------

# Reduced to the IPMI request logger only. All custom OEM command/SMBIOS-synth
# handlers were removed: SMBIOS now flows host -> BMC via the standard
# smbios-ipmi-blob (/smbios) receiver (see smbios-mdr_%.bbappend) fed by our
# injected SmbiosBmcPushDxe; standard phosphor providers handle the rest.
# kcsmonitor is a pass-through filter that logs every inbound IPMI request
# (incl. unhandled ones) — kept as a blank-screen / POST sanity check.
asrockoemcmds_src = [
    'src/kcsmonitor.cpp',
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
