#!/bin/sh
# Shared POSIX session library. Thin entry scripts set fixed B/R/S12DIR/PROC.
# Sourcing defines functions only; no microphone or process starts here.
set -f
SESSION_CLIENT_PID=
SESSION_CLIENT_ID=
SESSION_REAPED=no
SESSION_LOCKED=no
SESSION_REQUESTED=
SESSION_RESULT=1
SESSION_SIGNAL_EPOCH=0

session_identity() {
    identity_pid=$1
    case "$identity_pid" in ''|*[!0-9]*) return 1;; esac
    IFS= read -r identity_line < "$S12_PROC/$identity_pid/stat" 2>/dev/null || return 1
    case "$identity_line" in "$identity_pid ("*") "*) :;; *) return 1;; esac
    # Remove the complete parenthesized comm, which itself may contain spaces
    # and ')' characters. Kernel stat fields after it contain only state/numbers.
    identity_fields=${identity_line##*) }
    set -- $identity_fields
    [ "$#" -ge 20 ] || return 1
    identity_parent=$2
    shift 19
    identity_started=$1
    case "$identity_parent:$identity_started" in *[!0-9:]*|:*) return 1;; esac
    printf '%s:%s\n' "$identity_parent" "$identity_started"
}
session_owned_child() {
    [ -n "$SESSION_CLIENT_PID" ] && [ -n "$SESSION_CLIENT_ID" ] || return 1
    session_current=$(session_identity "$SESSION_CLIENT_PID") || return 1
    [ "$session_current" = "$SESSION_CLIENT_ID" ] || return 1
    case "$session_current" in "$$":*) return 0;; *) return 1;; esac
}
session_signal() {
    SESSION_SIGNAL_EPOCH=$((SESSION_SIGNAL_EPOCH+1))
    SESSION_REQUESTED=$1
    printf 'stopping:%s\n' "$1" > "$R/$SESSION_MODE.state"
    if session_owned_child; then "$B" kill "-$1" "$SESSION_CLIENT_PID" 2>/dev/null; fi
}
session_wait() {
    while :; do
        session_wait_epoch=$SESSION_SIGNAL_EPOCH
        wait "$SESSION_CLIENT_PID"
        SESSION_RESULT=$?
        # wait can be interrupted by a trapped signal before the child exits.
        # Match parent+starttime, not merely kill -0, to avoid PID-reuse races.
        if [ "$SESSION_RESULT" -gt 128 ] && [ "$session_wait_epoch" -ne "$SESSION_SIGNAL_EPOCH" ]; then continue; fi
        if session_owned_child; then continue; fi
        SESSION_REAPED=yes
        break
    done
}
session_cleanup() {
    if [ -n "$SESSION_CLIENT_PID" ] && [ "$SESSION_REAPED" != yes ]; then
        session_signal TERM
        session_wait
    fi
    if [ "$SESSION_LOCKED" = yes ]; then
        # Clear only our own records after our child has been waited for.
        if [ "$("$B" cat "$R/client-wrapper.pid" 2>/dev/null)" = "$$" ]; then
            "$B" rm -f "$R/client-wrapper.pid"
            if [ "$("$B" cat "$R/client.pid" 2>/dev/null)" = "$SESSION_CLIENT_PID" ]; then
                "$B" rm -f "$R/client.pid"
            fi
        fi
        if [ "$("$B" cat "$R/turn.lock/owner.pid" 2>/dev/null)" = "$$" ]; then
            "$B" rm -f "$R/turn.lock/owner.pid"
            "$B" rmdir "$R/turn.lock" 2>/dev/null
        fi
        SESSION_LOCKED=no
    fi
}
session_run() {
    SESSION_MODE=$1
    shift
    case "$SESSION_MODE" in once|wake) :;; *) return 2;; esac
    [ -x "$B" ] || return 1
    [ "$("$B" cat "$R/supervisor.pid" 2>/dev/null)" = 1 ] || return 1
    [ "$("$B" cat "$R/supervisor.tag" 2>/dev/null)" = S12_CUSTOM_RUNTIME_V1 ] || return 1
    [ "$("$B" cat "$R/state" 2>/dev/null)" = ready ] || return 1
    session_identity "$$" >/dev/null || return 1
    "$B" mkdir -m 700 "$R/turn.lock" 2>/dev/null || { echo 'S12_CLIENT_SESSION: busy'; return 1; }
    SESSION_LOCKED=yes
    trap 'session_cleanup' 0
    trap 'session_signal TERM' TERM
    trap 'session_signal HUP' HUP
    trap 'session_signal INT' INT
    if ! printf '%s\n' "$$" > "$R/turn.lock/owner.pid"; then
        "$B" rm -f "$R/turn.lock/owner.pid"
        "$B" rmdir "$R/turn.lock" 2>/dev/null
        SESSION_LOCKED=no
        return 1
    fi
    # A stale outside PID marker is never overwritten or killed speculatively.
    [ ! -e "$R/client-wrapper.pid" ] && [ ! -L "$R/client-wrapper.pid" ] || return 1
    [ ! -e "$R/client.pid" ] && [ ! -L "$R/client.pid" ] || return 1
    printf '%s\n' "$$" > "$R/client-wrapper.pid" || return 1
    V=$R/system0
    L=$V/lib/ld-linux-armhf.so.3
    P=$V/lib:$V/usr/lib
    export ALSA_CONFIG_PATH=$S12DIR/alsa.conf ALSA_CONFIG_DIR=$V/usr/share/alsa
    export ALSA_PLUGIN_DIR=$V/usr/lib/alsa-lib S12_CAPTURE_PCM=s12cap S12_PLAY_PCM=s12play
    IFS= read -r expected_client < "$S12DIR/client.sha256" || return 1
    case "$expected_client" in ''|*[!0-9a-f]*) return 1;; esac
    [ "${#expected_client}" -eq 64 ] || return 1
    session_digest=$("$B" sha256sum "$S12DIR/client.arm") || return 1
    session_digest=${session_digest%% *}
    [ "$session_digest" = "$expected_client" ] || return 1
    [ -z "$SESSION_REQUESTED" ] || return 130
    printf 'starting\n' > "$R/$SESSION_MODE.state"
    "$L" --library-path "$P" "$S12DIR/client.arm" --config "$S12DIR/client.json" "$@" \
        > "$R/client.jsonl" 2> "$R/client.err" &
    SESSION_CLIENT_PID=$!
    SESSION_CLIENT_ID=$(session_identity "$SESSION_CLIENT_PID")
    printf '%s\n' "$SESSION_CLIENT_PID" > "$R/client.pid" || { session_signal TERM; session_wait; return 1; }
    printf 'running\n' > "$R/$SESSION_MODE.state"
    printf 'S12_CLIENT_SESSION: mode=%s client_pid=%s wrapper_pid=%s\n' "$SESSION_MODE" "$SESSION_CLIENT_PID" "$$"
    if [ -n "$SESSION_REQUESTED" ]; then session_signal "$SESSION_REQUESTED"; fi
    session_wait
    if [ -n "$SESSION_REQUESTED" ]; then
        printf 'stopped:%s\n' "$SESSION_REQUESTED" > "$R/$SESSION_MODE.state"
    elif [ "$SESSION_RESULT" -eq 0 ]; then
        printf 'exited:0\n' > "$R/$SESSION_MODE.state"
    else
        printf 'failed:%s\n' "$SESSION_RESULT" > "$R/$SESSION_MODE.state"
    fi
    printf 'S12_CLIENT_SESSION: mode=%s exit=%s; console remains available\n' "$SESSION_MODE" "$SESSION_RESULT"
    return "$SESSION_RESULT"
}
