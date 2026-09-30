#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 || ( "$1" != 18 && "$1" != 21 ) ]]; then
    printf 'Usage: %s DRAFT RUNNER_BIN AUDIT_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
draft=$1
runner_bin=$(realpath "$2")
audit_bin=$(realpath "$3")
publisher_bin=$(realpath "$4")
fixture=$(realpath "$5")
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
artifact_dir=${MOQ_INTEROP_REPEAT_ARTIFACT_DIR:-}
test_dir=$(mktemp -d /tmp/moq-interop-repeat.XXXXXX)
runner_pid=
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19341}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19342}

cleanup() {
    local status=$?
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    if [[ -n "$artifact_dir" ]]; then
        mkdir -p -- "$artifact_dir"
        for name in audit.json runner.log runs.sqlite3 runs.sqlite3-wal \
                    runs.sqlite3-shm; do
            if [[ -f "$test_dir/$name" ]]; then
                cp -- "$test_dir/$name" "$artifact_dir/$name"
            fi
        done
        if [[ -d "$test_dir/logs" ]]; then
            cp -a -- "$test_dir/logs" "$artifact_dir/logs"
        fi
    fi
    if [[ "$status" -ne 0 && "${MOQ_INTEROP_KEEP_FAILED:-0}" == 1 ]]; then
        printf 'retained repeatability artifacts: %s\n' "$test_dir" >&2
        return
    fi
    case "$test_dir" in
        /tmp/moq-interop-repeat.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
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
for attempt in {1..50}; do
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
request=$(jq -n --argjson draft "$draft" --arg scenario "$scenario" '{
    draft: $draft, transport: "webtransport", mode: "driven",
    scenarios: [$scenario], timeout_ms: 10000,
    track: {namespace_hex: ["6d65646961"], name_hex: "766964655f31"}}')
for repetition in 1 2 3; do
    created=$(curl --fail --silent --show-error -X POST \
        "http://127.0.0.1:$http_port/api/v1/runs" \
        -H 'Content-Type: application/json' -d "$request")
    run_id=$(jq -er '.run.id' <<<"$created")
    state=active
    for attempt in {1..130}; do
        state=$(curl --fail --silent --show-error \
            "http://127.0.0.1:$http_port/api/v1/runs/$run_id" | jq -r '.run.state')
        if [[ "$state" == finalized ]]; then break; fi
        sleep 0.1
    done
    [[ "$state" == finalized ]] || exit 1
    printf 'repeat=%s draft=%s run=%s finalized\n' "$repetition" "$draft" "$run_id"
done
kill "$runner_pid"
wait "$runner_pid"
runner_pid=

set +e
"$audit_bin" --draft "$draft" --database "$test_dir/runs.sqlite3" \
    --docs "$root_dir/docs" --requirements "$root_dir/requirements" \
    --format json >"$test_dir/audit.json"
audit_status=$?
set -e
[[ "$audit_status" -eq 1 ]] || exit 1
if ! jq -e '.execution_audit.consistent == true and
    .execution_audit.run_count == 3 and
    .execution_audit.scored_rows > 0' "$test_dir/audit.json" >/dev/null; then
    jq '.execution_audit' "$test_dir/audit.json" >&2
    sed -n '1,120p' "$test_dir/runner.log" >&2
    exit 1
fi
printf 'draft=%s three-run repeatability passed\n' "$draft"
