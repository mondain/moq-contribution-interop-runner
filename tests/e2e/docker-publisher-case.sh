#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 || ( "$1" != 18 && "$1" != 21 ) ||
      ( "$2" != native_quic && "$2" != webtransport ) ]]; then
    printf 'Usage: %s 18|21 native_quic|webtransport IMAGE NATIVE_PEER MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
draft=$1
transport=$2
image=$3
native_peer=$(realpath "$4")
publisher_bin=$(realpath "$5")
fixture=$(realpath "$6")
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
artifact_dir=${MOQ_INTEROP_CASE_ARTIFACT_DIR:-}
source_revision=$(git -C "$root_dir" rev-parse --verify 'HEAD^{commit}')
image_revision=$(docker image inspect --format \
    '{{ index .Config.Labels "org.opencontainers.image.revision" }}' "$image")
if [[ "$image_revision" != "$source_revision" ]]; then
    printf 'Docker image revision mismatch: image=%s source=%s\n' \
        "$image_revision" "$source_revision" >&2
    exit 1
fi
test_dir=$(mktemp -d /tmp/moq-interop-docker-case.XXXXXX)
container="moq-interop-case-$draft-$transport-$$"
container_started=0
port_offset=0
if [[ "$transport" == webtransport ]]; then port_offset=10; fi
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-$((19600 + draft + port_offset))}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-$((19700 + draft + port_offset))}

cleanup() {
    local status=$?
    if [[ -n "$artifact_dir" ]]; then
        mkdir -p -- "$artifact_dir"
        for name in publisher.log request.json stop.json \
                    create-request.json create-response.json; do
            if [[ -f "$test_dir/$name" ]]; then
                cp -- "$test_dir/$name" "$artifact_dir/$name"
            fi
        done
        if [[ -n "${result:-}" ]]; then
            printf '%s\n' "$result" >"$artifact_dir/result.json"
        fi
        if [[ "$container_started" == 1 ]]; then
            docker logs "$container" >"$artifact_dir/container.log" 2>&1 || true
            if [[ -n "${run_id:-}" ]]; then
                curl --fail --silent --show-error \
                    "http://127.0.0.1:$http_port/api/v1/runs/$run_id/events?limit=100" \
                    >"$artifact_dir/events.json" 2>"$artifact_dir/events-error.log" || true
            fi
        fi
    fi
    if [[ "$container_started" == 1 ]]; then
        docker rm -f "$container" >/dev/null 2>&1 || true
    fi
    if [[ "$status" -ne 0 && "${MOQ_INTEROP_KEEP_FAILED:-0}" == 1 ]]; then
        printf 'retained Docker case artifacts: %s\n' "$test_dir" >&2
        return
    fi
    case "$test_dir" in
        /tmp/moq-interop-docker-case.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -days 1 >/dev/null 2>&1
chmod 755 "$test_dir"
chmod 644 "$test_dir/key.pem" "$test_dir/cert.pem"

docker run -d --name "$container" --read-only \
    --tmpfs /tmp:rw,nosuid,size=32m \
    -v "$test_dir:/run/moq-interop-tls:ro" \
    -p "127.0.0.1:$http_port:8080/tcp" \
    -p "127.0.0.1:$udp_port:$udp_port/udp" \
    "$image" --bind 0.0.0.0 --port 8080 \
    --database /tmp/runs.sqlite3 \
    --docs /usr/share/moq-interop/docs \
    --requirements /usr/share/moq-interop/requirements \
    --publisher-bind 0.0.0.0 --publisher-advertise 127.0.0.1 \
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port" \
    --tls-cert /run/moq-interop-tls/cert.pem \
    --tls-key /run/moq-interop-tls/key.pem >/dev/null
container_started=1
healthy=0
for attempt in {1..60}; do
    if curl --fail --silent --output /dev/null \
        "http://127.0.0.1:$http_port/healthz"; then healthy=1; break; fi
    if [[ $(docker inspect -f '{{.State.Running}}' "$container") != true ]]; then
        docker logs "$container" >&2
        exit 1
    fi
    sleep 0.1
done
[[ "$healthy" == 1 ]] || { docker logs "$container" >&2; exit 1; }

scenario=subscribe-to-publisher-track
track_name=766964655f31
if [[ "$draft" == 21 ]]; then scenario=d21-publisher-request-stream-placement; fi
if [[ "$transport" == native_quic ]]; then track_name=74657374; fi
api_transport=$transport
if [[ "$transport" == native_quic ]]; then api_transport=native-quic; fi
request=$(jq -n --argjson draft "$draft" --arg transport "$api_transport" \
    --arg scenario "$scenario" --arg track_name "$track_name" '{
        draft: $draft, transport: $transport, mode: "observed",
        scenarios: [$scenario], timeout_ms: 12000,
        track: {namespace_hex: ["6d65646961"], name_hex: $track_name}}')
if [[ -n "$artifact_dir" ]]; then
    printf '%s\n' "$request" >"$test_dir/create-request.json"
fi
created=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' -d "$request")
run_id=$(jq -er '.run.id' <<<"$created")
if [[ -n "$artifact_dir" ]]; then
    printf '%s\n' "$created" >"$test_dir/create-response.json"
fi
endpoint_port=$(jq -er '.publisher_endpoint.port' <<<"$created")
[[ "$endpoint_port" == "$udp_port" ]] || exit 1

set +e
if [[ "$transport" == native_quic ]]; then
    action=draft18-subscribe-ok
    if [[ "$draft" == 21 ]]; then action=draft21-publish; fi
    "$native_peer" "$udp_port" "moqt-$draft" "$action" \
        >"$test_dir/publisher.log" 2>&1
else
    endpoint=$(jq -er '.publisher_endpoint.url' <<<"$created")
    jq -n --argjson draft "$draft" --arg run_id "$run_id" \
        --arg scenario "$scenario" --arg endpoint "$endpoint" \
        --arg fixture "$fixture" --arg ca "$test_dir/cert.pem" \
        --arg log_dir "$test_dir" '{schema_version: 1, run_id: $run_id,
        scenario_id: $scenario, draft: $draft, transport: "webtransport",
        endpoint: $endpoint, fixture: $fixture, tls_ca: $ca,
        log_dir: $log_dir, scenario_timeout_ms: 12000,
        process_timeout_ms: 14000, namespace_hex: ["6d65646961"],
        track_name_hex: "766964655f31"}' >"$test_dir/request.json"
    MOQXR_BIN="$publisher_bin" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" \
        bash "$root_dir/adapters/moqxr/run.sh" >"$test_dir/publisher.log" 2>&1
