#!/bin/sh
# x570d4i2t-vga-enable monitor: re-assert the AST2500 host VGA (VGA_EN) every
# time the host chassis powers on.
#
# Why this exists:
#   x570d4i2t-vga-enable.service is a oneshot that runs ONCE, at BMC boot,
#   before the host is powered. But a host *cold* power-cycle — e.g. Redfish
#   BootSourceOverrideTarget=BiosSetup ("Once") followed by a cold cycle, or any
#   power off/on — occurs in the AST2500 "VGA / never-reset" SCU domain, which a
#   cold reset can leave with VGA_EN CLEARED. When that happens the host
#   re-enumerates the non-VGA BMC/XDMA PCIe device (1a03:2402) instead of the
#   VGA controller (1a03:2000), so the aspeed-video engine has no signal and the
#   KVM is blank. It is most visible in BIOS Setup: a static screen with no OS
#   to re-init video, so the blank frame never refreshes.
#
#   The link-sever watchdog (aspeed-video-watchdog) only recovers this when a
#   KVM client is already connected — it keys off obmc-ikvm's "Link has been
#   severed" log. If nobody is connected across the power cycle (the common
#   case for "enter Setup, then open the KVM"), VGA_EN is never re-asserted.
#
#   This monitor closes that gap: it watches D-Bus for the chassis/host
#   power-on transition and re-runs the vga-enable actuator unconditionally,
#   independent of any KVM client, so VGA_EN is set again BEFORE the host BIOS
#   enumerates PCIe.
#
# Why D-Bus and not a systemd power-on target:
#   This board's power control is daemon-based and publishes no obmc-*poweron@
#   targets (verified: systemctl lists none). xyz.openbmc_project.State.Chassis
#   / State.Host PropertiesChanged is the canonical, stack-agnostic signal, and
#   the match is scoped to the two state object paths so the loop stays idle in
#   steady state (the sensor bus is otherwise very chatty).

VGA_ENABLE=/usr/libexec/x570d4i2t-vga-enable.sh
COOLDOWN=5
last=0

log() { logger -t x570-vga-monitor "$*"; }

assert_vga() {
    [ -x "$VGA_ENABLE" ] || { log "actuator $VGA_ENABLE missing"; return; }
    "$VGA_ENABLE" 2>/dev/null || true
}

reassert() {
    now=$(date +%s)
    # Debounce a burst of PropertiesChanged signals (chassis On + host Running
    # arrive within the same power-on). Re-asserting is idempotent, but there
    # is no need to run it several times per boot.
    if [ $((now - last)) -lt "$COOLDOWN" ]; then
        return
    fi
    last=$now
    log "power-on transition — re-asserting host VGA (VGA_EN)"
    assert_vga
}

# Assert once at startup too: if this monitor (re)started after a power event
# — e.g. a dbus restart — VGA_EN may already need re-forcing.
assert_vga

log "started — watching chassis0/host0 power state for power-on"

# Scope the match tightly to the two state object paths. member/path filtering
# keeps sensor PropertiesChanged (hundreds/sec) out of the read loop.
busctl monitor \
    --match="type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/xyz/openbmc_project/state/chassis0'" \
    --match="type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/xyz/openbmc_project/state/host0'" \
    2>/dev/null | \
    while read -r line; do
        case "$line" in
            *"PowerState.On"*|*"HostState.Running"*) reassert ;;
        esac
    done

# The pipe only closes if busctl monitor exits (e.g. the message bus was
# restarted). Return so systemd (Restart=always) brings us back up.
log "busctl monitor stream ended — exiting for restart"
