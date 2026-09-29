#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 || ( "$3" != 18 && "$3" != 21 ) ]]; then
    printf 'Usage: %s RUNNER_BIN QUICHE_PEER_BIN 18|21\n' "$0" >&2
    exit 2
fi

runner_bin=$1
peer_bin=$2
draft=$3
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-$((19170 + draft))}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-$((19270 + draft))}
test_dir=$(mktemp -d /tmp/moq-interop-native-peer.XXXXXX)
if [[ "$draft" == 21 ]]; then
    scenario=d21-publisher-request-stream-placement
    peer_action=draft21-publish
else
    scenario=subscribe-to-publisher-track
    peer_action=draft18-subscribe-ok
fi
runner_pid=

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    case "$test_dir" in
        /tmp/moq-interop-native-peer.*)
            rm -f -- "$test_dir/cert.pem" "$test_dir/key.pem" \
                "$test_dir/runs.sqlite3" "$test_dir/runs.sqlite3-shm" \
                "$test_dir/runs.sqlite3-wal" "$test_dir/runner.log"
            rmdir -- "$test_dir"
            ;;
    esac
}
trap cleanup EXIT

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -days 1 >/dev/null 2>&1

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
    sleep 0.1
done

run_json=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' \
    -d "{\"draft\":$draft,\"transport\":\"native-quic\",\"mode\":\"observed\",\"scenarios\":[\"$scenario\"],\"timeout_ms\":3000,\"track\":{\"namespace_hex\":[\"6d65646961\"],\"name_hex\":\"74657374\"}}")
run_id=$(jq -r '.run.id' <<<"$run_json")
endpoint_port=$(jq -r '.publisher_endpoint.port' <<<"$run_json")
if [[ "$run_id" == null || "$endpoint_port" != "$udp_port" ]]; then
    printf 'invalid run response: %s\n' "$run_json" >&2
    exit 1
fi

if "$peer_bin" "$endpoint_port" "moqt-$draft" "$peer_action"; then
    :
else
    peer_status=$?
    printf 'synthetic publisher failed with exit %s\n' "$peer_status" >&2
    curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id" |
        jq -c '{state:.run.state,events:.run.events.total,verdict:.run.verdict}' >&2
    curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events" |
        jq -c '[.items[].kind]' >&2
    sed -n '1,100p' "$test_dir/runner.log" >&2
    exit 1
fi

for attempt in {1..50}; do
    result_json=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
    if [[ $(jq -r '.run.state' <<<"$result_json") == finalized ]]; then
        break
    fi
    sleep 0.1
done
events_json=$(curl --fail --silent --show-error \
    "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events")
passed=$(jq '[.run.outcomes[] | select(.state == "pass")] | length' \
    <<<"$result_json")
if [[ $(jq -r '.run.state' <<<"$result_json") != finalized ]] || \
   [[ "$passed" -eq 0 ]] || \
   ! jq -e --arg draft "$draft" '.items | map(.kind) | if $draft == "21" then index("transport_established") != null and index("publish_observed") != null and index("response_delivered") != null else index("transport_established") != null and index("initial_response_observed") != null end' \
      <<<"$events_json" >/dev/null; then
    jq -c '{state:.run.state,verdict:.run.verdict,score:.run.score,passing:[.run.outcomes[]|select(.state=="pass")|.requirement_id]}' \
        <<<"$result_json" >&2
    jq -c '[.items[].kind]' <<<"$events_json" >&2
    sed -n '1,100p' "$test_dir/runner.log" >&2
    exit 1
fi

printf 'draft-%s native publisher: %s passing requirements for %s\n' \
    "$draft" "$passed" "$run_id"
if [[ -n ${MOQ_INTEROP_NATIVE_PARITY_OUTPUT:-} ]]; then
    jq -cS '{verdict:.run.verdict,score:.run.score,outcomes:[.run.outcomes[]|{requirement_id,state}]}' \
        <<<"$result_json" >"$MOQ_INTEROP_NATIVE_PARITY_OUTPUT"
    jq -cS '[.items[]|{kind,requirement_id}]' \
        <<<"$events_json" >>"$MOQ_INTEROP_NATIVE_PARITY_OUTPUT"
fi
