#!/usr/bin/env bash
set -euo pipefail

# Usage: driven-moqxr.sh [--dry-run] DRAFT TRANSPORT RUNNER_BIN MOQXR_BIN MP4_FIXTURE
#   DRAFT      18, 21, 22 or 23. Each draft drives one scenario: draft 18 subscribe-to-publisher-track,
#              draft 21 d21-publisher-request-stream-placement and draft 22 its shared twin
#              d22-publisher-request-stream-placement (the moqxr adapter runs it with the draft 21
#              options and --draft 22: moqt-22 over native QUIC, h3 over WebTransport).
#   TRANSPORT  native_quic or webtransport.
#   --dry-run  print the plan instead of running it: the runner invocation (`runner:` line, the
#              environment and arguments the runner would be started with) and the run request
#              (`request:` line, the POST /api/v1/runs body as compact JSON). Paths that only exist
#              during a run show as @TMP@. Nothing is started, no certificate or temporary directory
#              is created, and the binaries need not exist. Off by default.
# MOQ_INTEROP_TEST_HTTP_PORT and MOQ_INTEROP_TEST_UDP_PORT choose the ports (default 19301/19302).
# MOQ_INTEROP_TEST_KEEP=1 retains the database and logs. At draft 23,
# MOQ_INTEROP_TEST_SCENARIO selects a smoke ID or a d23-probe-* negative probe (default d23-publisher-smoke).
dry_run=0
if [[ "${1:-}" == --dry-run ]]; then
    dry_run=1
    shift
fi
if [[ $# -ne 5 ]]; then
    printf 'Usage: %s DRAFT TRANSPORT RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
draft=$1
transport=$2
realpath_args=()
if ((dry_run)); then realpath_args=(-m); fi
runner_bin=$(realpath "${realpath_args[@]}" "$3")
publisher_bin=$(realpath "${realpath_args[@]}" "$4")
fixture=$(realpath "${realpath_args[@]}" "$5")
[[ "$draft" == 18 || "$draft" == 21 || "$draft" == 22 || "$draft" == 23 ]] || exit 2
[[ "$transport" == native_quic || "$transport" == webtransport ]] || exit 2
scenario=subscribe-to-publisher-track
if [[ "$draft" == 21 ]]; then scenario=d21-publisher-request-stream-placement; fi
if [[ "$draft" == 22 ]]; then scenario=d22-publisher-request-stream-placement; fi
if [[ "$draft" == 23 ]]; then
    scenario=${MOQ_INTEROP_TEST_SCENARIO:-d23-publisher-smoke}
    case "$scenario" in
        d23-publisher-smoke|d23-direct-subscribe-smoke|d23-publish-smoke|d23-probe-*) ;;
        *) printf 'unsupported draft 23 smoke scenario: %s\n' "$scenario" >&2; exit 2 ;;
    esac
fi
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if ((dry_run)); then
    test_dir=@TMP@
else
    test_dir=$(mktemp -d /tmp/moq-interop-driven.XXXXXX)
fi
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19301}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19302}
runner_pid=
runner_args=(--bind 127.0.0.1 --port "$http_port"
    --database "$test_dir/runs.sqlite3"
    --docs "$root_dir/docs" --requirements "$root_dir/requirements"
    --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port"
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem"
    --driver-executable "$root_dir/adapters/moqxr/run.sh"
    --driver-fixture "$fixture" --driver-log-root "$test_dir/logs")

api_transport=$transport
if [[ "$transport" == native_quic ]]; then api_transport=native-quic; fi
request=$(jq -n --argjson draft "$draft" --arg transport "$api_transport" \
    --arg scenario "$scenario" '{draft: $draft, transport: $transport,
    mode: "driven", scenarios: [$scenario], timeout_ms: 12000,
    track: {namespace_hex: ["6d65646961"], name_hex: "766964655f31"}}')

if ((dry_run)); then
    printf 'runner: MOQXR_BIN=<%s> <%s>' "$publisher_bin" "$runner_bin"
    printf ' <%s>' "${runner_args[@]}"
    printf '\n'
    printf 'request: POST http://127.0.0.1:%s/api/v1/runs %s\n' "$http_port" \
        "$(jq -c . <<<"$request")"
    exit 0
