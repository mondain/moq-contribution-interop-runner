#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
    printf 'Usage: %s DRAFT TRANSPORT RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
draft=$1
transport=$2
runner_bin=$(realpath "$3")
publisher_bin=$(realpath "$4")
fixture=$(realpath "$5")
[[ "$draft" == 18 || "$draft" == 21 ]] || exit 2
[[ "$transport" == native_quic || "$transport" == webtransport ]] || exit 2
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/moq-interop-driven.XXXXXX)
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19301}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19302}
runner_pid=

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
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
MOQXR_BIN="$publisher_bin" "$runner_bin" \
    --bind 127.0.0.1 --port "$http_port" \
    --database "$test_dir/runs.sqlite3" \
    --docs "$root_dir/docs" --requirements "$root_dir/requirements" \
    --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port" \
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem" \
    --driver-executable "$root_dir/adapters/moqxr/run.sh" \
    --driver-fixture "$fixture" --driver-log-root "$test_dir/logs" \
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

scenario=subscribe-to-publisher-track
if [[ "$draft" == 21 ]]; then scenario=d21-publisher-request-stream-placement; fi
api_transport=$transport
if [[ "$transport" == native_quic ]]; then api_transport=native-quic; fi
request=$(jq -n --argjson draft "$draft" --arg transport "$api_transport" \
    --arg scenario "$scenario" '{draft: $draft, transport: $transport,
    mode: "driven", scenarios: [$scenario], timeout_ms: 12000,
    track: {namespace_hex: ["6d65646961"], name_hex: "766964655f31"}}')
created=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' -d "$request")
run_id=$(jq -er '.run.id' <<<"$created")
result=
for attempt in {1..160}; do
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
if [[ $(jq '[.items[] | select(.kind == "publisher_process")] | length' \
    <<<"$events") -ne 1 || ! -f "$test_dir/logs/$run_id/request.json" ||
    $(jq '.requirements | length' <<<"$exported") -eq 0 ]]; then
    printf 'missing driver evidence, retained contract, or requirement rows\n' >&2
    sed -n '1,120p' "$test_dir/runner.log" >&2
    exit 1
fi
printf 'draft=%s transport=%s run=%s verdict=%s publisher=%s pass=%s fail=%s\n' \
    "$draft" "$transport" "$run_id" \
    "$(jq -r '.run.verdict' <<<"$result")" \
    "$(jq -r '.items[] | select(.kind == "publisher_process") | .detail | fromjson | .status' \
        <<<"$events")" \
    "$(jq '[.run.outcomes[] | select(.state == "pass")] | length' <<<"$result")" \
    "$(jq '[.run.outcomes[] | select(.state == "fail")] | length' <<<"$result")"
