#!/usr/bin/env bash
set -euo pipefail

# Usage: driven-moq-lite.sh [--dry-run] TRANSPORT RUNNER_BIN [SCENARIO...]
#   TRANSPORT  native_quic or webtransport (moql://HOST:PORT/moq?token=l1d or
#              https://HOST:PORT/moq?token=l1d: the runner builds the endpoint, the adapter dials it).
#   RUNNER_BIN the built moq-interop-runner. The audit CLI is the moq-interop-audit next to it
#              (MOQ_INTEROP_AUDIT_BIN overrides).
#   SCENARIO   moq-lite-06 scenario ids, each posted as its own driven run, in order; default every
#              executable moq-lite-06 scenario (the 27 of include/moq/interop/app/lite_scenarios.h).
#              Ids joined by commas (a,b,c) are posted as ONE run of those scenarios (a group run: one
#              context, and one publisher, per scenario).
#   --dry-run  print the plan instead of running it: the runner invocation (`runner:` line, the
#              environment and arguments the runner would be started with), each run request
#              (`request:` line, the POST /api/v1/runs body as compact JSON) and the audit (`audit:`
#              line). Paths that only exist during a run show as @TMP@. Nothing is started, no
#              certificate or temporary directory is created, and the binaries need not exist.
# The publisher is the moq CLI of moq-dev/moq (binary `moq`) named by MOQ_CLI_BIN, through the
# bundled adapters/moq-lite/run.sh, which also needs ffmpeg (or MOQ_FFMPEG_BIN). The track fixture
# posted with every run is the one the adapter accepts (adapters/moq-lite/adapter.json).
# MOQ_LITE_ADAPTER=moq-lite-ref runs the reference publisher instead (adapters/moq-lite-ref, MOQ_LITE_REF_BIN and the
# operator flags MOQ_LITE_REF_ARGS; no ffmpeg needed).
# Without MOQ_CLI_BIN, ffmpeg, jq, curl, openssl, timeout or the runner binary the script skips
# (exit 77).
# MOQ_INTEROP_TEST_HTTP_PORT and MOQ_INTEROP_TEST_UDP_PORT choose the ports (default 19235/19236).
# MOQ_INTEROP_TEST_KEEP=1 keeps the temporary directory (database, runner log, adapter logs) and
# prints its path on standard error.
#
# Output: one line per run (`scenario=... transport=... run=... verdict=... publisher=...`), one
# indented `row REQUIREMENT STATE` line per stored outcome, then `audit status=N` and the audit
# report of the run database (moq-interop-audit --draft moq-lite-06 --database: the static staged
# audit and the execution audit of the stored runs). The script exits 0 when every run finalized and
# the audit could read the database (audit status 0, or 1 for findings, which are reported), whatever
# the verdicts (a lite run is never Pass: the catalog is staged).
dry_run=0
if [[ "${1:-}" == --dry-run ]]; then
    dry_run=1
    shift
