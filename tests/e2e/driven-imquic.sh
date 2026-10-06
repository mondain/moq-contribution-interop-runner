#!/usr/bin/env bash
set -euo pipefail

# Usage: driven-imquic.sh [--dry-run] DRAFT TRANSPORT RUNNER_BIN IMQUIC_PUB_BIN MP4_FIXTURE
#   DRAFT      22 only (the imquic adapter supports draft 22): the scenario is
#              d22-publisher-request-stream-placement over moqt-22 (native QUIC) or h3 (WebTransport).
#   TRANSPORT  native_quic or webtransport.
#   --dry-run  print the plan instead of running it: the runner invocation (`runner:` line, the
#              environment and arguments the runner would be started with) and the run request
#              (`request:` line, the POST /api/v1/runs body as compact JSON). Paths that only exist
#              during a run show as @TMP@. Nothing is started, no certificate or temporary directory
#              is created, and the binaries need not exist. Off by default.
# MOQ_INTEROP_TEST_HTTP_PORT and MOQ_INTEROP_TEST_UDP_PORT choose the ports (default 19221/19222).
dry_run=0
if [[ "${1:-}" == --dry-run ]]; then
    dry_run=1
    shift
fi
if [[ $# -ne 5 ]]; then
    printf 'Usage: %s DRAFT TRANSPORT RUNNER_BIN IMQUIC_PUB_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
draft=$1
transport=$2
realpath_args=()
if ((dry_run)); then realpath_args=(-m); fi
runner_bin=$(realpath "${realpath_args[@]}" "$3")
publisher_bin=$(realpath "${realpath_args[@]}" "$4")
fixture=$(realpath "${realpath_args[@]}" "$5")
[[ "$draft" == 22 ]] || exit 2
[[ "$transport" == native_quic || "$transport" == webtransport ]] || exit 2
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if ((dry_run)); then
    test_dir=@TMP@
else
    test_dir=$(mktemp -d /tmp/moq-interop-driven.XXXXXX)
fi
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19221}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19222}
runner_pid=
runner_args=(--bind 127.0.0.1 --port "$http_port"
    --database "$test_dir/runs.sqlite3"
    --docs "$root_dir/docs" --requirements "$root_dir/requirements"
    --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port"
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem"
    --driver-executable "$root_dir/adapters/imquic/run.sh"
    --driver-fixture "$fixture" --driver-log-root "$test_dir/logs")

scenario=d22-publisher-request-stream-placement
api_transport=$transport
if [[ "$transport" == native_quic ]]; then api_transport=native-quic; fi
request=$(jq -n --argjson draft "$draft" --arg transport "$api_transport" \
    --arg scenario "$scenario" '{draft: $draft, transport: $transport,
    mode: "driven", scenarios: [$scenario], timeout_ms: 12000,
    track: {namespace_hex: ["6d65646961"], name_hex: "766964655f31"}}')

if ((dry_run)); then
    printf 'runner: IMQUIC_PUB_BIN=<%s> <%s>' "$publisher_bin" "$runner_bin"
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
    case "$test_dir" in
        /tmp/moq-interop-driven.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$test_dir/key.pem" -out "$test_dir/cert.pem" \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -days 1 >/dev/null 2>&1
IMQUIC_PUB_BIN="$publisher_bin" "$runner_bin" "${runner_args[@]}" \
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
