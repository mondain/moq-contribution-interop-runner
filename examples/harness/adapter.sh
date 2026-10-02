#!/usr/bin/env bash
# Example driver adapter for the fictional publisher "acme-pub".
#
# The runner executes this file once per scenario context. It reads the
# versioned JSON request named by MOQ_INTEROP_DRIVER_REQUEST_FILE, translates
# it into acme-pub's own command line and execs the publisher. It never decides
# what MoQT behavior is expected and never produces a score.
set -euo pipefail

fail() {
    printf 'acme adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
# ACME_PUB_BIN overrides the default: an executable named acme-pub next to this
# script, which is where a Docker Compose setup puts it (/opt/publisher).
publisher_bin=${ACME_PUB_BIN:-$(dirname "${BASH_SOURCE[0]}")/acme-pub}
[[ -x "$publisher_bin" ]] || fail "publisher is not executable: $publisher_bin"

# Reject anything this adapter cannot map instead of guessing.
jq -e '
    .schema_version == 1 and
    (.draft == 18 or .draft == 21) and
    (.transport == "native_quic" or .transport == "webtransport") and
    (.endpoint | type == "string" and length > 0) and
    (.fixture | type == "string" and length > 0) and
    (.tls_ca | type == "string" and length > 0) and
    (.scenario_timeout_ms | type == "number" and . >= 1) and
    (.namespace_hex | type == "array" and length >= 1) and
    (.track_name_hex | type == "string")
' "$request_file" >/dev/null || fail 'unsupported or malformed request'

draft=$(jq -r '.draft' "$request_file")
transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
fixture=$(jq -r '.fixture' "$request_file")
ca_cert=$(jq -r '.tls_ca' "$request_file")
scenario_id=$(jq -r '.scenario_id' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")

[[ -r "$fixture" ]] || fail 'fixture is unreadable'
[[ -r "$ca_cert" ]] || fail 'TLS CA is unreadable'

# The runner hands out moqt:// for native QUIC and https:// for WebTransport.
case "$transport" in
    native_quic)  [[ "$endpoint" == moqt://* ]] || fail 'native QUIC requires a moqt:// endpoint'
                  acme_transport=quic ;;
    webtransport) [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an https:// endpoint'
                  acme_transport=wt ;;
esac

# Namespace fields and track name arrive as lowercase hex of opaque bytes.
# acme-pub accepts printable names only, so refuse anything else.
hex_to_text() {
    local hex=$1 text= index
    # Printable ASCII only (0x21-0x7e). Checking the hex first also keeps a
    # NUL byte from vanishing inside the command substitution below.
    [[ "$hex" =~ ^(2[1-9a-f]|[3-6][0-9a-f]|7[0-9a-e])+$ ]] ||
        fail 'namespace or track name is not printable ASCII'
    for ((index = 0; index < ${#hex}; index += 2)); do
        text+=$(printf "\\x${hex:index:2}")
    done
    [[ "$text" =~ ^[A-Za-z0-9._-]+$ ]] || fail 'namespace or track name is not a plain name'
    printf '%s' "$text"
}
namespace=
while IFS= read -r field_hex; do
    namespace+="${namespace:+/}$(hex_to_text "$field_hex")"
done < <(jq -r '.namespace_hex[]' "$request_file")
track=$(hex_to_text "$(jq -r '.track_name_hex' "$request_file")")

# acme-pub counts whole seconds; stay alive until the runner's own deadline.
timeout_seconds=$(((timeout_ms + 999) / 1000))

# "serve" waits for the relay to subscribe, "announce" sends the publisher's own
# PUBLISH. The mode only makes acme-pub emit the messages a scenario observes;
# it never describes what the runner should expect.
mode=serve
case "$scenario_id" in
    publish-track-under-single-period-namespace|\
    application-publish-track-in-session-namespace|\
    publish-distinct-content-tracks-in-same-scope|\
    d21-publisher-request-stream-placement)
        mode=announce
        ;;
esac

args=(--input "$fixture" --connect "$endpoint" --transport "$acme_transport"
      --draft "$draft" --namespace "$namespace" --track "$track"
      --ca "$ca_cert" --exit-after "$timeout_seconds" --mode "$mode")

exec "$publisher_bin" "${args[@]}"
