FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Build the web UI from the pi-bmc fork instead of openbmc/webui-vue upstream.
# Override both the git URL and SRCREV — the upstream recipe pins its own
# SRCREV, so it must be overridden in lockstep with the URL.
SRC_URI = "git://github.com/pi-bmc/webui-vue.git;branch=bootstrap-dark;protocol=https"
SRCREV = "8b437065e9fb3f0d11de1c70108db5f093d3c70e"
