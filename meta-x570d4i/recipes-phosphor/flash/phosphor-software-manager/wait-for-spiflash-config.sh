#!/bin/sh
# Block until entity-manager has published the SPIFlash configuration.
#
# phosphor-bios-software-update queries the EM config on startup and throws an
# UNCAUGHT sdbusplus ResourceNotFound if it is not there yet, which aborts the
# process (SIGABRT + coredump). systemd then restarts it and the second attempt
# succeeds -- so the daemon works, but every boot leaves a core file and a
# scary "dumped core" in the journal.
#
# Unit ordering alone does not fix this: entity-manager reaches "started" long
# before it finishes publishing inventory objects onto D-Bus. Poll for the
# object instead.
#
# The interface name tracks the EM record's "Type", so it must stay in step
# with x570d4i2t.json. Upstream replaced the generic "SPIFlash" type with
# "HostSPIFlash" (phosphor-bmc-code-mgmt 03581d33, "bios: Fix D-Bus bindings
# and transition to HostSPIFlash types"); polling the retired name matches
# nothing, so the loop just burns all 60 iterations and gives up -- the daemon
# still comes up, but 60s late and with this guard silently doing nothing.
CONFIG_IFACE=xyz.openbmc_project.Configuration.HostSPIFlash

# Always exits 0 -- if the config genuinely never appears we let the daemon
# start and fail in its own way rather than silently blocking boot forever.
for _ in $(seq 1 60); do
    if busctl call xyz.openbmc_project.ObjectMapper \
            /xyz/openbmc_project/object_mapper \
            xyz.openbmc_project.ObjectMapper GetSubTree sias \
            / 0 1 "$CONFIG_IFACE" 2>/dev/null \
            | grep -q HostSPIFlash; then
        exit 0
    fi
    sleep 1
done
echo "wait-for-spiflash-config: $CONFIG_IFACE never appeared; starting anyway" >&2
exit 0
