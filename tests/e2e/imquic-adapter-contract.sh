#!/usr/bin/env bash
# Contract test of adapters/imquic/run.sh, the adapter for imquic's moq-pub example publisher.
#
# A stub IMQUIC_PUB_BIN records its arguments (one `<arg>` per line) on standard output, which the
# adapter redirects into <log_dir>/publisher.log; a second stub sleeps until it is signalled, to show
# that the adapter's `timeout` wrapper ends a publisher the runner never stops. Needs bash, jq and
# coreutils `timeout`; no network.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/imquic/run.sh"
capture_source="$root_dir/tests/support/capture_publisher.sh"
test_dir=$(mktemp -d /tmp/imquic-adapter-contract.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
log_dir="$test_dir/log dir"
mkdir -p "$log_dir"
touch "$test_dir/fixture.mp4" "$test_dir/ca.pem"
capture="$test_dir/publisher binary"
ln -s "$capture_source" "$capture"

fail() {
    printf 'imquic adapter contract: %s\n' "$1" >&2
    exit 1
}

# make_request DRAFT TRANSPORT [ENDPOINT] [SCENARIO] [NAMESPACE_JSON] [TRACK_HEX] [TIMEOUT_MS]
make_request() {
    local draft=$1 transport=$2 endpoint=${3:-} scenario=${4:-d22-successful-subscribe-response}
    local namespace=${5:-'["6d65646961"]'} track=${6:-766964655f31} timeout_ms=${7:-2500}
    if [[ -z "$endpoint" ]]; then
        if [[ "$transport" == webtransport ]]; then
            endpoint=https://127.0.0.1:4443/moq
        else
            endpoint=moqt://127.0.0.1:4443/moq
        fi
    fi
    jq -n --arg endpoint "$endpoint" --arg transport "$transport" --arg scenario "$scenario" \
        --arg fixture "$test_dir/fixture.mp4" --arg ca "$test_dir/ca.pem" --arg log_dir "$log_dir" \
        --argjson namespace "$namespace" --arg track "$track" --argjson draft "$draft" \
        --argjson timeout_ms "$timeout_ms" '{
            schema_version: 1, run_id: "run 1", scenario_id: $scenario,
            endpoint: $endpoint, draft: $draft, transport: $transport,
            namespace_hex: $namespace, track_name_hex: $track,
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: $timeout_ms, process_timeout_ms: ($timeout_ms + 1000)
        }' >"$test_dir/request.json"
}

# Runs the adapter on request.json with BIN as IMQUIC_PUB_BIN. Sets `status`, `out` (the adapter's
# own standard output), `err` (its standard error) and `log` (publisher.log, empty if absent).
run_adapter() {
    local bin=${1-$capture}
    rm -f -- "$log_dir/publisher.log"
    set +e
    IMQUIC_PUB_BIN="$bin" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" \
        "$adapter" >"$test_dir/out" 2>"$test_dir/err"
    status=$?
    set -e
    out=$(cat "$test_dir/out")
    err=$(cat "$test_dir/err")
    log=
    [[ -f "$log_dir/publisher.log" ]] && log=$(cat "$log_dir/publisher.log")
    return 0
}

# expect_refused MESSAGE_FRAGMENT: the adapter exited 64, said why, and started nothing.
expect_refused() {
    local what=$1
    [[ "$status" -eq 64 ]] || fail "expected exit 64 for $what, got $status (stderr: $err)"
    [[ "$err" == "imquic adapter: "*"$what"* ]] || fail "refusal message for $what: $err"
    [[ ! -e "$log_dir/publisher.log" ]] || fail "publisher started although $what was refused"
}

# args_line: the recorded arguments on one line, `<a> <b> ...`.
args_line() { tr '\n' ' ' <<<"$log" | sed 's/ $//'; }

# Accepted on both transports: the hex is decoded to the moq-pub strings, -M 22, the endpoint becomes
# -r/-R plus -q (raw QUIC) or -w -H PATH (WebTransport), and the output lands in log_dir.
make_request 22 native_quic
run_adapter
[[ "$status" -eq 0 ]] || fail "native QUIC request failed with $status: $err"
[[ -z "$out" ]] || fail "publisher output was not redirected into log_dir: $out"
[[ "$(args_line)" == "<-M> <22> <-n> <media> <-N> <vide_1> <-r> <127.0.0.1> <-R> <4443> <-q> <-d> <4>" ]] ||
    fail "native QUIC arguments: $(args_line)"
