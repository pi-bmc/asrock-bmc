#!/bin/sh
# asrock-bios-flash-sync — keep BaseBIOSTable backed by the real BIOS flash,
# and push Redfish attribute changes back into it.
#
# Why: the XML the BIOS pushes over KCS carries a trustworthy schema but its
# CurrentVal is the knob's own *default* on every knob and every boot, so it can
# never show the live configuration. The live values live in AMI NVAR variables
# in the 32 MB BIOS SPI flash. The BMC can reach that flash only while the host
# is powered off, by steering the SPI mux to the BMC and binding spi-nor.
#
# Ordering matters and is not negotiable:
#   * the host must be OFF before the mux moves, or the running host loses the
#     flash it is executing from;
#   * pending writes are applied BEFORE the read-back, so what we publish is
#     what the BIOS will actually see on the next POST.
#
# Usage: asrock-bios-flash-sync.sh [sync|apply|auto]
#   sync   read flash -> publish BaseBIOSTable
#   apply  fold PendingAttributes into the flash (writes!), then re-publish
#   auto   apply if anything is pending, otherwise just sync   [default]

set -u

TOOL=/usr/libexec/asrock-bios-nvram
MUX_LINE=control-bios-spi-mux-n
SPI_DEV=spi1.0
SPI_DRV=/sys/bus/spi/drivers/spi-nor
# 32MB does not fit in the rwfs overlay (~20MB free); /tmp is tmpfs.
WORK=/tmp/asrock-bios-flash
IMG="$WORK/bios.bin"
NEW="$WORK/bios-new.bin"

MODE="${1:-auto}"
GPID=""

log() { logger -t asrock-bios-flash "$*"; echo "asrock-bios-flash: $*"; }

host_is_off() {
    state=$(busctl get-property xyz.openbmc_project.State.Chassis \
        /xyz/openbmc_project/state/chassis0 \
        xyz.openbmc_project.State.Chassis CurrentPowerState 2>/dev/null)
    case "$state" in
        *PowerState.Off*) return 0 ;;
        *) return 1 ;;
    esac
}

release_flash() {
    # Always give the flash back to the host, even on error paths: leaving the
    # mux on the BMC means the host cannot fetch its BIOS and will not boot.
    echo "$SPI_DEV" > "$SPI_DRV/unbind" 2>/dev/null
    [ -n "$GPID" ] && kill "$GPID" 2>/dev/null
    GPID=""
    gpioset "$(gpiofind $MUX_LINE)"=1 2>/dev/null
    rm -rf "$WORK"
}
trap release_flash EXIT INT TERM

claim_flash() {
    line=$(gpiofind "$MUX_LINE") || { log "GPIO $MUX_LINE not found"; return 1; }
    # Hold the mux low (BMC side) for as long as this process lives.
    gpioset --mode=signal $line=0 &
    GPID=$!
    sleep 1
    echo "$SPI_DEV" > "$SPI_DRV/bind" 2>/dev/null
    sleep 1
    MTD=$(grep -i '"bios"' /proc/mtd | cut -d: -f1)
    [ -n "$MTD" ] || { log "no bios MTD after bind"; return 1; }
    return 0
}

if [ ! -x "$TOOL" ]; then
    log "$TOOL missing"
    exit 1
fi

if ! host_is_off; then
    log "host is not powered off; refusing to touch the BIOS flash"
    exit 1
fi

mkdir -p "$WORK"
claim_flash || exit 1

log "reading /dev/$MTD"
if ! dd if="/dev/$MTD" of="$IMG" bs=65536 2>/dev/null; then
    log "flash read failed"
    exit 1
fi

do_apply=0
if [ "$MODE" = "apply" ]; then
    do_apply=1
elif [ "$MODE" = "auto" ]; then
    # apply exits 2 when there is nothing pending, so probe cheaply.
    "$TOOL" apply "$IMG" "$NEW" >/dev/null 2>&1
    [ $? -eq 0 ] && do_apply=1
fi

if [ "$do_apply" = "1" ]; then
    if "$TOOL" apply "$IMG" "$NEW"; then
        log "writing changed attributes back to /dev/$MTD"
        # flashcp erases as it goes: a 0->1 bit (e.g. 0x02 -> 0x01) cannot be
        # programmed in place on NOR, so the whole image is rewritten.
        if flashcp "$NEW" "/dev/$MTD"; then
            log "flash updated; clearing PendingAttributes"
            busctl set-property xyz.openbmc_project.BIOSConfigManager \
                /xyz/openbmc_project/bios_config/manager \
                xyz.openbmc_project.BIOSConfig.Manager \
                PendingAttributes "a{s(sv)}" 0 2>/dev/null
            cp "$NEW" "$IMG"
        else
            log "FLASH WRITE FAILED - PendingAttributes left in place"
        fi
    fi
fi

# Publish whatever is actually in the flash now.
"$TOOL" sync "$IMG" || log "sync failed"

log "done ($MODE)"
