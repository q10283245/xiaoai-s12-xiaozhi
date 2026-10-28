#!/bin/sh
[ "$#" -eq 0 ] || exit 2
PATH=/bin:/sbin
export PATH
B=/bin/busybox R=/run/s12 S12DIR=/s12 S12_PROC=/proc
V=$R/system0 L=$R/system0/lib/ld-linux-armhf.so.3 P=$R/system0/lib:$R/system0/usr/lib
. /s12/network_session.sh || exit 1
network_run
exit "$?"
