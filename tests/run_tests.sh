#!/usr/bin/env bash
# End-to-end tests for es2netif-bridge, run against tests/fake_netif.
#
# Usage: tests/run_tests.sh [BUILD_DIR [TEST...]]   (default: build, all tests)
#   TEST is a test function name without the test_ prefix, e.g. sighup.
#   or:  ctest --test-dir build --output-on-failure
#
# Needs bash, socat, pgrep and ipcs. Uses /tmp/epsonWork (hard-coded in es2netif),
# so don't run it while a real bridge or an epsonscan2 scan is active.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$(cd "${1:-$ROOT/build}" && pwd) || { echo "build dir not found"; exit 2; }
BRIDGE=$BUILD/es2netif-bridge
FAKE=$BUILD/fake_netif
WORK=/tmp/epsonWork
DAT=$WORK/interrupt.dat

for bin in "$BRIDGE" "$FAKE"; do
    [ -x "$bin" ] || { echo "missing $bin (build first)"; exit 2; }
done
for tool in socat pgrep ipcs; do
    command -v "$tool" >/dev/null || { echo "$tool is required"; exit 2; }
done
if pgrep -x es2netif-bridge >/dev/null || pgrep -x es2netif >/dev/null; then
    echo "an es2netif-bridge or es2netif process is already running; refusing to share /tmp/epsonWork"
    exit 2
fi

T=$(mktemp -d "${TMPDIR:-/tmp}/es2nb-test.XXXXXX")
SOCK=$T/events.sock
CONF=$T/test.conf
LOG=$T/bridge.log
BRIDGE_PID=
CLIENT_PIDS=()
PASS=0
FAIL=0
CURRENT=

cleanup()
{
    [ -n "$BRIDGE_PID" ] && kill -TERM "$BRIDGE_PID" 2>/dev/null && wait "$BRIDGE_PID" 2>/dev/null
    stop_clients
    pkill -x fake_netif 2>/dev/null
    if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; else echo "artifacts kept in $T"; fi
}
trap cleanup EXIT

# ---- helpers -----------------------------------------------------------------

begin() { CURRENT=$1; echo "== $1"; }

ok()
{
    PASS=$((PASS + 1))
    echo "   ok   $1"
}

fail()
{
    FAIL=$((FAIL + 1))
    echo "   FAIL $1"
    if [ -f "$LOG" ]; then
        echo "   --- bridge log ($CURRENT):"
        sed 's/^/   | /' "$LOG"
    fi
}

check() # description, command...
{
    local what=$1; shift
    if "$@"; then ok "$what"; else fail "$what"; fi
}

# write_conf [extra key=value lines...]
write_conf()
{
    {
        echo "scanner_address = 10.0.0.5"
        echo "netif_path = $FAKE"
        echo "socket_path = $SOCK"
        echo "probe_port = 0"
        echo "reconnect_min_s = 30"
        echo "reconnect_max_s = 60"
        echo "log_level = debug"
        for line in "$@"; do echo "$line"; done
    } > "$CONF"
}

# start_bridge MODE [PAUSE_US]: launches the bridge with FAKE_MODE=MODE (and
# optionally FAKE_PAUSE_US) and waits for the socket.
start_bridge()
{
    : > "$LOG"
    FAKE_MODE=$1 FAKE_PAUSE_US=${2:-1500000} "$BRIDGE" -c "$CONF" 2> "$LOG" &
    BRIDGE_PID=$!
    for _ in $(seq 50); do
        [ -S "$SOCK" ] && return 0
        sleep 0.1
    done
    fail "bridge did not create $SOCK"
    return 1
}

stop_bridge()
{
    [ -n "$BRIDGE_PID" ] || return 0
    kill -TERM "$BRIDGE_PID" 2>/dev/null
    wait "$BRIDGE_PID"
    local rc=$?
    BRIDGE_PID=
    return $rc
}

