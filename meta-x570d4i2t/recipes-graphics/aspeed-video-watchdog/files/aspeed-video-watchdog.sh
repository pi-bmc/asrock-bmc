#!/bin/sh
# aspeed-video-watchdog: reset the ASPEED V4L2 video engine when
# obmc-ikvm reports "Link has been severed" (ENOLINK from aspeed-video).
#
# The aspeed-video kernel driver sets ENOLINK when it loses the capture
# link (e.g. host power-cycle, mode change, or hardware glitch).  obmc-ikvm
# exits on ENOLINK, but the driver stays wedged — simply restarting ikvm
# with Restart=always is not enough.  This watchdog catches the sentinel
# log line, tears down the driver binding, then brings ikvm back up.

COOLDOWN=30
STAMP=/run/aspeed-video-watchdog.stamp
DRIVER=/sys/bus/platform/drivers/aspeed-video

log() { logger -t aspeed-video-watchdog "$*"; }

find_video_dev() {
    # Symlink names look like "1e700000.video"; skip the driver control
    # files which are plain alpha names (bind, unbind, uevent, …).
    for path in "$DRIVER"/*.*; do
        [ -e "$path" ] || continue
        name=${path##*/}
        case "$name" in
            [0-9a-f]*.*)
                printf '%s\n' "$name"
                return 0
                ;;
        esac
    done
}

reset_driver() {
    now=$(date +%s)
    if [ -f "$STAMP" ]; then
        last=$(cat "$STAMP" 2>/dev/null || echo 0)
        elapsed=$((now - last))
        if [ "$elapsed" -lt "$COOLDOWN" ]; then
            log "cooldown active (${elapsed}s < ${COOLDOWN}s), skipping reset"
            return
        fi
    fi
    printf '%s\n' "$now" > "$STAMP"

    log "link severed — stopping obmc-ikvm and resetting aspeed-video"
    systemctl stop obmc-ikvm.service 2>/dev/null || true

    # Re-assert the AST2500 host VGA PCIe function (SCU PCIE_CONF VGA_EN).
    # A link-sever often follows a host/SoC event that can drop the host's
    # view of the onboard VGA (1a03:2000); re-running the vga-enable oneshot
    # re-connects the PCIe VGA device before we rebind the capture engine, so
    # the host has a signal to capture. Harmless if VGA_EN was already set.
    if [ -x /usr/libexec/x570d4i2t-vga-enable.sh ]; then
        log "re-asserting host VGA PCIe enable (VGA_EN)"
        /usr/libexec/x570d4i2t-vga-enable.sh 2>/dev/null || true
    fi

    if lsmod 2>/dev/null | grep -q '^aspeed_video '; then
        modprobe -r aspeed_video 2>/dev/null || true
        sleep 1
        modprobe aspeed_video 2>/dev/null || true
    else
        dev=$(find_video_dev)
        if [ -n "$dev" ]; then
            log "unbind $dev"
            echo "$dev" > "$DRIVER/unbind" 2>/dev/null || true
            sleep 1
            log "bind $dev"
            echo "$dev" > "$DRIVER/bind" 2>/dev/null || true
        else
            log "aspeed-video device not found under $DRIVER — cannot reset"
        fi
    fi

    sleep 1
    log "restarting obmc-ikvm"
    systemctl start obmc-ikvm.service 2>/dev/null || true
}

log "started — monitoring obmc-ikvm for video link failures"

# Follow the obmc-ikvm unit journal.  --no-hostname -o cat keeps lines
# clean.  Match inside the read loop with a case glob instead of
# `grep --line-buffered` (a GNU-only option BusyBox grep rejects, which
# made this script exit immediately and trip the systemd start-limit).
journalctl -f -u obmc-ikvm.service --no-hostname -o cat 2>/dev/null | \
    while read -r line; do
        case "$line" in
            *"Link has been severed"*) reset_driver ;;
        esac
    done
