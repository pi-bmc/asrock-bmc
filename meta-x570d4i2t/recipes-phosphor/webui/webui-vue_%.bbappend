FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Build the web UI from the pi-bmc fork instead of openbmc/webui-vue upstream.
# Override both the git URL and SRCREV — the upstream recipe pins its own
# SRCREV, so it must be overridden in lockstep with the URL.
SRC_URI = "git://github.com/pi-bmc/webui-vue.git;branch=master;protocol=https"
SRCREV = "2f44d44efc153205f8c96942ef444a35c1525047"
