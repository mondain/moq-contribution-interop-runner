#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    printf 'Usage: %s DRAFT RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi

draft=$1
runner_bin=$2
publisher_bin=$3
media_file=$4
if [[ "$draft" != 18 && "$draft" != 21 ]]; then
    printf 'draft must be 18 or 21\n' >&2
    exit 2
fi
scenario=subscribe-to-publisher-track
publisher_forward=0
if [[ "$draft" == 21 ]]; then
    scenario=d21-publisher-request-stream-placement
    publisher_forward=1
fi
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19191}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19192}
test_dir=$(mktemp -d /tmp/moq-interop-wt-e2e.XXXXXX)
runner_pid=

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    case "$test_dir" in
        /tmp/moq-interop-wt-e2e.*)
            rm -f -- "$test_dir/cert.pem" "$test_dir/key.pem" \
                "$test_dir/runs.sqlite3" "$test_dir/runs.sqlite3-shm" \
                "$test_dir/runs.sqlite3-wal" "$test_dir/runner.log" \
                "$test_dir/publisher.log" "$test_dir/stop.json"
            rmdir -- "$test_dir"
            ;;
    esac
}
trap cleanup EXIT

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -days 1 >/dev/null 2>&1

"$runner_bin" --bind 127.0.0.1 --port "$http_port" \
    --database "$test_dir/runs.sqlite3" \
    --docs "$root_dir/docs" --requirements "$root_dir/requirements" \
    --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port" \
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem" \
    >"$test_dir/runner.log" 2>&1 &
runner_pid=$!

for attempt in {1..50}; do
    if curl --fail --silent --output /dev/null \
        "http://127.0.0.1:$http_port/healthz"; then
        break
    fi
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        printf 'runner exited during startup\n' >&2
        sed -n '1,100p' "$test_dir/runner.log" >&2
        exit 1
    fi
    sleep 0.1
done

run_json=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' \
    -d "{\"draft\":$draft,\"transport\":\"webtransport\",\"mode\":\"observed\",\"scenarios\":[\"$scenario\"],\"timeout_ms\":20000,\"track\":{\"namespace_hex\":[\"6d65646961\"],\"name_hex\":\"766964655f31\"}}")
run_id=$(jq -r '.run.id' <<<"$run_json")
endpoint_url=$(jq -r '.publisher_endpoint.url' <<<"$run_json")
endpoint_alpn=$(jq -r '.publisher_endpoint.alpn' <<<"$run_json")
endpoint_protocol=$(jq -r '.publisher_endpoint.protocol' <<<"$run_json")
if [[ "$run_id" == null || "$endpoint_url" != "https://127.0.0.1:$udp_port/moq" ||
      "$endpoint_alpn" != h3 || "$endpoint_protocol" != "moqt-$draft" ]]; then
    printf 'invalid run creation response: %s\n' "$run_json" >&2
    exit 1
fi

set +e
OPENMOQ_PICOQUIC_TRACE=1 "$publisher_bin" \
    --input "$media_file" --endpoint "$endpoint_url" \
    --transport webtransport --namespace media --draft "$draft" \
    --forward "$publisher_forward" --timeout 10 --insecure \
    >"$test_dir/publisher.log" 2>&1
publisher_exit=$?
set -e

current_state=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id" | jq -r '.run.state')
if [[ "$current_state" != finalized ]]; then
    curl --silent --show-error -X POST \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id/stop" \
        -H 'Content-Type: application/json' -d '' \
        --output "$test_dir/stop.json" >/dev/null
fi
result_json=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
setup_events=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events?limit=100" |
    jq '[.items[] | select(.kind == "peer_setup_received")] | length')
pass_count=$(jq '[.run.outcomes[] | select(.state == "pass")] | length' \
    <<<"$result_json")
printf 'publisher_exit=%s run_id=%s state=%s verdict=%s setup_events=%s\n' \
    "$publisher_exit" "$run_id" \
    "$(jq -r '.run.state' <<<"$result_json")" \
    "$(jq -r '.run.verdict' <<<"$result_json")" "$setup_events"
jq -r '.run.outcomes[] | select(.state == "pass" or .state == "fail") | "\(.requirement_id) \(.state)"' \
    <<<"$result_json"

if [[ "$publisher_exit" -ne 0 || "$setup_events" -eq 0 || "$pass_count" -eq 0 ]]; then
    printf 'recorded events:\n' >&2
    curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events?limit=100" |
        jq -r '.items[] | "\(.kind) \(.detail)"' >&2
    printf 'publisher log:\n' >&2
    sed -n '1,140p' "$test_dir/publisher.log" >&2
    printf 'runner log:\n' >&2
    sed -n '1,140p' "$test_dir/runner.log" >&2
    exit 1
fi