make_request 22 webtransport
run_adapter
[[ "$status" -eq 0 ]] || fail "WebTransport request failed with $status: $err"
[[ -z "$out" ]] || fail "publisher output was not redirected into log_dir: $out"
[[ "$(args_line)" == "<-M> <22> <-n> <media> <-N> <vide_1> <-r> <127.0.0.1> <-R> <4443> <-w> <-H> </moq> <-d> <4>" ]] ||
    fail "WebTransport arguments: $(args_line)"

# Publish-first (-X) versus announce-and-wait (no -X), and the one emission option (-D datagram).
make_request 22 native_quic "" d22-publisher-location-filter-parameter
run_adapter
[[ "$status" -eq 0 && "$log" == *"<-X>"* ]] || fail "publisher-location-filter-parameter must publish first: $(args_line)"
make_request 22 native_quic "" d22-subscribe-bounded-location-range
run_adapter
[[ "$status" -eq 0 && "$log" != *"<-X>"* ]] || fail "subscribe-bounded-location-range must announce and wait: $(args_line)"
make_request 22 webtransport "" d22-subscribe-empty-namespace-field
run_adapter
[[ "$status" -eq 0 && "$log" != *"<-X>"* ]] || fail "a moqxr paced override must announce and wait: $(args_line)"
make_request 22 webtransport "" d22-subscribe-single-subgroup
run_adapter
[[ "$status" -eq 0 && "$log" != *"<-X>"* ]] || fail "subscribe-single-subgroup must announce and wait: $(args_line)"
for id in d22-setup-key-value-type-overflow d22-setup-key-value-declared-length-overflow \
          d22-setup-register-default-zero-cache; do
    make_request 22 native_quic "" "$id"
    run_adapter
    [[ "$status" -eq 0 && "$log" != *"<-X>"* ]] ||
        fail "$id must announce and wait (its probe SUBSCRIBE needs an accepting publisher): $(args_line)"
done
make_request 22 native_quic "" d22-publish-ok-with-track-properties
run_adapter
[[ "$status" -eq 0 && "$log" == *"<-X>"* ]] || fail "publish-ok-with-track-properties must publish first: $(args_line)"
make_request 22 native_quic "" d22-object-datagram-flags
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-q> <-D> <datagram> <-d> <4>" && "$log" != *"<-X>"* ]] ||
    fail "object-datagram-flags arguments: $(args_line)"

# Endpoint translation: IPv6 literals lose their brackets, host names pass, an empty WebTransport path
# is "/", a WebTransport query stays in the HTTP/3 path, and odd ports are normalized.
make_request 22 native_quic 'moqt://[::1]:4443/moq'
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-r> <::1> <-R> <4443> <-q>"* ]] || fail "IPv6 native endpoint: $(args_line) $err"
make_request 22 webtransport 'https://[fe80::1:2]:443/moq'
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-r> <fe80::1:2> <-R> <443> <-w> <-H> </moq>"* ]] ||
    fail "IPv6 WebTransport endpoint: $(args_line) $err"
make_request 22 webtransport 'https://localhost:65535'
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-r> <localhost> <-R> <65535> <-w> <-H> </>"* ]] ||
    fail "WebTransport endpoint without a path: $(args_line) $err"
make_request 22 webtransport 'https://relay.example:4443/moq/a?run=1'
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-r> <relay.example> <-R> <4443> <-w> <-H> </moq/a?run=1>"* ]] ||
    fail "WebTransport endpoint with a query: $(args_line) $err"
make_request 22 native_quic 'moqt://127.0.0.1:04443/other'
run_adapter
[[ "$status" -eq 0 && "$(args_line)" == *"<-r> <127.0.0.1> <-R> <4443> <-q>"* ]] ||
    fail "native endpoint with another path: $(args_line) $err"

