#!/bin/sh
# Called once as a PID1 child. Only two known vendor drivers are considered.
# No vendor daemon, touch-MCU firmware trigger, writable disk or unload path.
PATH=/bin:/sbin
export PATH
umask 077
B=/bin/busybox
R=/run/s12
V=$R/system0
L=$V/lib/ld-linux-armhf.so.3
P=$V/lib:$V/usr/lib
[ -x "$B" ] && [ -x "$L" ] || exit 1
[ "$("$B" cat "$R/supervisor.pid" 2>/dev/null)" = 1 ] || exit 1
[ "$("$B" cat "$R/supervisor.tag" 2>/dev/null)" = S12_CUSTOM_RUNTIME_V1 ] || exit 1
case "$("$B" cat "$R/state" 2>/dev/null)" in starting|ready) :;; *) exit 1;; esac
[ -d "$R" ] && [ ! -L "$R" ] || exit 1
"$B" mkdir -m 700 "$R/panel.lock" 2>/dev/null || exit 1
printf '%s\n' "$$" > "$R/panel.pid" || exit 1
printf 'starting\n' > "$R/panel.state"
IFS= read -r expected_panel < /s12/panel.sha256 || exit 1
case "$expected_panel" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#expected_panel}" -eq 64 ] || exit 1
panel_digest=$("$B" sha256sum /s12/panel.arm) || exit 1
[ "${panel_digest%% *}" = "$expected_panel" ] || exit 1
"$L" --library-path "$P" /s12/panel.arm --selftest > "$R/panel-selftest.log" 2>&1 || exit 1

load_panel_module() {
    panel_module=$1
    case "$panel_module" in cyttsp_button|is31fl3236a) :;; *) return 1;; esac
    [ ! -d "/sys/module/$panel_module" ] || return 0
    # Directory creation is the one-attempt marker. Never retry a partial load.
    "$B" mkdir -m 700 "$R/panel-$panel_module-attempted" 2>/dev/null || return 1
    [ -f "$V/lib/modules/4.9.61/$panel_module.ko" ] || return 1
    /sbin/insmod "$V/lib/modules/4.9.61/$panel_module.ko" > "$R/panel-$panel_module.log" 2>&1
}
load_panel_module cyttsp_button || printf 'touch driver unavailable\n' > "$R/panel-touch.degraded"
load_panel_module is31fl3236a || printf 'LED driver unavailable\n' > "$R/panel-led.degraded"

panel_node_is() {
    [ ! -L "$1" ] && [ -c "$1" ] || return 1
    "$B" ls -ln "$1" | "$B" grep -Eq "^c[^ ]* +[0-9]+ +0 +0 +13, +$2 +"
}
if [ ! -e /dev/input ] && [ ! -L /dev/input ]; then "$B" mkdir -m 700 /dev/input; fi
if [ -d /dev/input ] && [ ! -L /dev/input ]; then
    for panel_sys in /sys/class/input/event*; do
        [ -f "$panel_sys/device/name" ] && [ -f "$panel_sys/dev" ] && [ -f "$panel_sys/uevent" ] || continue
        IFS= read -r panel_name < "$panel_sys/device/name" || continue
        [ "$panel_name" = keys-cyttsp ] || continue
        panel_event=${panel_sys##*/}
        panel_number=${panel_event#event}
        case "$panel_number" in ''|*[!0-9]*) continue;; esac
        [ "$panel_number" -le 255 ] || continue
        IFS= read -r panel_dev < "$panel_sys/dev" || continue
        case "$panel_dev" in 13:*) panel_minor=${panel_dev#13:};; *) continue;; esac
        case "$panel_minor" in ''|*[!0-9]*) continue;; esac
        [ "$panel_minor" -le 255 ] || continue
        panel_devname=
        while IFS= read -r panel_line; do
            case "$panel_line" in DEVNAME=*) panel_devname=${panel_line#DEVNAME=};; esac
        done < "$panel_sys/uevent"
        [ "$panel_devname" = "input/$panel_event" ] || continue
        panel_node=/dev/input/$panel_event
        if [ ! -e "$panel_node" ] && [ ! -L "$panel_node" ]; then
            "$B" mknod -m 600 "$panel_node" c 13 "$panel_minor" || continue
        fi
        panel_node_is "$panel_node" "$panel_minor" || continue
    done
fi
panel_keymap=103,108,28,105,106
if [ -f /s12/panel.keymap ] && [ ! -L /s12/panel.keymap ]; then
    IFS= read -r panel_keymap < /s12/panel.keymap || exit 1
fi
set -- --keymap "$panel_keymap"
# Explicit packaging opt-in only after validating channel order on hardware.
if [ -f /s12/panel-rgb-confirmed ] && [ ! -L /s12/panel-rgb-confirmed ]; then
    IFS= read -r panel_rgb < /s12/panel-rgb-confirmed || exit 1
    [ "$panel_rgb" != yes ] || set -- "$@" --rgb-confirmed
fi
exec "$L" --library-path "$P" /s12/panel.arm "$@" > "$R/panel.log" 2>&1
