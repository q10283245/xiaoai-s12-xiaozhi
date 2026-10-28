#!/bin/sh
# RAM boot validation only. Do not invoke vendor init or any NAND/env/UBI tool.
PATH=/bin:/sbin
export PATH
umask 077

echo 'S12_RAM_BOOT_PROBE_V1: custom init reached'
/bin/mount -t proc proc /proc || echo 'probe: proc mount failed'
/bin/mount -t sysfs sysfs /sys || echo 'probe: sysfs mount failed'
echo "S12_RAM_BOOT_PROBE_V1: supervisor_pid=$$; no vendor root/data mounted"
echo 'probe: interactive shell is a child; exiting it restarts only the shell'

# Keep PID1 alive and reap its console child. No automatic reboot or slot write.
while :; do
    /bin/sh -i </dev/console >/dev/console 2>&1
    echo 'probe: console shell exited; restarting in RAM'
    /bin/busybox sleep 1
done