# Endpoints imquic's command line cannot express are refused with exit 64.
make_request 22 native_quic 'moqt://127.0.0.1:4443/moq?run=1'
run_adapter
expect_refused 'query'
make_request 22 native_quic 'moqt://127.0.0.1:4443/moq?'
run_adapter
expect_refused 'query'
for endpoint in 'moqt://:4443/moq' 'moqt://127.0.0.1/moq' 'moqt://127.0.0.1:0/moq' \
                'moqt://127.0.0.1:65536/moq' 'moqt://127.0.0.1:99999999999999999999/moq' \
                'moqt://user@127.0.0.1:4443/moq' 'moqt://127.0.0.1:4443/moq#frag' \
                'moqt://[fe80::1%25eth0]:4443/moq' 'moqt://-r:4443/moq' 'moqt://127.0.0.1:4443/a b' \
                'moqt://[::1:4443/moq' 'moqt://127.0.0.1:4443x/moq'; do
    make_request 22 native_quic "$endpoint"
    run_adapter
    expect_refused 'endpoint'
done
make_request 22 webtransport 'https://127.0.0.1:4443/moq#frag'
run_adapter
expect_refused 'endpoint'
make_request 22 native_quic 'https://127.0.0.1:4443/moq'
run_adapter
expect_refused 'moqt'
make_request 22 webtransport 'moqt://127.0.0.1:4443/moq'
run_adapter
expect_refused 'https'

# Drafts other than 22 are refused before the publisher starts.
for draft in 18 21 23; do
    make_request "$draft" native_quic
    run_adapter
    expect_refused "draft $draft is not supported (supported drafts: 22)"
done
# An unknown transport, another namespace or track, and malformed requests are refused.
make_request 22 quic
run_adapter
expect_refused 'unsupported or malformed request'
make_request 22 native_quic "" d22-successful-subscribe-response '["00"]'
run_adapter
expect_refused 'unsupported or malformed request'
make_request 22 native_quic "" d22-successful-subscribe-response '["6d65646961", "6d65646961"]'
run_adapter
expect_refused 'unsupported or malformed request'
make_request 22 native_quic "" d22-successful-subscribe-response '["6d65646961"]' 00
run_adapter
expect_refused 'unsupported or malformed request'
make_request 22 native_quic "" d22-successful-subscribe-response '["6d65646961"]' 766964655f31 0
run_adapter
expect_refused 'unsupported or malformed request'
printf '{"schema_version": 1, "draft": 22' >"$test_dir/request.json"
run_adapter
expect_refused 'unsupported or malformed request'
make_request 22 native_quic
jq '.log_dir = "'"$test_dir"'/missing"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
run_adapter
[[ "$status" -eq 64 && "$err" == *"log_dir"* ]] || fail "missing log_dir accepted ($status: $err)"
# The contract version and the publisher binary are checked first.
make_request 22 native_quic
set +e
IMQUIC_PUB_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=2 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>"$test_dir/err"
status=$?
set -e
[[ "$status" -eq 64 ]] && grep -q 'unsupported driver contract version' "$test_dir/err" ||
    fail "contract version 2 accepted ($status)"
run_adapter ""
expect_refused 'IMQUIC_PUB_BIN must name an executable'
run_adapter "$test_dir/fixture.mp4"
expect_refused 'IMQUIC_PUB_BIN must name an executable'
set +e
env -u IMQUIC_PUB_BIN MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>"$test_dir/err"
status=$?
set -e
[[ "$status" -eq 64 ]] && grep -q 'IMQUIC_PUB_BIN must name an executable' "$test_dir/err" ||
    fail "missing IMQUIC_PUB_BIN accepted ($status)"

# imquic reads neither the fixture nor a CA (it publishes a clock and does not verify TLS): an
# unconfigured fixture or trust file does not stop it.
make_request 22 native_quic
jq '.fixture = "" | .tls_ca = ""' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
run_adapter
[[ "$status" -eq 0 && "$log" == *"<-M>"* ]] || fail "empty fixture and tls_ca refused ($status: $err)"

