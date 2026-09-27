#!/usr/bin/env bash
# Tests for contrib/scan-trigger/es-scan-trigger, run against a mock event
# socket served by socat and a stub naps2 that records its arguments. It needs
# no bridge, scanner, NAPS2 or /tmp/epsonWork, so it is safe to run while a
# real es-bridge service is active.
#
# Usage: tests/test_scan_trigger.sh
# Needs bash, socat and jq.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TRIGGER=$ROOT/contrib/scan-trigger/es-scan-trigger
for tool in socat jq; do
    command -v "$tool" >/dev/null || { echo "$tool is required"; exit 2; }
done

T=$(mktemp -d "${TMPDIR:-/tmp}/esb-trigger-test.XXXXXX")
SOCK=$T/events.sock
CONF=$T/trigger.conf
LOG=$T/trigger.log
OUT=$T/scans
FEED=$T/feed           # lines the mock server sends on each connection
STUB=$T/naps2          # stand-in for naps2
CALLS=$T/calls         # one file of arguments per stub invocation
PASS=0
FAIL=0
SERVER_PID=
TRIGGER_PID=

cleanup()
{
    [ -n "$TRIGGER_PID" ] && kill -TERM "$TRIGGER_PID" 2>/dev/null && wait "$TRIGGER_PID" 2>/dev/null
    stop_server
    if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; else echo "artifacts kept in $T"; fi
}
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); echo "   ok   $1"; }
fail() { FAIL=$((FAIL + 1)); echo "   FAIL $1"; echo "   --- trigger log:"; sed 's/^/   | /' "$LOG"; }
check() { local what=$1; shift; if "$@"; then ok "$what"; else fail "$what"; fi; }