fi

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    if [[ "${MOQ_INTEROP_TEST_KEEP:-}" == 1 ]]; then
        printf 'kept %s\n' "$test_dir" >&2
        return
    fi
    case "$test_dir" in
        /tmp/moq-interop-driven.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -days 1 >/dev/null 2>&1
MOQXR_BIN="$publisher_bin" "$runner_bin" "${runner_args[@]}" \
    >"$test_dir/runner.log" 2>&1 &
runner_pid=$!
for attempt in {1..60}; do
    if curl --fail --silent --output /dev/null \
        "http://127.0.0.1:$http_port/healthz"; then break; fi
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        sed -n '1,120p' "$test_dir/runner.log" >&2
        exit 1
    fi
    sleep 0.1
done

created=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' -d "$request")
run_id=$(jq -er '.run.id' <<<"$created")
result=
poll_attempts=160
if [[ "$draft" == 23 ]]; then poll_attempts=300; fi
for ((attempt=0; attempt<poll_attempts; ++attempt)); do
    result=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
    if [[ $(jq -r '.run.state' <<<"$result") == finalized ]]; then break; fi
    sleep 0.1
done
if [[ $(jq -r '.run.state' <<<"$result") != finalized ]]; then
    printf 'run did not finalize: %s\n' "$result" >&2
    exit 1
fi
events=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events?limit=100")
exported=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/results/$run_id.json")
contract_path="$test_dir/logs/$run_id/request.json"
if [[ "$draft" == 23 ]]; then contract_path="$test_dir/logs/$run_id/$scenario/request.json"; fi
if [[ $(jq '[.items[] | select(.kind == "publisher_process")] | length' \
    <<<"$events") -ne 1 || ! -f "$contract_path" ||
    $(jq '.requirements | length' <<<"$exported") -eq 0 ]]; then
    printf 'missing driver evidence, retained contract, or requirement rows\n' >&2
    sed -n '1,120p' "$test_dir/runner.log" >&2
    exit 1
fi
printf 'draft=%s transport=%s run=%s verdict=%s publisher=%s pass=%s fail=%s' \
    "$draft" "$transport" "$run_id" \
    "$(jq -r '.run.verdict' <<<"$result")" \
    "$(jq -r '.items[] | select(.kind == "publisher_process") | .detail | fromjson | .status' \
        <<<"$events")" \
    "$(jq '[.run.outcomes[] | select(.state == "pass")] | length' <<<"$result")" \
    "$(jq '[.run.outcomes[] | select(.state == "fail")] | length' <<<"$result")"

if [[ "$draft" == 23 && "$scenario" == d23-probe-* ]]; then
    # A negative probe passes or fails on how the publisher closes the session; a failed row is a finding, not a harness
    # error. Only a probe that was never sent (the publisher did not get as far as SETUP) fails this script.
    probe=$(jq -r '[.items[] | select(.kind == "draft23_probe") | .detail | fromjson] | last // empty' <<<"$events")
    if [[ -z "$probe" || $(jq -r '.injected' <<<"$probe") != true ]]; then
        printf '\ndraft 23 probe was not sent: %s\n' "$events" >&2
        printf 'set MOQ_INTEROP_TEST_KEEP=1 to retain logs for debugging\n' >&2
        exit 1
    fi
    expected=$(jq -r '.expected_code' <<<"$probe")
    close_code=$(jq -r '.close.code // empty' <<<"$probe")
    if [[ -z "$close_code" ]]; then
        close_text=none
    elif [[ $(jq -r '.close.application' <<<"$probe") == true ]]; then
        close_text=$(printf 'app:0x%x' "$close_code")
    else
        close_text=$(printf 'transport:0x%x' "$close_code")
    fi
    printf ' probe=%s expected=0x%x close=%s\n' "$scenario" "$expected" "$close_text"
elif [[ "$draft" == 23 ]]; then
    smoke=$(jq -r '[.items[] | select(.kind == "draft23_smoke") | .detail | fromjson | .status] | last // "MISSING"' <<<"$events")
    printf ' smoke=%s\n' "$smoke"
    if [[ "$smoke" != PASS || $(jq -r '.run.verdict' <<<"$result") != incomplete ]]; then
        printf 'draft 23 smoke did not demonstrate delivery: %s\n' "$events" >&2
        printf 'set MOQ_INTEROP_TEST_KEEP=1 to retain logs for debugging\n' >&2
        exit 1
    fi
else
    printf '\n'
fi