# The publisher runs under `timeout`: a publisher that never exits on its own is sent SIGTERM at the
# scenario timeout (1 s, rounded up) plus 3 s, and its own exit status (0 here) is kept.
sleeper="$test_dir/sleeping publisher"
cat >"$sleeper" <<'STUB'
#!/usr/bin/env bash
sleep 30 &
child=$!
trap 'kill "$child" 2>/dev/null; printf "stub: SIGTERM\n"; exit 0' TERM
printf 'stub: started\n'
wait "$child"
printf 'stub: not signalled\n'
exit 3
STUB
chmod +x "$sleeper"
make_request 22 native_quic "" d22-successful-subscribe-response '["6d65646961"]' 766964655f31 1000
started=$SECONDS
run_adapter "$sleeper"
elapsed=$((SECONDS - started))
[[ "$status" -eq 0 ]] || fail "timed-out publisher exit status $status (expected its own 0)"
[[ "$log" == *"stub: started"*"stub: SIGTERM"* ]] || fail "publisher was not sent SIGTERM: $log"
((elapsed >= 3 && elapsed <= 8)) || fail "publisher ended after ${elapsed}s (expected about 4s)"
# --preserve-status: a publisher that exits 5 on the deadline's SIGTERM makes the adapter exit 5.
sed 's/exit 0. TERM/exit 5'"'"' TERM/' "$sleeper" >"$test_dir/exit5 publisher"
chmod +x "$test_dir/exit5 publisher"
grep -q "exit 5' TERM" "$test_dir/exit5 publisher" || fail 'could not derive the exit-5 stub'
run_adapter "$test_dir/exit5 publisher"
[[ "$status" -eq 5 && "$log" == *"stub: SIGTERM"* ]] || fail "timed-out publisher exit 5 became $status: $log"

# Normal completion: the adapter waits for the publisher and returns its exit status.
early="$test_dir/early publisher"
printf '#!/usr/bin/env bash\nprintf "stub: done\\n"\nexit %s\n' 0 >"$early"
chmod +x "$early"
make_request 22 native_quic
run_adapter "$early"
[[ "$status" -eq 0 && "$log" == "stub: done" ]] || fail "publisher exiting 0 by itself gave $status: $log"
printf '#!/usr/bin/env bash\nsleep 0.2\nprintf "stub: done\\n"\nexit %s\n' 7 >"$early"
run_adapter "$early"
[[ "$status" -eq 7 && "$log" == "stub: done" ]] || fail "publisher exiting 7 by itself gave $status: $log"

# Shutdown by the runner. The runner starts the adapter as a process group leader, sends SIGTERM to
# the whole group and SIGKILLs the group 100 ms later, recording a SIGKILL as a driver failure
# (src/app/publisher_driver.cpp, native_run_manager.cpp driver_failed). moq-pub needs up to ~160 ms
# to finish after SIGTERM, so the adapter itself must exit 0 within the grace while the publisher,
# which received the group's SIGTERM, finishes on its own (bounded by `timeout -k 2`).
#
# group_term.py mirrors the runner: it spawns the adapter in a new session, waits for the stub's
# "started" line, sends SIGTERM to the group, polls the adapter's exit every millisecond, then waits
# until no process of the group is left. It prints `adapter_ms status group_ms`.
cat >"$test_dir/group_term.py" <<'PY'
import os, signal, subprocess, sys, time
adapter, log, out, err = sys.argv[1:5]
limit = float(sys.argv[5])
def group_alive(pgid):
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            with open(f'/proc/{entry}/stat') as f:
                fields = f.read().rsplit(')', 1)[1].split()
        except OSError:
            continue
        if fields[0] != 'Z' and int(fields[2]) == pgid:
            return True
    return False
with open(out, 'wb') as o, open(err, 'wb') as e:
    p = subprocess.Popen([adapter], stdin=subprocess.DEVNULL, stdout=o, stderr=e,
                         start_new_session=True)
pgid = p.pid
deadline = time.monotonic() + 10
while True:
    try:
        with open(log) as f:
            if 'stub: started' in f.read():
                break
    except OSError:
        pass
    if time.monotonic() > deadline or p.poll() is not None:
        os.killpg(pgid, signal.SIGKILL)
        print('-1 not-started -1')
        sys.exit(0)
    time.sleep(0.005)
time.sleep(0.05)
t0 = time.monotonic()
os.killpg(pgid, signal.SIGTERM)
adapter_ms, status = -1, 'running'
while time.monotonic() - t0 < limit:
    code = p.poll()
    if code is not None:
        adapter_ms, status = int((time.monotonic() - t0) * 1000), code
        break
    time.sleep(0.001)
group_ms = -1
while time.monotonic() - t0 < limit:
    if not group_alive(pgid):
        group_ms = int((time.monotonic() - t0) * 1000)
        break
    time.sleep(0.005)
