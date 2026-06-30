# Force the NO_YAML branch in scripts/dtc/Makefile.
#
# The Makefile detects libyaml availability by checking
# `$(wildcard $(PKG_CONFIG_SYSROOT_DIR)/usr/include/yaml.h)`. For native
# recipes PKG_CONFIG_SYSROOT_DIR is empty, so the wildcard finds the host's
# /usr/include/yaml.h (present on Arch / many distros) and the build adds
# yamltree.o. But `pkg-config --libs yaml-0.1` is sysroot-scoped and returns
# empty, so -lyaml never reaches the link command and dtc fails to link.
#
# Rewriting the conditional to `ifeq (,)` (always-true) routes the build into
# the NO_YAML branch unconditionally, dropping yamltree.o.
do_configure:append() {
    if [ -f ${S}/scripts/dtc/Makefile ]; then
        sed -i 's|ifeq (\$(wildcard \$(PKG_CONFIG_SYSROOT_DIR)/usr/include/yaml.h),)|ifeq (,)|' \
            ${S}/scripts/dtc/Makefile
    fi
}
