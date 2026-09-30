#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'Usage: %s RUNNER_BIN AUDIT_BIN\n' "$0" >&2
    exit 2
fi
runner_bin=$1
audit_bin=$2
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/moq-interop-audit.XXXXXX)
runner_pid=
http_port=19331
udp_port=19332

cleanup() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    case "$test_dir" in
        /tmp/moq-interop-audit.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -days 1 >/dev/null 2>&1
"$runner_bin" --bind 127.0.0.1 --port "$http_port" \
    --database "$test_dir/runs.sqlite3" \
    --docs "$root_dir/docs" --requirements "$root_dir/requirements" \
    --publisher-bind 127.0.0.1 --publisher-port-start "$udp_port" \
    --publisher-port-end "$udp_port" \
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem" \
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
created=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' \
    -d '{"draft":18,"transport":"webtransport","mode":"observed","scenarios":["subscribe-to-publisher-track"],"timeout_ms":100,"track":{"namespace_hex":["6e"],"name_hex":"78"}}')
run_id=$(jq -er '.run.id' <<<"$created")
for attempt in {1..50}; do
    state=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id" | jq -r '.run.state')
    if [[ "$state" == finalized ]]; then break; fi
    sleep 0.1
done
[[ "$state" == finalized ]] || exit 1
kill "$runner_pid"
wait "$runner_pid"
runner_pid=

for draft in 18 21; do
    set +e
    "$audit_bin" --draft "$draft" --database "$test_dir/runs.sqlite3" \
        --docs "$root_dir/docs" --requirements "$root_dir/requirements" \
        --format json >"$test_dir/audit-$draft.json"
    status=$?
    set -e
    [[ "$status" -eq 1 ]] || exit 1
    if [[ "$draft" == 18 ]]; then
        jq -e '.static_complete == false and
            .execution_audit.consistent == true and
            .execution_audit.run_count == 1 and
            .execution_audit.scored_rows == 0 and
            (.execution_audit.runs[0].canonical_sha256 | length) == 64' \
            "$test_dir/audit-$draft.json" >/dev/null
    else
        jq -e '.static_complete == false and
            .execution_audit.consistent == true and
            .execution_audit.run_count == 0' \
            "$test_dir/audit-$draft.json" >/dev/null
    fi
done
printf 'audit CLI database smoke passed\n'
