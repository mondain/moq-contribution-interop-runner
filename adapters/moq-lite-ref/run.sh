#!/usr/bin/env bash
# Driver adapter for the moq-lite-06 reference publisher of this repository (moq-interop-lite-ref-publisher, L2c). It
# validates the runner's request exactly as adapters/moq-lite/run.sh does and runs
#
#   moq-interop-lite-ref-publisher --connect ENDPOINT [MOQ_LITE_REF_ARGS...]
#
# The endpoint is passed unchanged. MOQ_LITE_REF_ARGS (word-split) are the operator's flags (--defect NAME,
# --datagrams, --probe-level, --frames-per-group); they are never taken from the request, so the runner does not decide
# what the publisher does. Every one of the 27 executable scenarios gets the same command line unless the operator
# sets the flags (tests/golden/moq-lite-ref-cmdlines.txt).
set -euo pipefail

readonly fixture_namespace_hex='["696e7465726f702e68616e67"]'
readonly fixture_track_name_hex='302e6d3473'

fail() {
    printf 'moq-lite-ref adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -f "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
ref_bin=${MOQ_LITE_REF_BIN:-}
[[ -n "$ref_bin" && -f "$ref_bin" && -x "$ref_bin" ]] ||
    fail 'MOQ_LITE_REF_BIN must name the executable moq-interop-lite-ref-publisher'
read -r -a extra_args <<<"${MOQ_LITE_REF_ARGS:-}"
command -v timeout >/dev/null || fail 'coreutils timeout is required'
for name in $(compgen -e); do
    if [[ "$name" == MOQ_* ]]; then
        unset "$name"
    fi
done

jq -en '[inputs] | length == 1 and (.[0] | type == "object")' "$request_file" >/dev/null 2>&1 ||
    fail 'request is not a JSON object'
case "$(jq -r '.draft | type' "$request_file")" in
    number) fail "draft $(jq -r '.draft' "$request_file") is not supported (supported drafts: moq-lite-06)" ;;
    string)
        jq -e '.draft == "moq-lite-06"' "$request_file" >/dev/null ||
            fail "draft $(jq -c '.draft' "$request_file") is not supported (supported drafts: moq-lite-06)" ;;
    *) fail 'draft is missing or not a string or number' ;;
esac
require() {
    jq -e --argjson namespace "$fixture_namespace_hex" --arg track "$fixture_track_name_hex" \
        "$1" "$request_file" >/dev/null 2>&1 || fail "$2"
}
plain='type == "string" and (test("[[:cntrl:]]") | not)'
require '.schema_version == 1' 'schema_version must be 1'
require '.transport == "native_quic" or .transport == "webtransport"' \
    'transport must be native_quic or webtransport'
require ".run_id | $plain and length > 0" 'run_id must be a non-empty single-line string'
require ".scenario_id | $plain and length > 0" 'scenario_id must be a non-empty single-line string'
require ".endpoint | $plain and length > 0" 'endpoint must be a non-empty single-line string'
require ".fixture | $plain" 'fixture must be a single-line string'
require ".tls_ca | $plain" 'tls_ca must be a single-line string'
require ".log_dir | $plain and length > 0" 'log_dir must be a non-empty single-line string'
require '.scenario_timeout_ms | type == "number" and (tostring | test("^[0-9]+$")) and . >= 1 and . <= 3600000' \
    'scenario_timeout_ms must be a whole number of milliseconds from 1 to 3600000'
require '.process_timeout_ms | type == "number" and (tostring | test("^[0-9]+$")) and . >= 1' \
    'process_timeout_ms must be a whole number of milliseconds of at least 1'
require '.namespace_hex == $namespace' "namespace_hex must be $fixture_namespace_hex (interop.hang)"
require '.track_name_hex == $track' "track_name_hex must be $fixture_track_name_hex (0.m4s)"

transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
log_dir=$(jq -r '.log_dir' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")
if [[ "$transport" == webtransport ]]; then
    [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an https:// endpoint'
else
    [[ "$endpoint" == moql://* ]] || fail 'native QUIC requires a moql:// endpoint'
fi
require '.endpoint | test("^[!-~]+$")' 'endpoint contains whitespace, control or non-ASCII characters'
[[ -d "$log_dir" && -w "$log_dir" ]] || fail 'log_dir is not a writable directory'

# The publisher ends when the runner closes the session; the timeout is a backstop 3 s past the scenario's.
timeout_seconds=$(((timeout_ms + 999) / 1000))
((timeout_seconds > 0)) || fail 'invalid scenario timeout'
supervise=(timeout --foreground --preserve-status -k 2 -s TERM "$((timeout_seconds + 3))")
printf 'moq-lite-ref adapter: publisher %s defect/flags: %s\n' "$ref_bin" "${extra_args[*]:-none}" >&2

# Supervisor: as adapters/moq-lite/run.sh. The runner stops the group with SIGTERM; this exits 0 at once.
trap 'exit 0' TERM INT HUP
publish() {
    set +e
    "${supervise[@]}" "$ref_bin" --connect "$endpoint" "${extra_args[@]}" >"$log_dir/publisher.log" 2>"$log_dir/publisher.err"
    exit $?
}
publish &
pipeline_pid=$!
status=0
wait "$pipeline_pid" || status=$?
exit "$status"
