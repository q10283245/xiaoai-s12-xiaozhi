#!/bin/sh
# Experimental original-board runtime. All mutable state is RAM. Only system0
# is mounted, read-only. No env writer, updater, writable disk or vendor daemon.
PATH=/bin:/sbin
export PATH
umask 077
B=/bin/busybox R=/run/s12
V=$R/system0 N=$R/system0-block
L=$V/lib/ld-linux-armhf.so.3 P=$V/lib:$V/usr/lib
[ "$$" -eq 1 ] && [ -x "$B" ] || exit 1
trap ':' INT TERM HUP
ulimit -c 0
rescue() {
    while :; do /bin/sh -i </dev/console >/dev/console 2>&1; "$B" sleep 1; done
}
fail() {
    echo "S12_XIAOZHI: startup failed at $1; RAM console retained"
    [ ! -d "$R" ] || printf 'failed\n' > "$R/state"
    rescue
}
basic_node() {
    if [ ! -e "$1" ] && [ ! -L "$1" ]; then "$B" mknod -m 600 "$1" c "$2" "$3" || return 1; fi
    [ -c "$1" ] && [ ! -L "$1" ]
}
"$B" mkdir -p /dev || rescue
basic_node /dev/console 5 1 || rescue
exec </dev/console >/dev/console 2>&1
basic_node /dev/null 1 3 && basic_node /dev/zero 1 5 && \
    basic_node /dev/random 1 8 && basic_node /dev/urandom 1 9 || fail base_nodes
"$B" mount -t proc proc /proc && "$B" mount -t sysfs sysfs /sys || fail pseudo_mounts
root_ram=no
while read -r source target fstype options rest; do
    case "$target:$fstype" in
        /:rootfs|/:ramfs|/:tmpfs) root_ram=yes;;
        /proc:proc|/sys:sysfs|/dev:devtmpfs) :;;
        *) fail unexpected_initial_mount;;
    esac
done < /proc/mounts
[ "$root_ram" = yes ] || fail non_ram_root
"$B" mkdir -p /run /tmp /etc || fail ram_directories
[ ! -e "$R" ] && [ ! -L "$R" ] && "$B" mkdir -m 700 "$R" || fail runtime_directory
printf '1\n' > "$R/supervisor.pid"
printf 'S12_CUSTOM_RUNTIME_V1\n' > "$R/supervisor.tag"
printf 'starting\n' > "$R/state"
[ "$("$B" cat /sys/class/block/mtdblock4/dev)" = 31:4 ] &&
    [ "$("$B" cat /sys/class/mtd/mtd4/name)" = system0 ] &&
    [ "$("$B" cat /sys/class/mtd/mtd4/size)" = 33554432 ] || fail system0_identity
"$B" mknod -m 600 "$N" b 31 4 && "$B" mkdir -m 700 "$V" || fail system0_ram_handle
"$B" mount -t squashfs -o ro "$N" "$V" || fail system0_read_only_mount
mount_ok=no
while read -r source target fstype options rest; do
    if [ "$source" = "$N" ] && [ "$target" = "$V" ] && [ "$fstype" = squashfs ]; then
        case ",$options," in *,ro,*) mount_ok=yes;; esac
    fi
done < /proc/mounts
[ "$mount_ok" = yes ] && [ -x "$L" ] || fail system0_confirmation
export ALSA_CONFIG_PATH=/s12/alsa.conf ALSA_CONFIG_DIR=$V/usr/share/alsa
export ALSA_PLUGIN_DIR=$V/usr/lib/alsa-lib S12_CAPTURE_PCM=s12cap S12_PLAY_PCM=s12play
"$B" mkdir -m 700 /dev/snd || fail sound_directory
for sound_name in controlC0 pcmC0D1p pcmC0D2c; do
    IFS= read -r sound_dev < "/sys/class/sound/$sound_name/dev" || fail sound_identity
    case "$sound_dev" in 116:*) sound_minor=${sound_dev#116:};; *) fail sound_major;; esac
    case "$sound_minor" in ''|*[!0-9]*) fail sound_minor;; esac
    [ "$sound_minor" -le 255 ] || fail sound_minor
    "$B" mknod -m 600 "/dev/snd/$sound_name" c 116 "$sound_minor" || fail sound_node
