#!/bin/sh
# asrock-bios-flash-monitor — run the BIOS flash sync whenever the host reaches
# a powered-off state.
#
# The BIOS flash is only reachable by the BMC while the host is off (the SPI mux
# has to move away from the host), so that transition is the one safe window in
# which we can read the live BIOS attribute values and push back any pending
# Redfish changes. Watching D-Bus is the stack-agnostic way to catch it: this
# board's power control is daemon-based and publishes no obmc-*poweroff@
# targets, so there is no systemd unit to order against.
#
# The match is scoped to the chassis0 object path so the read loop stays idle in
# steady state; the sensor bus is otherwise very chatty.

SYNC=/usr/libexec/asrock-bios-flash-sync.sh
# A sync moves the SPI mux and can rewrite the flash; never let power-state
# chatter start a second one on top of a running cycle.
LOCK=/run/asrock-bios-flash.lock

log() { logger -t asrock-bios-flash-monitor "$*"; }

run_sync() {
    if ! mkdir "$LOCK" 2>/dev/null; then
        log "sync already in progress, skipping"
        return
    fi
    log "host powered off - syncing BIOS attributes from flash"
    "$SYNC" auto >/dev/null 2>&1
    rmdir "$LOCK" 2>/dev/null
}

[ -x "$SYNC" ] || { log "$SYNC missing"; exit 1; }
rmdir "$LOCK" 2>/dev/null

# Catch the case where the host is already off at BMC start: without this the
# table stays empty until the next power transition, which may be days away.
state=$(busctl get-property xyz.openbmc_project.State.Chassis \
    /xyz/openbmc_project/state/chassis0 \
    xyz.openbmc_project.State.Chassis CurrentPowerState 2>/dev/null)
case "$state" in
    *PowerState.Off*) run_sync ;;
esac

log "started - watching chassis0 for power-off"

busctl monitor \
    --match="type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/xyz/openbmc_project/state/chassis0'" \
    2>/dev/null | \
    while read -r line; do
        case "$line" in
            *"PowerState.Off"*) run_sync ;;
        esac
    done

# Only reached if busctl exits (e.g. the bus restarted); systemd restarts us.
log "busctl monitor stream ended - exiting for restart"
