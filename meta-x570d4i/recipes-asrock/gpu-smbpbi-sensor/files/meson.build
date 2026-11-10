project(
    'gpu-smbpbi-sensor',
    'cpp',
    version: '1.0',
    default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=false'],
)

# Boost.Asio (used by sdbusplus/asio) is header-only and staged into the
# sysroot's default include path via the recipe's DEPENDS on boost.
executable(
    'gpu-smbpbi-sensor',
    'gpu-smbpbi-sensor.cpp',
    dependencies: [dependency('sdbusplus')],
    install: true,
    install_dir: get_option('bindir'),
)
