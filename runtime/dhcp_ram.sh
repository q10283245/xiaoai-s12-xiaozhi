#!/bin/sh
# Temporary wlan0-only DHCP hook. All files written here reside in RAM.
[ "$interface" = wlan0 ] || exit 1
B=/bin/busybox
set -f
umask 077
valid_v4() {
    case "$1" in ''|*[!0-9.]*) return 1;; esac
    case "$1" in .*|*.|*..*) return 1;; esac
    saved_ifs=$IFS;IFS=.;set -- $1;IFS=$saved_ifs
    [ "$#" -eq 4 ] || return 1
    for octet in "$@"; do
        [ "${#octet}" -le 3 ] && [ "$octet" -le 255 ] || return 1
    done
    return 0
}
ram_dns() {
    [ ! -L /etc/resolv.conf ] && [ ! -L /etc/resolv.conf.new ] || return 1
    "$B" mkdir -p /etc || return 1
    : > /etc/resolv.conf.new || return 1
    dns_count=0
    for server in $dns; do
        valid_v4 "$server" || continue
        case "$server" in 0.*|127.*|255.*) continue;; esac
        printf 'nameserver %s\n' "$server" >> /etc/resolv.conf.new || return 1
        dns_count=$((dns_count+1));[ "$dns_count" -lt 3 ] || break
    done
    if [ "$dns_count" -eq 0 ]; then
        printf 'nameserver 223.5.5.5\nnameserver 1.1.1.1\n' > /etc/resolv.conf.new || return 1
    fi
    "$B" chmod 644 /etc/resolv.conf.new && "$B" mv -f /etc/resolv.conf.new /etc/resolv.conf
}
case "$1" in
    deconfig)
        "$B" ifconfig wlan0 0.0.0.0
        "$B" rm -f /tmp/s12-dhcp-status
        ;;
    bound|renew)
        valid_v4 "$ip" && valid_v4 "$subnet" || exit 1
        "$B" ifconfig wlan0 "$ip" netmask "$subnet" || exit 1
        gateway=${router%% *}
        if valid_v4 "$gateway"; then
            "$B" route del default dev wlan0 2>/dev/null
            "$B" route add default gw "$gateway" dev wlan0 || exit 1
        fi
        ram_dns || exit 1
        printf 'bound=%s\nip=%s\n' "$1" "$ip" > /tmp/s12-dhcp-status
        ;;
    *) exit 0;;
esac