wait_for() # FILE FIXED-STRING [TIMEOUT_S]
{
    local n=$(( ${3:-8} * 10 ))
    for _ in $(seq "$n"); do
        grep -qF -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

wait_count() # MIN [TIMEOUT_S]: wait for at least MIN stub calls
{
    local n=$(( ${2:-8} * 10 ))
    for _ in $(seq "$n"); do
        [ "$(calls)" -ge "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

calls() { find "$CALLS" -type f 2>/dev/null | wc -l; }

# The stub writes its arguments (one per line) to $CALLS/N, then behaves as
# STUB_MODE says: ok (write the --output file), nopages (write nothing, exit 0,
# like NAPS2 with an empty feeder) or fail (exit 3). STUB_SECONDS delays it.
write_stub()
{
    cat > "$STUB" <<EOF
#!/usr/bin/env bash
mkdir -p '$CALLS'
n=\$(( \$(find '$CALLS' -type f | wc -l) + 1 ))
printf '%s\n' "\$@" > '$CALLS'/\$n
echo \$\$ > '$T'/stub.pid
echo "\${SANE_CONFIG_DIR-<unset>}" > '$T'/stub.sane
sleep "\${STUB_SECONDS:-0}"
case "\${STUB_MODE:-ok}" in
    fail) exit 3 ;;
    nopages) echo "0 page(s) scanned."; exit 0 ;;
esac
out=
while [ \$# -gt 0 ]; do [ "\$1" = --output ] && out=\$2; shift; done
echo "scanned" > "\$out"
EOF
    chmod +x "$STUB"
}

# Event line in es-bridge's format, stamped now.
event() # NAME [SCANNER]
{
    printf '{"ts":"%s","event":"%s","scanner":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" "$1" "${2:-10.0.0.5}"
}

# The mock server sends $FEED, then holds the connection open until
# $FEED.close appears.
start_server()
{
    rm -f "$FEED.close"
    socat "UNIX-LISTEN:$SOCK,fork" \
        SYSTEM:"cat '$FEED'; while [ ! -e '$FEED.close' ]; do sleep 0.1; done" 2>/dev/null &
    SERVER_PID=$!
    for _ in $(seq 50); do [ -S "$SOCK" ] && return 0; sleep 0.1; done
    echo "mock server did not start"; return 1
}

stop_server()
{
    [ -n "$SERVER_PID" ] || return 0
    touch "$FEED.close"
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
    rm -f "$SOCK"
}

# Replaces the feed and ends the current connection, so the trigger reconnects
# and reads the new events.
send_new_feed() # event lines on stdin
{
    cat > "$FEED.next"
    mv "$FEED.next" "$FEED"
    touch "$FEED.close"
}

# write_conf [extra lines...]
write_conf()
{
    {
        echo "SOCKET='$SOCK'"
        echo "OUTPUT_DIR='$OUT'"
        echo "OUTPUT_NAME='scan.pdf'"
        echo "NAPS2='$STUB'"
        echo "NAPS2_DEVICE='EPSON FF-680W'"
        echo "RECONNECT_DELAY=1"
        for line in "$@"; do echo "$line"; done
    } > "$CONF"
}

start_trigger()
{
    : > "$LOG"
    "$TRIGGER" -c "$CONF" 2> "$LOG" &
    TRIGGER_PID=$!
}

stop_trigger()
{
    kill -TERM "$TRIGGER_PID" 2>/dev/null
    wait "$TRIGGER_PID"
    local rc=$?
    TRIGGER_PID=
    return $rc
}

reset()
{
    stop_server
    rm -rf "$OUT" "$CALLS"
    : > "$FEED"
    write_stub
}

# ---- tests -------------------------------------------------------------------

test_naps2_command_line()
{
    echo "== request_start_scanning runs NAPS2 with the configured options"
    reset
    { event connected; event request_start_scanning; } > "$FEED"
    write_conf "NAPS2_EXTRA_ARGS=(--pagesize letter --pdftitle 'My scans')"
    start_server || return
    start_trigger

    check "scan saved" wait_for "$LOG" "scan: saved $OUT/scan.pdf"
    check "system SANE config by default" test "$(cat "$T/stub.sane" 2>/dev/null)" = "<unset>"
    local expected
    expected=$(printf '%s\n' console --noprofile --driver sane --device 'EPSON FF-680W' \
        --source duplex --dpi 300 --bitdepth color --verbose \
        --pagesize letter --pdftitle 'My scans' --output "$OUT/scan.pdf")
    check "exact NAPS2 arguments" test "$(cat "$CALLS/1" 2>/dev/null)" = "$expected"
    check "exactly one scan" test "$(calls)" -eq 1
    check "SIGTERM exits 0" stop_trigger
}

test_options_omitted_when_empty()
{
    echo "== empty settings leave their options out"
    reset
    event request_start_scanning > "$FEED"
    write_conf "NAPS2_DRIVER=" "NAPS2_DEVICE=" "NAPS2_SOURCE=" "NAPS2_DPI=" "NAPS2_BITDEPTH=" "NAPS2_VERBOSE=no"
    start_server || return
    start_trigger

    check "scan saved" wait_for "$LOG" "scan: saved"
    check "only the fixed arguments" test "$(cat "$CALLS/1" 2>/dev/null)" = "$(printf '%s\n' console --noprofile --output "$OUT/scan.pdf")"
    check "warned about the missing device" wait_for "$LOG" "NAPS2_DEVICE is empty"
    check "SIGTERM exits 0" stop_trigger
}

test_invalid_config_rejected()
{
    echo "== invalid settings are rejected at startup"
    reset
    local setting
    for setting in "NAPS2_SOURCE=tray" "NAPS2_DPI=high" "NAPS2_BITDEPTH=16" "NAPS2_DRIVER=usb" \
                   "NAPS2_VERBOSE=maybe" "NAPS2=/nonexistent/naps2" \
                   "AIRSCAN_URL=192.168.1.122" "AIRSCAN_URL=http://x/ AIRSCAN_PROTOCOL=IPP" \
                   "AIRSCAN_URL=http://x/ NAPS2_DRIVER=escl" "AIRSCAN_URL=http://x/ NAPS2_DEVICE="; do
        write_conf "$setting"
        : > "$LOG"
        # A config that is wrongly accepted would wait for the socket forever.
        timeout 5 "$TRIGGER" -c "$CONF" 2> "$LOG"
        check "$setting refused (exit 1)" test $? -eq 1
    done
}

test_airscan_config()
{
    echo "== AIRSCAN_URL gives NAPS2 a private SANE configuration"
    reset
    event request_start_scanning > "$FEED"
    write_conf "AIRSCAN_URL=http://192.168.1.122:80/WDP/SCAN" "AIRSCAN_PROTOCOL=WSD"
    start_server || return
    RUNTIME_DIRECTORY=$T/run start_trigger

    check "scan saved" wait_for "$LOG" "scan: saved"
    check "NAPS2 got SANE_CONFIG_DIR" test "$(cat "$T/stub.sane" 2>/dev/null)" = "$T/run/sane"
    check "dll.conf enables only airscan" test "$(cat "$T/run/sane/dll.conf" 2>/dev/null)" = airscan
    check "airscan.conf declares the device" \
        grep -qxF '"EPSON FF-680W" = http://192.168.1.122:80/WDP/SCAN, WSD' "$T/run/sane/airscan.conf"
    check "discovery disabled" grep -qx 'discovery = disable' "$T/run/sane/airscan.conf"

    # Removing AIRSCAN_URL on reload goes back to the system configuration.
    write_conf
    kill -HUP "$TRIGGER_PID"
    check "reloaded" wait_for "$LOG" "config reloaded"
    rm -f "$T/stub.sane"
    event request_start_scanning | send_new_feed
    check "second scan ran" wait_count 2 10
    check "SANE_CONFIG_DIR no longer set" test "$(cat "$T/stub.sane" 2>/dev/null)" = "<unset>"
    check "SIGTERM exits 0" stop_trigger
}

test_other_events_ignored()
{
    echo "== non-trigger events and other scanners are ignored"
    reset
    { event button_press; event request_stop; event request_start_scanning 10.9.9.9; event request_start_scanning; } > "$FEED"
    write_conf "SCANNER=10.0.0.5"
    start_server || return
    start_trigger

    check "matching scanner scanned" wait_for "$LOG" "scan: saved"
    check "other scanner ignored" wait_for "$LOG" "ignoring request_start_scanning from 10.9.9.9"
    check "only one scan" bash -c "sleep 0.5; [ \$(find '$CALLS' -type f | wc -l) -eq 1 ]"
    check "SIGTERM exits 0" stop_trigger
}

test_json_parsing()
{
    echo "== events are parsed as JSON, not by position"
    reset
    # Reordered fields, spaces and an extra field must still trigger. Malformed
    # lines are logged and skipped, without stopping the service.
    {
        echo '{not json'
        echo '["request_start_scanning"]'
        printf '{ "scanner" : "10.0.0.5", "button": 1, "event" : "request_start_scanning", "ts" : "%s" }\n' \
            "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
    } > "$FEED"
    write_conf "SCANNER=10.0.0.5"
    start_server || return
    start_trigger

    check "reordered event scanned" wait_for "$LOG" "scan: saved"
    check "malformed line logged" wait_for "$LOG" "ignoring a line that isn't a JSON event: {not json"
    # Lines not starting with "{" are logged as socat output, not parsed.
    check "non-object line logged, not scanned" wait_for "$LOG" '["request_start_scanning"]'
    check "exactly one scan" test "$(calls)" -eq 1
    check "SIGTERM exits 0" stop_trigger
}

test_trigger_list()
{
    echo "== TRIGGER_EVENTS accepts several events"
    reset
    event button_press > "$FEED"
    write_conf "TRIGGER_EVENTS='button_press request_start_scanning'"
    start_server || return
    start_trigger
    check "button_press scanned" wait_for "$LOG" "scan: saved $OUT/scan.pdf"

    # The second event must come after the first scan, or it would count as a
    # press made during that scan.
    event request_start_scanning | send_new_feed
    check "request_start_scanning scanned, with a -2 suffix" wait_for "$LOG" "scan: saved $OUT/scan-2.pdf" 10
    check "SIGTERM exits 0" stop_trigger
}

test_presses_during_scan_ignored()
{
    echo "== presses made while a scan runs don't queue more scans"
    reset
    { event request_start_scanning; event request_start_scanning; event request_start_scanning; } > "$FEED"
    write_conf
    start_server || return
    STUB_SECONDS=2 start_trigger

    check "first scan saved" wait_for "$LOG" "scan: saved"
    check "queued presses ignored" wait_for "$LOG" "arrived while the previous scan was running"
    check "only one scan" bash -c "sleep 0.5; [ \$(find '$CALLS' -type f | wc -l) -eq 1 ]"

    event request_start_scanning | send_new_feed
    check "a later press scans again" wait_count 2 10
    check "SIGTERM exits 0" stop_trigger
}

test_no_pages()
{
    echo "== NAPS2 exiting 0 without a file is reported as 'no pages'"
    reset
    event request_start_scanning > "$FEED"
    write_conf
    start_server || return
    STUB_MODE=nopages start_trigger
    check "no-pages message" wait_for "$LOG" "scan: no pages scanned"
    check "NAPS2 output reaches the log" wait_for "$LOG" "0 page(s) scanned."
    check "still running" kill -0 "$TRIGGER_PID"
    check "SIGTERM exits 0" stop_trigger
}

test_failed_scan()
{
    echo "== a failing NAPS2 is logged and doesn't stop the service"
    reset
    event request_start_scanning > "$FEED"
    write_conf
    start_server || return
    STUB_MODE=fail start_trigger
    check "failure logged" wait_for "$LOG" "scan: failed, NAPS2 exit status 3"
    check "still running" kill -0 "$TRIGGER_PID"
    check "SIGTERM exits 0" stop_trigger
}

test_reconnect()
{
    echo "== reconnects when the socket goes away and comes back"
    reset
    event connected > "$FEED"
    write_conf
    start_server || return
    start_trigger
    check "connected" wait_for "$LOG" "connected to $SOCK"

    stop_server
    check "noticed disconnect" wait_for "$LOG" "disconnected from $SOCK"
    check "waits for the socket" wait_for "$LOG" "waiting for $SOCK"

    event request_start_scanning > "$FEED"
    start_server || return
    check "scans after reconnecting" wait_for "$LOG" "scan: saved" 10
    check "SIGTERM exits 0" stop_trigger
}

test_sighup_reload()
{
    echo "== SIGHUP reloads the config"
    reset
    : > "$FEED"
    write_conf
    start_server || return
    start_trigger
    check "connected" wait_for "$LOG" "connected to $SOCK"

    write_conf "NAPS2_DPI=600" "OUTPUT_NAME='reloaded.pdf'"
    kill -HUP "$TRIGGER_PID"
    check "config reloaded" wait_for "$LOG" "config reloaded"

    echo "NAPS2_SOURCE=nowhere" >> "$CONF"
    kill -HUP "$TRIGGER_PID"
    check "bad config rejected" wait_for "$LOG" "reload failed, keeping current settings"
    check "still running" kill -0 "$TRIGGER_PID"

    event request_start_scanning | send_new_feed
    check "new OUTPUT_NAME used" wait_for "$LOG" "scan: saved $OUT/reloaded.pdf" 10
    check "new DPI used, old source kept" bash -c "grep -qx 600 '$CALLS/1' && grep -qx duplex '$CALLS/1'"
    check "SIGTERM exits 0" stop_trigger
}

test_sigterm_during_scan()
{
    echo "== SIGTERM during a scan stops NAPS2 and everything it started"
    reset
    event request_start_scanning > "$FEED"
    write_conf
    start_server || return
    STUB_SECONDS=30 start_trigger
    check "scan started" wait_count 1
    sleep 0.3
    local t0=$SECONDS
    check "SIGTERM exits 0" stop_trigger
    check "exited promptly" test $((SECONDS - t0)) -le 3
    # setsid makes the stub a process-group leader, so its PID is the group ID.
    check "stub's whole process group stopped" bash -c "sleep 0.3; ! pgrep -g \$(cat '$T/stub.pid') >/dev/null"
}

test_naps2_command_line
test_options_omitted_when_empty
test_invalid_config_rejected
test_airscan_config
test_other_events_ignored
test_json_parsing
test_trigger_list
test_presses_during_scan_ignored
test_no_pages
test_failed_scan
test_reconnect
test_sighup_reload
test_sigterm_during_scan

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
