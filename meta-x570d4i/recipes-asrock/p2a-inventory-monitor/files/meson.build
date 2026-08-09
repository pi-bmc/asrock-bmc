project(
    'p2a-inventory-monitor',
    'cpp',
    version: '1.0',
    default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=false'],
)

# Boost.Asio (used by sdbusplus/asio) is header-only and staged into the
# sysroot's default include path via the recipe's DEPENDS on boost, so it needs
# no meson dependency object. Only sdbusplus and pthread require linking. The
# host<->BMC blob header (oob-inventory-blob.h) is unpacked alongside the source.
deps = [
    dependency('sdbusplus'),
    dependency('threads'),
]

executable(
    'p2a-inventory-monitor',
    'p2a-inventory-monitor.cpp',
    dependencies: deps,
    install: true,
    install_dir: get_option('bindir'),
)