fi
publisher_status=$?
set -e

state=active
for attempt in {1..150}; do
    result=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
    state=$(jq -r '.run.state' <<<"$result")
    if [[ "$state" == finalized ]]; then break; fi
    sleep 0.1
done
if [[ "$state" != finalized ]]; then
    curl --fail --silent --show-error -X POST \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id/stop" \
        -H 'Content-Type: application/json' -d '' \
        >"$test_dir/stop.json"
    result=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
fi
pass_count=$(jq '[.run.outcomes[] | select(.state == "pass")] | length' <<<"$result")
fail_count=$(jq '[.run.outcomes[] | select(.state == "fail")] | length' <<<"$result")
printf 'docker draft=%s transport=%s run=%s state=%s verdict=%s publisher_exit=%s pass=%s fail=%s\n' \
    "$draft" "$transport" "$run_id" "$(jq -r '.run.state' <<<"$result")" \
    "$(jq -r '.run.verdict' <<<"$result")" "$publisher_status" \
    "$pass_count" "$fail_count"
if [[ $(jq -r '.run.state' <<<"$result") != finalized ||
      "$pass_count" -eq 0 ||
      ( "$transport" == native_quic && "$publisher_status" -ne 0 ) ||
      "$publisher_status" -eq 64 ]]; then
    jq '{state:.run.state,verdict:.run.verdict,
        passing:[.run.outcomes[]|select(.state=="pass")|.requirement_id]}' \
        <<<"$result" >&2
    sed -n '1,120p' "$test_dir/publisher.log" >&2
    docker logs "$container" >&2
    exit 1
fi
