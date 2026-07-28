FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Keep the KVM session alive across the host's flapping/severed aspeed-video
# signal. Without this, obmc-ikvm's uncaught elog<> exception on a lost V4L2
# link terminates the process (systemd restarts it), dropping the client every
# time the host video signal drops. See the patch header for detail.
SRC_URI += "file://0001-survive-video-link-severed.patch"
