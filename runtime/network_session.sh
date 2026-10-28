#!/bin/sh
# RAM-only Wi-Fi reconnection supervisor. Sourcing defines functions only.
set -f
umask 077
NET_STOP=
NET_WPA_PID= NET_WPA_ID=
NET_DHCP_PID= NET_DHCP_ID=
NET_SLEEP_PID= NET_SLEEP_ID=

network_identity() {
    case "$1" in ''|*[!0-9]*) return 1;; esac
    IFS= read -r net_stat < "$S12_PROC/$1/stat" 2>/dev/null || return 1
    case "$net_stat" in "$1 ("*") "*) :;; *) return 1;; esac
    net_fields=${net_stat##*) }; set -- $net_fields
    [ "$#" -ge 20 ] || return 1
    net_parent=$2;shift 19;net_started=$1
    case "$net_parent:$net_started" in *[!0-9:]*|:*) return 1;; esac
    printf '%s:%s\n' "$net_parent" "$net_started"
}
network_owned() {
    [ -n "$1" ] && [ -n "$2" ] || return 1
    net_current=$(network_identity "$1") || return 1
    [ "$net_current" = "$2" ] || return 1
    case "$net_current" in "$$":*) return 0;; *) return 1;; esac
}
network_signal() {
    NET_STOP=yes
    if network_owned "$NET_WPA_PID" "$NET_WPA_ID"; then "$B" kill -TERM "$NET_WPA_PID" 2>/dev/null; fi
    if network_owned "$NET_DHCP_PID" "$NET_DHCP_ID"; then "$B" kill -TERM "$NET_DHCP_PID" 2>/dev/null; fi
    if network_owned "$NET_SLEEP_PID" "$NET_SLEEP_ID"; then "$B" kill -TERM "$NET_SLEEP_PID" 2>/dev/null; fi
}
network_state() {
    case "$1" in offline|link|ready|stopped) :;; *) return 1;; esac
    [ ! -L "$R/network.state" ] && [ ! -L "$R/network.state.new" ] || return 1
    printf '%s\n' "$1" > "$R/network.state.new" && "$B" mv -f "$R/network.state.new" "$R/network.state"
}
network_sleep() {
    [ -z "$NET_STOP" ] || return 1
    "$B" sleep 5 </dev/null &
    NET_SLEEP_PID=$!; NET_SLEEP_ID=$(network_identity "$NET_SLEEP_PID")
    [ -z "$NET_STOP" ] || network_signal
    wait "$NET_SLEEP_PID" 2>/dev/null
    NET_SLEEP_PID= NET_SLEEP_ID=
    [ -z "$NET_STOP" ]
}
network_run() {
    [ "$#" -eq 0 ] || return 2
    [ "$("$B" cat "$R/supervisor.pid" 2>/dev/null)" = 1 ] || return 1
    [ "$("$B" cat "$R/supervisor.tag" 2>/dev/null)" = S12_CUSTOM_RUNTIME_V1 ] || return 1
    case "$(network_identity "$$")" in 1:*) :;; *) return 1;; esac
    [ ! -L "$R" ] && [ -d "$R" ] && [ -x "$L" ] || return 1
    "$B" mkdir -m 700 "$R/network.lock" 2>/dev/null || return 1
    trap 'network_signal' TERM INT HUP
    trap 'network_signal; network_state stopped' 0
    printf '%s\n' "$$" > "$R/network.pid" || return 1
    network_state offline || return 1
    while [ -z "$NET_STOP" ]; do
        if ! network_owned "$NET_WPA_PID" "$NET_WPA_ID"; then
            [ -z "$NET_WPA_PID" ] || wait "$NET_WPA_PID" 2>/dev/null
            "$L" --library-path "$P" "$V/usr/sbin/wpa_supplicant" \
                -D nl80211 -i wlan0 -c "$S12DIR/private/wpa.conf" -C "$R/wpa-ctrl" \
                </dev/null > "$R/wpa.log" 2>&1 &
            NET_WPA_PID=$!;NET_WPA_ID=$(network_identity "$NET_WPA_PID")
            printf '%s\n' "$NET_WPA_PID" > "$R/wpa.pid"
        fi
        [ -z "$NET_STOP" ] || break
        if ! network_owned "$NET_DHCP_PID" "$NET_DHCP_ID"; then
            [ -z "$NET_DHCP_PID" ] || wait "$NET_DHCP_PID" 2>/dev/null
            # No -n/-q: absence of an AP/lease never terminates local voice.
            "$B" udhcpc -f -i wlan0 -s "$S12DIR/dhcp_ram.sh" -t 4 -T 3 -A 20 -p "$R/udhcpc.pid" \
                </dev/null > "$R/dhcp.log" 2>&1 &
            NET_DHCP_PID=$!;NET_DHCP_ID=$(network_identity "$NET_DHCP_PID")
        fi
        [ -z "$NET_STOP" ] || break
        "$L" --library-path "$P" "$V/usr/sbin/wpa_cli" -p "$R/wpa-ctrl" -i wlan0 status \
            > "$R/wpa-status" 2> "$R/wpa-status.err"
        if "$B" grep -qx 'wpa_state=COMPLETED' "$R/wpa-status"; then
            if [ -s /tmp/s12-dhcp-status ]; then network_state ready; else network_state link; fi
        else
            network_state offline
        fi
        network_sleep || break
    done
    network_signal
    return 0
}
