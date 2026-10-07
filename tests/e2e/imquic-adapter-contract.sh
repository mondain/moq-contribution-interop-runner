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

printf 'imquic adapter contract passed\n'
