#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'Usage: %s RUNNER_BIN AUDIT_BIN FIXTURE_BIN\n' "$0" >&2
    exit 2
fi
runner_bin=$1
audit_bin=$2
fixture_bin=$3
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/moq-interop-audit22.XXXXXX)
runner_pid=
cleanup() {
    local status=$?
    # A failure after the runner started shows its log.
    if [[ "$status" -ne 0 && -s "$test_dir/runner.log" ]]; then
        printf 'runner.log:\n' >&2
        sed -n '1,120p' "$test_dir/runner.log" >&2
    fi
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    case "$test_dir" in
        /tmp/moq-interop-audit22.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT
common=(--docs "$root_dir/docs" --requirements "$root_dir/requirements")
free_port() {
    python3 - "$1" <<'PY' 2>/dev/null
import socket, sys
kind = socket.SOCK_DGRAM if sys.argv[1] == "udp" else socket.SOCK_STREAM
with socket.socket(socket.AF_INET, kind) as probe:
    probe.bind(("127.0.0.1", 0))
    print(probe.getsockname()[1])
PY
}

set +e
"$audit_bin" --draft 22 "${common[@]}" >"$test_dir/text.out" 2>"$test_dir/text.err"
status=$?
set -e
grep -q '^Required executable coverage: 170/170$' "$test_dir/text.out"
# requirements/draft22.json is complete and every required row is bound, so the static gate
# passes with no blocking finding and the exit status is 0.
[[ "$status" -eq 0 ]] || { echo "unexpected status $status" >&2; exit 1; }
grep -q '^Static gate: PASS' "$test_dir/text.out"

set +e
"$audit_bin" --draft 22 --format json "${common[@]}" >"$test_dir/audit.json"
status=$?
set -e
[[ "$status" -eq 0 ]] || { echo "unexpected JSON status $status" >&2; exit 1; }
jq -e '.draft == 22 and .executable_coverage.required_covered == 170 and
    .executable_coverage.required_total == 170 and .static_complete == true and
    .source_audit.complete == true and
    ([.findings[] | select(.blocking)] | length) == 0' \
    "$test_dir/audit.json" >/dev/null

# The production runner (src/app/main.cpp wiring) accepts a draft 22 run through the API, advertises
# draft 22 as supported and configured, and stores the run as draft 22. No publisher connects, so the
# run times out without scored rows.
http_port=$(free_port tcp || true)
udp_port=$(free_port udp || true)
http_port=${http_port:-19341}
udp_port=${udp_port:-19342}
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -days 1 >/dev/null 2>&1
"$runner_bin" --bind 127.0.0.1 --port "$http_port" \
    --database "$test_dir/runner.sqlite3" "${common[@]}" \
    --publisher-bind 127.0.0.1 --publisher-port-start "$udp_port" \
    --publisher-port-end "$udp_port" \
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem" \
    >"$test_dir/runner.log" 2>&1 &
runner_pid=$!
ready=
for attempt in {1..50}; do
    if curl --fail --silent --output /dev/null "http://127.0.0.1:$http_port/healthz"; then ready=1; break; fi
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        echo "runner exited before answering /healthz" >&2
        exit 1
    fi
    sleep 0.1
done
[[ -n "$ready" ]] || { echo "runner did not answer /healthz on port $http_port within 5 s" >&2; exit 1; }
curl --fail --silent --show-error "http://127.0.0.1:$http_port/healthz" |
    jq -e '.supported_drafts == [18, 21, 22, "moq-lite-06"] and
        ([.executable_profiles[] | select(.draft == 22 and .mode == "observed" and .configured)] | length) > 0' \
        >/dev/null || { echo "healthz does not offer draft 22" >&2; exit 1; }
curl --fail --silent --show-error "http://127.0.0.1:$http_port/api/v1/drafts" |
    jq -e '[.drafts[] | select(.draft == 22 and .runnable and .complete)] | length == 1' >/dev/null ||
    { echo "draft 22 is not listed as runnable" >&2; exit 1; }
created=$(curl --fail --silent --show-error -X POST \
    "http://127.0.0.1:$http_port/api/v1/runs" \
    -H 'Content-Type: application/json' \
    -d '{"draft":22,"transport":"native-quic","mode":"observed","scenarios":["d22-publisher-request-stream-placement"],"timeout_ms":100,"track":{"namespace_hex":["6e"],"name_hex":"78"}}')
jq -e '.run.config.draft == 22 and .publisher_endpoint.alpn == "moqt-22"' <<<"$created" >/dev/null
run_id=$(jq -er '.run.id' <<<"$created")
for attempt in {1..50}; do
    state=$(curl --fail --silent --show-error \
        "http://127.0.0.1:$http_port/api/v1/runs/$run_id" | jq -r '.run.state')
    if [[ "$state" == finalized ]]; then break; fi
    sleep 0.1
done
[[ "$state" == finalized ]] || { echo "draft 22 run did not finalize" >&2; exit 1; }
kill "$runner_pid"
wait "$runner_pid"
runner_pid=
"$audit_bin" --draft 22 --database "$test_dir/runner.sqlite3" "${common[@]}" --format json \
    >"$test_dir/runner-audit.json"
jq -e '.execution_audit.consistent == true and .execution_audit.run_count == 1' \
    "$test_dir/runner-audit.json" >/dev/null

# Draft 22 runs the run manager executed against publisher stand-ins (one shared selection, one own
# scenario): the audit is consistent, with scored rows, and exits 0.
"$fixture_bin" "$test_dir/runs.sqlite3" "$test_dir/tampered.sqlite3" >"$test_dir/fixture.out"
set +e
"$audit_bin" --draft 22 --database "$test_dir/runs.sqlite3" "${common[@]}" \
    >"$test_dir/db.out" 2>"$test_dir/db.err"
status=$?
set -e
[[ "$status" -eq 0 ]] || { echo "unexpected --database status $status" >&2; cat "$test_dir/db.err" >&2; exit 1; }
grep -q '^Static gate: PASS' "$test_dir/db.out"
grep -Eq '^Execution audit: consistent \(2 runs, [1-9][0-9]* scored rows, 0 findings\)$' "$test_dir/db.out"
"$audit_bin" --draft 22 --database "$test_dir/runs.sqlite3" "${common[@]}" --format json \
    >"$test_dir/db.json"
jq -e '.execution_audit.consistent == true and .execution_audit.run_count == 2 and
    .execution_audit.scored_rows > 0 and (.execution_audit.findings | length) == 0 and
    ([.execution_audit.runs[].canonical_sha256 | length] == [64, 64])' "$test_dir/db.json" >/dev/null
# The same database audited as draft 21 holds no draft 21 run.
"$audit_bin" --draft 21 --database "$test_dir/runs.sqlite3" "${common[@]}" --format json \
    >"$test_dir/db21.json" || true
jq -e '.execution_audit.run_count == 0' "$test_dir/db21.json" >/dev/null

# A stored draft 22 run whose passed row lost its declared evidence is a finding, and the gate fails (exit 1).
set +e
"$audit_bin" --draft 22 --database "$test_dir/tampered.sqlite3" "${common[@]}" --format json \
    >"$test_dir/tampered.json"
status=$?
set -e
[[ "$status" -eq 1 ]] || { echo "unexpected tampered status $status" >&2; exit 1; }
jq -e '.execution_audit.consistent == false and .execution_audit.run_count == 1 and
    ([.execution_audit.findings[] | select(.code == "missing_evaluator_evidence" and
        .requirement_id == "D22-6-3-MAY-159")] | length) == 1' \
    "$test_dir/tampered.json" >/dev/null
printf 'audit CLI draft 22 passed\n'