if group_ms < 0:
    os.killpg(pgid, signal.SIGKILL)
if p.poll() is None:
    p.kill()
p.wait()
print(adapter_ms, status, group_ms)
PY

# group_term BIN LIMIT_S: runs group_term.py on request.json; sets adapter_ms, status, group_ms, log.
group_term() {
    rm -f -- "$log_dir/publisher.log"
    read -r adapter_ms status group_ms < <(IMQUIC_PUB_BIN="$1" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" \
        python3 "$test_dir/group_term.py" "$adapter" "$log_dir/publisher.log" "$test_dir/out" \
        "$test_dir/err" "$2") || fail 'group_term.py gave no result'
    log=
    [[ -f "$log_dir/publisher.log" ]] && log=$(cat "$log_dir/publisher.log")
    return 0
}

# A publisher like moq-pub: SIGTERM starts a 300 ms cleanup, then it exits 0. It counts the SIGTERMs
# it receives: the group's and the one `timeout` relays, one or two in all (the adapter adds none).
# moq-pub itself exits(1) when a signal takes its stop counter past two, which connection loss or
# GOAWAY can bump first; this stub has no such counter.
slow="$test_dir/slow publisher"
cat >"$slow" <<'STUB'
#!/usr/bin/env bash
terms=0
trap 'terms=$((terms + 1))' TERM
printf 'stub: started\n'
sleep 30 &
wait "$!"
sleep 0.3
printf 'stub: cleanup done after %s SIGTERM(s)\n' "$terms"
exit 0
STUB
chmod +x "$slow"
make_request 22 native_quic "" d22-successful-subscribe-response '["6d65646961"]' 766964655f31 10000
# The adapter's own exit is usually a few ms after the signal; the 100 ms grace is checked with up to
# three attempts so that one scheduling stall on a loaded machine cannot fail the test (an adapter
# that waits for the publisher takes the stub's 300 ms every time). Everything else is checked on
# every attempt with generous bounds.
fast=0
for attempt in 1 2 3; do
    group_term "$slow" 6
    [[ "$status" == 0 ]] || fail "adapter status after the group SIGTERM: $status (attempt $attempt, ${adapter_ms} ms)"
    ((group_ms >= 0 && group_ms <= 1500)) ||
        fail "the publisher's process group outlived the SIGTERM by ${group_ms} ms (attempt $attempt)"
    [[ "$log" =~ stub:\ cleanup\ done\ after\ [12]\ SIGTERM ]] ||
        fail "the publisher did not finish its cleanup after one or two SIGTERMs: $log"
    ((group_ms >= 300)) || fail "the publisher's cleanup was cut short (${group_ms} ms)"
    if ((adapter_ms >= 0 && adapter_ms < 100)); then
        fast=1
        break
    fi
    printf 'imquic adapter contract: attempt %s: adapter exited %s ms after SIGTERM\n' "$attempt" "$adapter_ms" >&2
done
((fast)) || fail "the adapter did not exit within the runner's 100 ms grace (last: ${adapter_ms} ms)"
printf 'group SIGTERM: adapter exited after %s ms, the slow publisher finished after %s ms\n' \
    "$adapter_ms" "$group_ms"

# A publisher that ignores SIGTERM is killed by `timeout -k 2` about 2 s after the signal; the adapter
# still exits 0 at once.
deaf="$test_dir/deaf publisher"
printf '#!/usr/bin/env bash\ntrap "" TERM\nprintf "stub: started\\n"\nexec sleep 30\n' >"$deaf"
chmod +x "$deaf"
group_term "$deaf" 8
[[ "$status" == 0 && "$adapter_ms" -ge 0 && "$adapter_ms" -lt 1000 ]] ||
    fail "adapter with a publisher ignoring SIGTERM: status $status after ${adapter_ms} ms"
((group_ms >= 1500 && group_ms <= 4500)) ||
    fail "a publisher ignoring SIGTERM ended ${group_ms} ms after it (expected timeout -k 2: about 2000)"
printf 'group SIGTERM: adapter exited after %s ms, a publisher ignoring SIGTERM was killed after %s ms\n' \
    "$adapter_ms" "$group_ms"

printf 'imquic adapter contract passed\n'
