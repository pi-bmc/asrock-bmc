FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Build the web UI from the pi-bmc fork instead of openbmc/webui-vue upstream.
# Override both the git URL and SRCREV — the upstream recipe pins its own
# SRCREV, so it must be overridden in lockstep with the URL.
SRC_URI = "git://github.com/pi-bmc/webui-vue.git;branch=master;protocol=https"
SRCREV = "255a5ac9d31a437efbb6c4253c5d1c82fa0fa181"
