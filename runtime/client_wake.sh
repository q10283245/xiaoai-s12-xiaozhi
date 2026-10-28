#!/bin/sh
# Fixed generic Xiaozhi entry. No project cloud/control channel is started.
[ "$#" -eq 0 ] || exit 2
PATH=/bin:/sbin
export PATH
umask 077
B=/bin/busybox R=/run/s12 S12DIR=/s12 S12_PROC=/proc
. /s12/client_session.sh || exit 1
session_run wake --wake-loop --buffer-tts --playback --timeout 120 --panel \
    --mic-array --local-wake --followup-seconds 0
exit "$?"