fi
if [[ $# -lt 2 ]]; then
    printf 'Usage: %s [--dry-run] TRANSPORT RUNNER_BIN [SCENARIO...]\n' "$0" >&2
    exit 2
fi
transport=$1
[[ "$transport" == native_quic || "$transport" == webtransport ]] || exit 2
realpath_args=()
if ((dry_run)); then realpath_args=(-m); fi
runner_bin=$(realpath "${realpath_args[@]}" "$2")
# The audit CLI next to the runner as named (a symlinked runner keeps its directory's audit CLI).
audit_bin=${MOQ_INTEROP_AUDIT_BIN:-$(realpath -m "$(dirname "$2")")/moq-interop-audit}
shift 2
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
all_scenarios=(
    l06-setup-stream l06-setup-unknown-parameter l06-setup-duplicate-parameter
    l06-setup-duplicate-stream l06-setup-server-path l06-setup-server-role l06-setup-client-path
    l06-announce-prefix l06-announce-lifecycle l06-session-stream-close l06-subscribe-latest
    l06-subscribe-refused l06-subscribe-invalid-frame-bounds l06-subscribe-group-floor
    l06-subscribe-abutting-frame-start l06-errors-unknown-stream-type l06-errors-unknown-reset-code
    l06-errors-reserved-reset-code l06-errors-code-space
    l06-track-info l06-fetch-group l06-fetch-unknown-group l06-probe-report
    l06-datagram-size l06-goaway-single l06-goaway-duplicate l06-goaway-oversize
)
if [[ $# -gt 0 ]]; then
    scenarios=("$@")
else
    scenarios=("${all_scenarios[@]}")
fi
for scenario in "${scenarios[@]}"; do
    IFS=, read -r -a group <<<"$scenario"
    [[ ${#group[@]} -gt 0 && "$scenario" != *, ]] || { printf 'empty moq-lite-06 scenario group: %s\n' "$scenario" >&2; exit 2; }
    for member in "${group[@]}"; do
        known=0
        for id in "${all_scenarios[@]}"; do [[ "$member" == "$id" ]] && known=1; done
        ((known)) || { printf 'unknown moq-lite-06 scenario: %s\n' "$member" >&2; exit 2; }
    done
done

adapter_name=${MOQ_LITE_ADAPTER:-moq-lite}
[[ "$adapter_name" == moq-lite || "$adapter_name" == moq-lite-ref ]] || { printf 'unknown MOQ_LITE_ADAPTER: %s\n' "$adapter_name" >&2; exit 2; }
if [[ "$adapter_name" == moq-lite-ref ]]; then publisher_var=MOQ_LITE_REF_BIN; else publisher_var=MOQ_CLI_BIN; fi
if ((!dry_run)); then
    missing=
    [[ -n "${!publisher_var:-}" && -x "${!publisher_var:-}" ]] || missing+=" $publisher_var"
    if [[ "$adapter_name" == moq-lite ]]; then
        if [[ -n "${MOQ_FFMPEG_BIN:-}" ]]; then
            [[ -x "$MOQ_FFMPEG_BIN" ]] || missing+=' ffmpeg'
        else
            command -v ffmpeg >/dev/null 2>&1 || missing+=' ffmpeg'
        fi
    fi
    for tool in jq curl openssl timeout; do command -v "$tool" >/dev/null 2>&1 || missing+=" $tool"; done
    [[ -x "$runner_bin" ]] || missing+=' runner'
    if [[ -n "$missing" ]]; then
        printf 'SKIP: missing%s\n' "$missing" >&2
        exit 77
    fi
fi
publisher_bin=$(realpath "${realpath_args[@]}" "${!publisher_var:-moq}")
adapter_json="$root_dir/adapters/$adapter_name/adapter.json"
namespace_hex=$(jq -c '.supported_namespace_hex' "$adapter_json")
track_name_hex=$(jq -c '.supported_track_name_hex' "$adapter_json")

if ((dry_run)); then
    test_dir=@TMP@
else
    test_dir=$(mktemp -d /tmp/moq-interop-driven.XXXXXX)
fi
http_port=${MOQ_INTEROP_TEST_HTTP_PORT:-19235}
udp_port=${MOQ_INTEROP_TEST_UDP_PORT:-19236}
runner_pid=
runner_args=(--bind 127.0.0.1 --port "$http_port"
    --database "$test_dir/runs.sqlite3"
    --docs "$root_dir/docs" --requirements "$root_dir/requirements"
    --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1
    --publisher-port-start "$udp_port" --publisher-port-end "$udp_port"
    --tls-cert "$test_dir/cert.pem" --tls-key "$test_dir/key.pem"
    --driver-executable "$root_dir/adapters/$adapter_name/run.sh"
    --driver-log-root "$test_dir/logs")
audit_args=(--draft moq-lite-06 --database "$test_dir/runs.sqlite3"
    --docs "$root_dir/docs" --requirements "$root_dir/requirements")

api_transport=$transport
if [[ "$transport" == native_quic ]]; then api_transport=native-quic; fi
# The timeout bounds the connection wait and each probe, and must exceed the longest probe's windows
# (l06-fetch-group: 2 x 3 s + 15 s + 3 s, refused at exactly 24000 ms).
request_for() {
    jq -cn --arg transport "$api_transport" --arg scenario "$1" \
        --argjson namespace "$namespace_hex" --argjson name "$track_name_hex" \
        '{draft: "moq-lite-06", transport: $transport, mode: "driven", scenarios: ($scenario | split(",")),
          timeout_ms: 30000, track: {namespace_hex: $namespace, name_hex: $name}}'
}

if ((dry_run)); then
    printf 'runner: %s=<%s> <%s>' "$publisher_var" "$publisher_bin" "$runner_bin"
    printf ' <%s>' "${runner_args[@]}"
    printf '\n'
    for scenario in "${scenarios[@]}"; do
        printf 'request: POST http://127.0.0.1:%s/api/v1/runs %s\n' "$http_port" "$(request_for "$scenario")"
    done
    printf 'audit: <%s>' "$audit_bin"
    printf ' <%s>' "${audit_args[@]}"
    printf '\n'
    exit 0
fi

stop_runner() {
    if [[ -n "$runner_pid" ]]; then
        kill "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
        runner_pid=
    fi
}
cleanup() {
    stop_runner
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
env "$publisher_var=$publisher_bin" "$runner_bin" "${runner_args[@]}" \
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

# Every stored event of run $1 as one {"items": [...]} object (the route pages by at most 100).
all_events() {
    local offset=0 pages="$test_dir/events-$1.json"
    : >"$pages"
    while :; do
        curl --fail --silent --show-error \
            "http://127.0.0.1:$http_port/api/v1/runs/$1/events?limit=100&offset=$offset" >>"$pages"
        offset=$(jq -rs '.[-1].pagination.next_offset // empty' "$pages")
        [[ -n "$offset" ]] || break
    done
    jq -cs '{items: (map(.items) | add)}' "$pages"
}

for scenario in "${scenarios[@]}"; do
    created=$(curl --fail-with-body --silent --show-error -X POST \
        "http://127.0.0.1:$http_port/api/v1/runs" \
        -H 'Content-Type: application/json' -d "$(request_for "$scenario")") ||
        { printf 'run %s refused: %s\n' "$scenario" "$created" >&2; exit 1; }
    run_id=$(jq -er '.run.id' <<<"$created")
    result=
    # A context waits up to the timeout for the publisher, then runs its probe up to the timeout:
    # 70 s per scenario of the run (a group run has one context per scenario).
    IFS=, read -r -a group <<<"$scenario"
    attempts=$((700 * ${#group[@]}))
    for ((attempt = 1; attempt <= attempts; attempt++)); do
        result=$(curl --fail --silent --show-error \
            "http://127.0.0.1:$http_port/api/v1/runs/$run_id")
        if [[ $(jq -r '.run.state' <<<"$result") == finalized ]]; then break; fi
        sleep 0.1
    done
    if [[ $(jq -r '.run.state' <<<"$result") != finalized ]]; then
        printf 'run did not finalize: %s\n' "$result" >&2
        exit 1
    fi
    events=$(all_events "$run_id")
    # A context the runner refused before starting the driver (harness_error, for example a probe
    # whose windows do not fit the timeout) has no driver evidence; it is reported below.
    if [[ $(jq '[.items[] | select(.kind == "harness_error")] | length' <<<"$events") -eq 0 &&
        ($(jq '[.items[] | select(.kind == "publisher_process")] | length' <<<"$events") -lt 1 ||
        ! -f "$test_dir/logs/$run_id/1-${scenario%%,*}/request.json") ]]; then
        printf 'missing driver evidence or retained contract for %s\n' "$scenario" >&2
        sed -n '1,120p' "$test_dir/runner.log" >&2
        exit 1
    fi
    count() { jq --arg state "$1" '[.run.outcomes[] | select(.state == $state)] | length' <<<"$result"; }
    printf 'scenario=%s transport=%s run=%s verdict=%s publisher=%s pass=%s fail=%s not_run=%s\n' \
        "$scenario" "$transport" "$run_id" \
        "$(jq -r '.run.verdict' <<<"$result")" \
        "$(jq -r '[.items[] | select(.kind == "publisher_process") | .detail | fromjson |
            (.status // .error // "none") | tostring] | join(",")' <<<"$events")" \
        "$(count pass)" "$(count fail)" "$(count not_run)"
    # The rows this run judged (a staged lite run lists every catalog row; the rest are not_run,
    # not_applicable or not_testable).
    jq -r '.run.outcomes[] | select(.state == "pass" or .state == "fail") |
        "  row \(.requirement_id) \(.state)"' <<<"$result"
    jq -r '.items[] | select(.kind == "harness_error" or .kind == "context_skipped") |
        "  \(.kind) \(.detail)"' <<<"$events"
done
stop_runner

# The audit of the run database: the static staged audit and the execution audit of the stored runs.
audit_status=0
"$audit_bin" "${audit_args[@]}" >"$test_dir/audit.txt" 2>&1 || audit_status=$?
printf 'audit status=%s\n' "$audit_status"
cat "$test_dir/audit.txt"
# 0: clean; 1: findings (reported above). Anything else means the audit could not run.
((audit_status == 0 || audit_status == 1)) || exit 1
