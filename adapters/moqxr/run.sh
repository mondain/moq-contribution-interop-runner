#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'moqxr adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
publisher_bin=${MOQXR_BIN:-}
[[ -n "$publisher_bin" && -x "$publisher_bin" ]] ||
    fail 'MOQXR_BIN must name an executable publisher'

jq -e '
    .schema_version == 1 and
    (.draft == 18 or .draft == 21) and
    (.transport == "native_quic" or .transport == "webtransport") and
    (.run_id | type == "string" and length > 0) and
    (.scenario_id | type == "string" and length > 0) and
    (.endpoint | type == "string" and length > 0 and (contains("\n") | not)) and
    (.fixture | type == "string" and length > 0 and (contains("\n") | not)) and
    (.tls_ca | type == "string" and length > 0 and (contains("\n") | not)) and
    (.log_dir | type == "string" and length > 0) and
    (.scenario_timeout_ms | type == "number" and . == floor and . >= 1 and . <= 3600000) and
    (.process_timeout_ms | type == "number" and . == floor and . >= 1) and
    (.namespace_hex == ["6d65646961"]) and
    (.track_name_hex == "766964655f31")
' "$request_file" >/dev/null || fail 'unsupported or malformed request'

draft=$(jq -r '.draft' "$request_file")
transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
fixture=$(jq -r '.fixture' "$request_file")
ca_cert=$(jq -r '.tls_ca' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")
[[ -r "$fixture" ]] || fail 'fixture is unreadable'
[[ -r "$ca_cert" ]] || fail 'TLS CA is unreadable'
if [[ "$transport" == webtransport ]]; then
    [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an HTTPS endpoint'
    publisher_transport=webtransport
else
    [[ "$endpoint" == moqt://* ]] || fail 'native QUIC requires a moqt endpoint'
    publisher_transport=raw
fi
timeout_seconds=$(((timeout_ms + 999) / 1000))
((timeout_seconds > 0)) || fail 'invalid scenario timeout'

# These options describe moqxr's CLI, not MOQT conformance expectations.
args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
      --namespace media --draft "$draft" --forward 0
      --timeout "$timeout_seconds" --ca "$ca_cert")
if [[ "$draft" == 21 && "$transport" == native_quic ]]; then
    args+=(--preannounce-tracks)
elif [[ "$draft" == 21 && "$transport" == webtransport ]]; then
    args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
          --namespace media --draft "$draft" --forward 1
          --timeout "$timeout_seconds" --ca "$ca_cert")
fi
exec "$publisher_bin" "${args[@]}"
