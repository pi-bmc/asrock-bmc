project(
    'supermicro-psu-monitor',
    'cpp',
    version: '1.0',
    default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=false'],
)

# Boost.Asio (used by sdbusplus/asio) is header-only and staged into the
# sysroot's default include path via the recipe's DEPENDS on boost, so it needs
# no meson dependency object. Only sdbusplus and pthread require linking.
deps = [
    dependency('sdbusplus'),
    dependency('threads'),
]

executable(
    'supermicro-psu-monitor',
    'supermicro-psu-monitor.cpp',
    dependencies: deps,
    install: true,
    install_dir: get_option('bindir'),
)
