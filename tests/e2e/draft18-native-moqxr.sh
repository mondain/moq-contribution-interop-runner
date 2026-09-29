#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'Usage: %s RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi

runner_bin=$1
publisher_bin=$2
media_file=$3
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19181}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19182}
test_dir=$(mktemp -d /tmp/moq-interop-e2e.XXXXXX)
runner_pid=

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    case "$test_dir" in
        /tmp/moq-interop-e2e.*)
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
    --publisher-bind 127.0.0.1 \
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
    -d '{"draft":18,"transport":"native-quic","mode":"observed","scenarios":["subscribe-to-publisher-track"],"timeout_ms":20000,"track":{"namespace_hex":["6d65646961"],"name_hex":"766964655f31"}}')
run_id=$(jq -r '.run.id' <<<"$run_json")
endpoint_port=$(jq -r '.publisher_endpoint.port' <<<"$run_json")
if [[ "$run_id" == null || "$endpoint_port" != "$udp_port" ]]; then
    printf 'invalid run creation response: %s\n' "$run_json" >&2
    exit 1
fi

set +e
OPENMOQ_PICOQUIC_TRACE=1 "$publisher_bin" \
    --input "$media_file" --endpoint "moqt://127.0.0.1:$udp_port/moq" \
    --namespace media --draft 18 --forward 0 --timeout 10 --insecure \
    >"$test_dir/publisher.log" 2>&1
publisher_exit=$?
set -e

if [[ "$publisher_exit" -ne 0 ]]; then
    stop_status=$(curl --silent --show-error -X POST \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id/stop" \
        -H 'Content-Type: application/json' -d '' \
        --output "$test_dir/stop.json" --write-out '%{http_code}')
    if [[ "$stop_status" != 200 ]]; then
        printf 'stop returned HTTP %s: ' "$stop_status" >&2
        sed -n '1p' "$test_dir/stop.json" >&2
    fi
fi

for attempt in {1..100}; do
    result_json=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
    if [[ $(jq -r '.run.state' <<<"$result_json") == finalized ]]; then
        break
    fi
    sleep 0.1
done

result_json=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
printf 'publisher_exit=%s run_id=%s state=%s verdict=%s events=%s\n' \
    "$publisher_exit" "$run_id" \
    "$(jq -r '.run.state' <<<"$result_json")" \
    "$(jq -r '.run.verdict' <<<"$result_json")" \
    "$(jq -r '.run.events.total' <<<"$result_json")"
jq -r '.run.outcomes[] | select(.state == "pass" or .state == "fail") | "\(.requirement_id) \(.state)"' \
    <<<"$result_json"

if [[ "$publisher_exit" -ne 0 || \
      $(jq -r '.run.events.total' <<<"$result_json") -eq 0 ]]; then
    printf 'publisher log:\n' >&2
    sed -n '1,120p' "$test_dir/publisher.log" >&2
    printf 'runner log:\n' >&2
    sed -n '1,120p' "$test_dir/runner.log" >&2
    exit 1
fi