done
"$L" --library-path "$P" "$V/usr/bin/amixer" -D s12ctl cset numid=2 on > "$R/mixer-2.log" 2>&1 || fail mixer2
"$L" --library-path "$P" "$V/usr/bin/amixer" -D s12ctl cset numid=3 on > "$R/mixer-3.log" 2>&1 || fail mixer3
"$L" --library-path "$P" "$V/usr/bin/amixer" -D s12ctl cset numid=4 off,off > "$R/mixer-4.log" 2>&1 || fail mixer4
# Fail closed on an unknown original KWS/AFE ABI. No models are distributed.
VENDOR_SHA=79f1a33d9683cd6940c8d23e3dd4e992f1958fe220c0dfff8d230f9f8a1b5e73
set -- $("$B" sha256sum "$V/usr/lib/libxaudio_engine.so")
[ "$1" = "$VENDOR_SHA" ] || fail vendor_wake_abi
printf '%s\n' "$VENDOR_SHA" > "$R/vendor-wake.verified"
for program in client panel bridge; do
    IFS= read -r expected < "/s12/$program.sha256" || fail artifact_digest
    set -- $("$B" sha256sum "/s12/$program.arm")
    [ "$1" = "$expected" ] || fail artifact_digest
done
"$B" cp /s12/private/token "$R/token" && "$B" chmod 600 "$R/token" || fail token_copy
"$L" --library-path "$P" /s12/client.arm --selftest > "$R/client-selftest.log" 2>&1 || fail client_selftest
"$L" --library-path "$P" /s12/panel.arm --selftest > "$R/panel-selftest.log" 2>&1 || fail panel_selftest
printf 'boot\n' > "$R/ui.state"
/bin/sh /s12/panel_start.sh </dev/null > "$R/panel-start.log" 2>&1 &
printf '%s\n' "$!" > "$R/panel-supervisor-child.pid"
# The build timestamp is only a clock floor. Retain an already newer clock;
# the bridge performs bounded background SNTP while TLS verifies certificates.
IFS= read -r BUILD_UNIX < /s12/build-unix || fail build_time
case "$BUILD_UNIX" in ''|*[!0-9]*) fail build_time;; esac
CLOCK_NOW=$("$B" date -u +%s) || fail current_clock
case "$CLOCK_NOW" in ''|*[!0-9]*) fail current_clock;; esac
if [ "$CLOCK_NOW" -lt "$BUILD_UNIX" ]; then
    "$B" date -u -s "@$BUILD_UNIX" > "$R/build-clock.log" 2>&1 || fail build_clock
fi
"$B" ifconfig lo up || fail loopback_up
/s12/bridge.arm --config /s12/private/server.json --sync-time </dev/null > "$R/bridge.log" 2>&1 &
BRIDGE_PID=$!
printf '%s\n' "$BRIDGE_PID" > "$R/bridge.pid"
"$B" sleep 1
"$B" kill -0 "$BRIDGE_PID" || fail bridge_start
[ ! -d /sys/module/dhd ] || fail wifi_already_loaded
"$B" mkdir -m 700 "$R/wifi" "$R/wpa-ctrl" || fail wifi_ram_directory
IFS= read -r WIFI_MAC < /s12/private/wifi-mac || fail wifi_identity
printf '%s\n' "$WIFI_MAC" | "$B" grep -Eq '^[0-9a-f]{2}(:[0-9a-f]{2}){5}$' || fail wifi_identity
for file in fw_bcm43455c0_ag.bin fw_bcm43454f_ag.bin nvram.txt nvram_s12.txt config.txt; do
    "$B" cp "$V/etc/wifi/$file" "$R/wifi/$file" || fail wifi_firmware_copy
done
for file in nvram.txt nvram_s12.txt; do
    [ "$("$B" grep -c '^macaddr=' "$R/wifi/$file")" = 1 ] || fail wifi_mac_entry
    "$B" sed -i "s/^macaddr=.*/macaddr=$WIFI_MAC/" "$R/wifi/$file" || fail wifi_ram_mac
done
unset WIFI_MAC
printf 'attempted\n' > "$R/dhd-attempted"
/sbin/insmod "$V/lib/modules/4.9.61/dhd.ko" firmware_path="$R/wifi/fw_bcm43455c0_ag.bin" \
    nvram_path="$R/wifi/nvram.txt" config_path="$R/wifi/config.txt" op_mode=0 > "$R/wifi-driver.log" 2>&1 || fail wifi_driver
tries=0
while [ ! -d /sys/class/net/wlan0 ] && [ "$tries" -lt 20 ]; do "$B" sleep 1; tries=$((tries+1)); done
[ -d /sys/class/net/wlan0 ] && "$B" ifconfig wlan0 up || fail wlan0_up
printf 'ready\n' > "$R/state"
/bin/sh /s12/network_start.sh </dev/null > "$R/network.log" 2>&1 &
printf '%s\n' "$!" > "$R/network-supervisor-child.pid"
/bin/sh /s12/client_wake.sh </dev/null > "$R/wake-supervisor.log" 2>&1 &
printf '%s\n' "$!" > "$R/wake-supervisor-child.pid"
echo 'S12_XIAOZHI: startup launched; experimental RAM runtime; physical console retained'
rescue