# start_client OUTFILE: records everything from the socket into OUTFILE, and
# appends "<EOF>" when the connection ends.
start_client()
{
    : > "$1"
    ( socat -u "UNIX-CONNECT:$SOCK" STDOUT >> "$1" 2>/dev/null; echo "<EOF>" >> "$1" ) &
    CLIENT_PIDS+=($!)
    # Give the bridge a moment to accept.
    for _ in $(seq 20); do
        grep -q "client connected" "$LOG" && return 0
        sleep 0.1
    done
    return 0
}

# Each client is a subshell running socat, so kill the socat child first.
stop_clients()
{
    for p in "${CLIENT_PIDS[@]}"; do
        pkill -P "$p" socat 2>/dev/null
        kill "$p" 2>/dev/null
        wait "$p" 2>/dev/null
    done
    CLIENT_PIDS=()
}

# wait_for FILE FIXED-STRING [TIMEOUT_S]
wait_for()
{
    local file=$1 pat=$2 timeout=${3:-8}
    local n=$((timeout * 10))
    for _ in $(seq "$n"); do
        grep -qF -- "$pat" "$file" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

no_leftovers()
{
    ! pgrep -x fake_netif >/dev/null && [ ! -e "$SOCK" ] && [ ! -e "$DAT" ]
}

# Creates a segment that collides with the bridge's interrupt segment, as a
# crashed run would leave it, and prints its shmid.
make_stale_shm()
{
    mkdir -p "$WORK"
    touch "$DAT"
    "$FAKE" --make-stale-shm
}

# ---- tests -------------------------------------------------------------------

test_config_validation()
{
    begin "config validation"
    check "example config is valid" bash -c "'$BRIDGE' -t -c '$ROOT/es2netif-bridge.conf.example' >/dev/null 2>&1"

    printf 'scanner_address = 1.2.3.4\nsocket_mode = 999\n' > "$T/bad.conf"
    check "invalid socket_mode is rejected" bash -c "! '$BRIDGE' -t -c '$T/bad.conf' 2>/dev/null"

    printf 'probe_port = 1865\n' > "$T/bad.conf"
    check "missing scanner_address is rejected" bash -c "! '$BRIDGE' -t -c '$T/bad.conf' 2>/dev/null"

    printf 'scanner_address = 1.2.3.4\nnot a key value line\n' > "$T/bad.conf"
    check "malformed line is rejected" bash -c "! '$BRIDGE' -t -c '$T/bad.conf' 2>/dev/null"

    printf 'scanner_address = 1.2.3.4\nmystery = 1\n' > "$T/warn.conf"
    check "unknown key only warns" bash -c "'$BRIDGE' -t -c '$T/warn.conf' 2>&1 | grep -q \"unknown key 'mystery'\""

    check "missing config file is rejected" bash -c "! '$BRIDGE' -t -c '$T/nonexistent.conf' 2>/dev/null"
}

test_events_and_disconnect()
{
    begin "events and scanner disconnect (FAKE_MODE=disconnect)"
    write_conf
    start_bridge disconnect || return
    local out=$T/events_disconnect.out
    start_client "$out"

    check "button_press relayed" wait_for "$out" '"event":"button_press","scanner":"10.0.0.5","button":3'
    check "prevent_timeout answered true" wait_for "$out" '"event":"prevent_timeout_query","scanner":"10.0.0.5","answer":true'
    check "reserved_by_host relayed" wait_for "$out" '"event":"reserved_by_host","scanner":"10.0.0.5","address":"10.0.0.9"'
    check "disconnect relayed" wait_for "$out" '"event":"disconnect"'
    check "disconnected status emitted" wait_for "$out" '"event":"disconnected","scanner":"10.0.0.5","reason":"scanner disconnected"'
    check "reconnect scheduled" wait_for "$out" '"event":"reconnecting","scanner":"10.0.0.5","attempt":1,"next_retry_s":30'
    check "lines carry an RFC 3339 timestamp" \
        grep -qE '^\{"ts":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z"' "$out"
    check "fake saw the right UDI" grep -qF "fake: open //10.0.0.5 sem_key=" "$LOG"

    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftover process, socket or interrupt.dat" no_leftovers
}

test_child_crash()
{
    begin "es2netif crash (FAKE_MODE=exit)"
    write_conf
    start_bridge exit || return
    local out=$T/events_exit.out
    start_client "$out"

    check "events before crash relayed" wait_for "$out" '"event":"button_press"'
    check "crash detected" wait_for "$out" '"event":"disconnected","scanner":"10.0.0.5","reason":"es2netif'
    check "reconnect scheduled" wait_for "$out" '"event":"reconnecting"'
    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
}

test_socket_eof()
{
    begin "es2netif closes its socket (FAKE_MODE=eof)"
    write_conf
    start_bridge eof || return
    local out=$T/events_eof.out
    start_client "$out"

    check "EOF detected" wait_for "$out" '"reason":"es2netif closed its connection"'
    check "reconnect scheduled" wait_for "$out" '"event":"reconnecting"'
    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
}

test_reconnect()
{
    begin "automatic reconnect after disconnect"
    write_conf "reconnect_min_s = 1" "reconnect_max_s = 2"
    start_bridge disconnect || return
    local out=$T/events_reconnect.out
    start_client "$out"

    check "first disconnect" wait_for "$out" '"event":"disconnected"'
    check "reconnected" wait_for "$out" '"event":"connected"' 10
    check "events flow after reconnect" bash -c "sleep 3; [ \$(grep -cF '\"event\":\"button_press\"' '$out') -ge 2 ]"
    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
}

test_semaphore_race()
{
    begin "no lost events when es2netif releases on a whole second"
    # The fake pauses exactly 1 s before its final disconnect event. With the old
    # 1 s timed semaphore wait, the release often landed between two waits, and
    # the fake re-acquired its own release. Reconnect quickly and require several
    # clean cycles in a row.
    write_conf "reconnect_min_s = 1" "reconnect_max_s = 1"
    start_bridge disconnect 1000000 || return
    local out=$T/events_race.out
    start_client "$out"

    local cycles=${RACE_CYCLES:-5}
    check "$cycles disconnect events without a lost one" \
        bash -c "for _ in \$(seq $((cycles * 60))); do [ \$(grep -cF '\"event\":\"disconnect\"' '$out') -ge $cycles ] && exit 0; sleep 0.1; done; exit 1"
    check "fake never timed out waiting for the bridge" bash -c "! grep -qF 'fake: semtimedop' '$LOG'"
    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
}

test_prevent_timeout_false()
{
    begin "prevent_timeout = false"
    write_conf "prevent_timeout = false"
    start_bridge hold || return
    local out=$T/events_pt.out
    start_client "$out"

    check "prevent_timeout answered false" wait_for "$out" '"event":"prevent_timeout_query","scanner":"10.0.0.5","answer":false'
    check "fake received answer 0" wait_for "$LOG" "fake: prevent_timeout answer=0"
    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
}

test_status_events_off()
{
    begin "emit_status_events = false"
    write_conf "emit_status_events = false"
    start_bridge disconnect || return
    local out=$T/events_nostatus.out
    start_client "$out"

    check "scanner events still relayed" wait_for "$out" '"event":"disconnect"'
    check "reconnect happened internally" wait_for "$LOG" "reconnecting in"
    check "no status events emitted" \
        bash -c "! grep -qE '\"event\":\"(connected|disconnected|reconnecting|config_reloaded)\"' '$out'"
    check "SIGTERM exits 0" stop_bridge
    stop_clients
}

test_sighup()
{
    begin "SIGHUP reload"
    write_conf
    start_bridge hold || return
    local out=$T/events_hup.out
    start_client "$out"

    check "first session answers true" wait_for "$out" '"answer":true'

    write_conf "prevent_timeout = false"
    kill -HUP "$BRIDGE_PID"
    check "config_reloaded emitted" wait_for "$out" '"event":"config_reloaded"'
    check "reload reason reported" wait_for "$out" '"reason":"configuration reload"'
    check "session reopened" wait_for "$out" '"event":"connected"'
    check "new setting applied" wait_for "$out" '"answer":false'
    check "client kept across reload" bash -c "! grep -qF '<EOF>' '$out'"

    echo "this is not valid" >> "$CONF"
    kill -HUP "$BRIDGE_PID"
    check "broken config rejected" wait_for "$LOG" "reload failed, keeping current configuration"
    check "bridge still running" kill -0 "$BRIDGE_PID"

    # Changing socket_path must rebind: the old socket goes away, the new one appears.
    local old_sock=$SOCK
    SOCK=$T/events2.sock
    write_conf
    kill -HUP "$BRIDGE_PID"
    check "new socket created" wait_for "$LOG" "listening on $SOCK"
    check "old socket removed" bash -c "sleep 0.5; [ ! -e '$old_sock' ]"
    check "old client dropped on rebind" wait_for "$out" "<EOF>"

    check "SIGTERM exits 0" stop_bridge
    stop_clients
    check "no leftovers" no_leftovers
    SOCK=$old_sock
}

test_single_client()
{
    begin "single client: a new connection replaces the old"
    write_conf
    start_bridge hold || return
    local first=$T/client1.out second=$T/client2.out
    start_client "$first"
    start_client "$second"

    check "first client disconnected" wait_for "$first" "<EOF>"
    check "bridge logged replacement" wait_for "$LOG" "replaced by new client"
    check "second client receives events" wait_for "$second" '"event":"prevent_timeout_query"'
    check "SIGTERM exits 0" stop_bridge
    check "second client closed at shutdown" wait_for "$second" "<EOF>"
    stop_clients
}

test_stale_cleanup()
{
    begin "stale shared-memory cleanup"
    local shmid
    if ! shmid=$(make_stale_shm); then
        fail "could not create stale segment"
        return
    fi

    write_conf
    start_bridge disconnect || return
    check "stale segment removed" wait_for "$LOG" "removing stale interrupt shared memory segment (id $shmid)"
    check "session opened afterwards" wait_for "$LOG" "connected to 10.0.0.5"
    check "SIGTERM exits 0" stop_bridge
    check "segment is gone" bash -c "! ipcs -m | awk '{print \$2}' | grep -qx '$shmid'"
    check "no leftovers" no_leftovers
}

test_stale_cleanup_disabled()
{
    begin "cleanup_stale = false leaves a colliding segment as an open error"
    local shmid
    if ! shmid=$(make_stale_shm); then
        fail "could not create stale segment"
        return
    fi

    write_conf "cleanup_stale = false"
    start_bridge disconnect || return
    check "open fails with a clear error" wait_for "$LOG" "interrupt channel setup failed"
    check "bridge retries instead of exiting" bash -c "kill -0 $BRIDGE_PID"
    check "SIGTERM exits 0" stop_bridge
    ipcrm -m "$shmid" 2>/dev/null
    rm -f "$DAT"
    check "no fake process left" bash -c "! pgrep -x fake_netif >/dev/null"
}

# ---- main --------------------------------------------------------------------

ALL_TESTS=(
    config_validation
    events_and_disconnect
    child_crash
    socket_eof
    reconnect
    semaphore_race
    prevent_timeout_false
    status_events_off
    sighup
    single_client
    stale_cleanup
    stale_cleanup_disabled
)
if [ $# -gt 1 ]; then
    shift
    SELECTED=("$@")
else
    SELECTED=("${ALL_TESTS[@]}")
fi
for t in "${SELECTED[@]}"; do
    if ! declare -F "test_$t" >/dev/null; then
        echo "unknown test: $t (available: ${ALL_TESTS[*]})"
        exit 2
    fi
    "test_$t"
done

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
