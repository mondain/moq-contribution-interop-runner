#!/usr/bin/env bash
# Driver adapter for imquic's example publisher (examples/moq-pub.c, built as imquic-moq-pub) at MoQ
# draft 22. It validates the runner's request, translates it into moq-pub options and execs the
# publisher under `timeout`, with its output in <log_dir>/publisher.log. It describes imquic's command
# line only; it never decides what the runner expects.
#
# moq-pub's command line (GLib options, examples/moq-pub-options.c):
#   -M 22        negotiate MoQT draft 22 only (moqt-22 ALPN / WebTransport protocol)
#   -n NAME      one namespace field (repeatable); -N NAME the track name. Plain C strings: the
#                adapter accepts only the reference fixture (media / vide_1), so decoding is exact.
#   -r HOST -R PORT  the server; HOST goes to getaddrinfo, so a name, IPv4 or bare IPv6 literal works.
#                SNI defaults to HOST (-S is not needed) and TLS is never verified, so tls_ca is unused.
#   -q / -w      raw QUIC / WebTransport; -H PATH is the HTTP/3 :path (verbatim, must start with "/",
#                default "/"). The raw QUIC client sends no PATH or AUTHORITY SETUP option, so a moqt
#                path cannot be conveyed (it is dropped) and a moqt query is refused.
#   -X           publish-first: PUBLISH right after SETUP, Objects only after PUBLISH_OK; it sends no
#                PUBLISH_NAMESPACE and answers every SUBSCRIBE with REQUEST_ERROR DUPLICATE_SUBSCRIPTION.
#                Without -X (announce-and-wait): PUBLISH_NAMESPACE right after SETUP, then the first
#                SUBSCRIBE for the track is accepted (one subscriber at a time) and Objects flow only
#                if that SUBSCRIBE carries FORWARD=1. A refused PUBLISH or PUBLISH_NAMESPACE ends it.
#   -D datagram  send Objects as datagrams instead of subgroup streams.
#   -d 4         log level (0-7, 4 = info), to standard output.
# It has no timeout or input option (the payload is a clock: one Object per second, one Group per
# minute) and stops on SIGTERM (PUBLISH_DONE, PUBLISH_NAMESPACE_DONE, exit 0), on connection loss or
# GOAWAY. A third signal makes it exit(1) at once.
set -euo pipefail

fail() {
    printf 'imquic adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
publisher_bin=${IMQUIC_PUB_BIN:-}
[[ -n "$publisher_bin" && -f "$publisher_bin" && -x "$publisher_bin" ]] ||
    fail 'IMQUIC_PUB_BIN must name an executable imquic moq-pub publisher'
command -v timeout >/dev/null || fail 'coreutils timeout is required'

malformed='unsupported or malformed request (supported drafts: 22)'
draft=$(jq -er '.draft | numbers' "$request_file" 2>/dev/null) || fail "$malformed"
[[ "$draft" == 22 ]] || fail "draft $draft is not supported (supported drafts: 22)"
jq -e '
    .schema_version == 1 and
    .draft == 22 and
    (.transport == "native_quic" or .transport == "webtransport") and
    (.run_id | type == "string" and length > 0) and
    (.scenario_id | type == "string" and length > 0 and (contains("\n") | not)) and
    (.endpoint | type == "string" and length > 0 and (contains("\n") | not)) and
    (.fixture | type == "string" and (contains("\n") | not)) and
    (.tls_ca | type == "string" and (contains("\n") | not)) and
    (.log_dir | type == "string" and length > 0 and (contains("\n") | not)) and
    (.scenario_timeout_ms | type == "number" and . == floor and . >= 1 and . <= 3600000) and
    (.process_timeout_ms | type == "number" and . == floor and . >= 1) and
    (.namespace_hex == ["6d65646961"]) and
    (.track_name_hex == "766964655f31")
' "$request_file" >/dev/null 2>&1 || fail "$malformed"

transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
log_dir=$(jq -r '.log_dir' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")
scenario_id=$(jq -r '.scenario_id' "$request_file")
[[ -d "$log_dir" && -w "$log_dir" ]] || fail 'log_dir is not a writable directory'
# imquic reads no fixture (it publishes a clock) and verifies no certificate (tls_ca): both unused.

# Opaque hex names to the plain strings moq-pub takes: printable ASCII without spaces only.
decode_name() {
    local hex=$1 out= index
    [[ "$hex" =~ ^([0-9a-f]{2})+$ ]] || return 1
    for ((index = 0; index < ${#hex}; index += 2)); do
        local byte=$((16#${hex:index:2}))
        ((byte >= 0x21 && byte <= 0x7e)) || return 1
        out+=$(printf "\\x${hex:index:2}")
    done
    printf '%s' "$out"
}
namespace_args=()
while IFS= read -r field_hex; do
    field=$(decode_name "$field_hex") || fail 'namespace fields must be printable ASCII'
    namespace_args+=(-n "$field")
done < <(jq -r '.namespace_hex[]' "$request_file")
track_name=$(decode_name "$(jq -r '.track_name_hex' "$request_file")") ||
    fail 'the track name must be printable ASCII'

# The endpoint: scheme://authority[/path][?query]. imquic takes the host and port separately and,
# on WebTransport only, the HTTP/3 path; anything it cannot express is refused.
if [[ "$transport" == webtransport ]]; then
    [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an https:// endpoint'
else
    [[ "$endpoint" == moqt://* ]] || fail 'native QUIC requires a moqt:// endpoint'
fi
[[ ! "$endpoint" =~ [[:space:][:cntrl:]] ]] || fail 'endpoint contains whitespace or control characters'
[[ "$endpoint" != *'#'* ]] || fail 'endpoint has a fragment, which imquic cannot express'
endpoint_pattern='^(moqt|https)://([^/?]*)(/[^?]*)?(\?.*)?$'
[[ "$endpoint" =~ $endpoint_pattern ]] || fail 'endpoint is not a moqt:// or https:// URI'
authority=${BASH_REMATCH[2]}
path=${BASH_REMATCH[3]}
query=${BASH_REMATCH[4]}
[[ "$authority" != *'@'* ]] || fail 'endpoint has user information, which imquic cannot express'
ipv6_pattern='^\[([0-9A-Fa-f:.]*:[0-9A-Fa-f:.]*)\]:([0-9]+)$'
name_pattern='^([A-Za-z0-9._~][A-Za-z0-9._~-]*):([0-9]+)$'
if [[ "$authority" =~ $ipv6_pattern || "$authority" =~ $name_pattern ]]; then
    host=${BASH_REMATCH[1]}
    port=${BASH_REMATCH[2]}
else
    fail 'endpoint needs a host (name, IPv4 or bracketed IPv6 without zone) and a port that imquic can use'
fi
((${#port} <= 5 && 10#$port >= 1 && 10#$port <= 65535)) || fail 'endpoint port is out of range'
port=$((10#$port))
if [[ "$transport" == webtransport ]]; then
    # moq-pub sends -H unchanged as the :path, query included.
    connection_args=(-w -H "${path:-/}$query")
else
    [[ -z "$query" ]] ||
        fail "native QUIC endpoint has a query; imquic's raw QUIC client sends no PATH setup option to carry it"
    connection_args=(-q)
fi

# moq-pub has no deadline of its own: `timeout` stops it at the scenario timeout (rounded up to whole
# seconds) plus 3 seconds, so the runner, not the publisher, ends the context, as with moqxr's paced
# runs. --foreground keeps `timeout` from signalling the whole process group as well: the runner
# already does, and a third SIGTERM would make moq-pub exit(1) without cleanup. --preserve-status
# reports moq-pub's own status (0 after SIGTERM) instead of 124; -k 2 kills it if it hangs.
timeout_seconds=$(((timeout_ms + 999) / 1000))
((timeout_seconds > 0)) || fail 'invalid scenario timeout'
publisher_timeout=$((timeout_seconds + 3))

# ---------------------------------------------------------------------------------------------------
# Per-scenario mode: publish-first (-X) or announce-and-wait (no -X). These choose moq-pub options so
# that it emits the messages a scenario observes; they never change what a scenario expects.
#
# Shared draft 22 scenarios (d22-X, paired with d21-X in the lineage table) are looked up by their
# d21- id, impl_id, the one normalization point below. Their mode is derived from the moqxr adapter
# (adapters/moqxr/run.sh), whose `--forward` choice was made per scenario and checked in its sweeps:
#   moqxr --forward 1 (moqxr sends its own PUBLISH first)              -> -X
#   moqxr --forward 0, with or without --paced (it announces and waits) -> no -X
# The assumption is that the two publish-first modes are interchangeable for the runner. They are
# not where the runner subscribes: moqxr blocks on its unanswered PUBLISH and serves nothing, imquic
# with -X refuses the SUBSCRIBE at once (DUPLICATE_SUBSCRIPTION) and imquic without -X has no PUBLISH
# at all. tests/e2e/imquic-adapter-cmdlines.sh checks the derivation against moqxr's draft 22 golden
# and lists the only exceptions, the two d22- keyed lists below.
#
# The fallback, as moqxr's draft 21/22 default --forward 1, is publish-first.
# moqxr's --forward 0 lists (both its await-subscribe and its paced lists), copied unchanged:
announce_wait_impl_ids() {
    case "$1" in
        d21-overlapping-subscriptions-*|d21-forward-location-and-range-filter-conjunction|\
        d21-fill-fails-before-first-object|\
        d21-cancel-subscription-with-concurrent-fill-streams|\
        d21-subscribe-tracks-publish-skipped-then-capacity-recovers|\
        d21-subgroup-completion-withheld-acknowledgments|d21-request-well-formed-invalid-token|\
        d21-expired-token-alias-lifetime) return 0 ;;
        d21-largest-object-* | d21-publish-done-* | d21-publisher-namespace-redirect | \
        d21-publisher-subscribe-tracks-redirect | d21-publish-state-notify-* | d21-padding-*-emission | \
        d21-namespace-discovery-authorization | d21-track-discovery-* | d21-subgroup-early-handoff-reset | \
        d21-filter-* | d21-grease-auth-token-type | d21-grease-stop-sending | d21-grease-setup-options | \
        d21-publisher-goaway-alternate-uri | \
        d21-cancel-fetch-with-open-request-and-data-streams | \
        d21-cancel-subscribe-with-open-streams | d21-coalesced-failed-update-response | \
        d21-coalesced-successful-update-responses | \
        d21-discovery-independent-overlap-spaces | \
        d21-discovery-update-independent-overlap-spaces | \
        d21-discovery-update-invalid-forward | d21-duplicate-range-filter-key-in-request | \
        d21-duplicate-range-filter-key-in-update | d21-duplicate-request-goaway | \
        d21-duplicate-request-update-id | d21-established-subscription-publisher-fin | \
        d21-failed-fetch-update-data-reset | d21-failed-subscribe-namespace-update-close | \
        d21-failed-subscribe-tracks-update-close | d21-failed-subscription-update-cleanup | \
        d21-fetch-datagram-preference | d21-goaway-on-distinct-request-streams | \
        d21-group-order-in-subscription-update | d21-immutable-property-repeat | \
        d21-namespace-prefix-update-overlap | d21-object-property-filter-odd-property-type | \
        d21-prior-group-gap-repeat | d21-priority-filter-end-above-255 | \
        d21-priority-filter-start-above-255 | d21-prior-object-gap-repeat | \
        d21-range-filter-end-delta-overflow | d21-range-filter-start-delta-overflow | \
        d21-range-filter-total-exceeds-negotiated-limit | d21-range-filter-total-limit | \
        d21-range-filter-update-total-limit | d21-register-token-on-other-request-error | \
        d21-register-token-on-unauthorized-request | d21-repeat-object-retrieval | \
        d21-request-deleted-token-alias | d21-request-stream-terminal-message-order | \
        d21-request-update-unlimited | d21-setup-register-use-value-fallback | \
        d21-single-request-update-response | d21-subgroup-premature-close-reset | \
        d21-subgroup-restart-after-reset | d21-subscribe-namespace-overlap | \
        d21-subscribe-tracks-overlap | d21-subscription-forwarding-preference | \
        d21-successful-subscribe-forward-zero | d21-token-delete-and-reuse | \
        d21-token-duplicate-registration | d21-token-register-alias-lifetime | \
        d21-track-prefix-update-overlap | d21-track-property-filter-odd-property-type | \
        d21-update-subscription-location-range | \
        d21-control-stream-lifetime | \
        d21-duplicate-control-goaway | \
        d21-duplicate-request-id-across-streams | \
        d21-fill-timeout-outside-fill-or-fetch | \
        d21-forward-value-255 | \
        d21-forward-value-two | \
        d21-goaway-uri-length-boundary | \
        d21-group-order-above-two | \
        d21-group-order-zero | \
        d21-inbound-padding-datagram | \
        d21-inbound-padding-stream | \
        d21-include-properties-value-255 | \
        d21-include-properties-value-two | \
        d21-invalid-bidirectional-request-stream-opener | \
        d21-location-filter-end-group-overflow | \
        d21-message-body-length-mismatch | \
        d21-parameter-invalid-message-scope | \
        d21-parameter-type-delta-overflow | \
        d21-publish-established-subscriber-sends-publish-state-notify | \
        d21-publish-update-ok-with-track-properties | \
        d21-publisher-request-response-before-fin | \
        d21-range-filter-default-zero-limit | \
        d21-range-filter-with-zero-negotiated-limit | \
        d21-request-id-wrong-sender-parity | \
        d21-request-message-truncated-at-fin | \
        d21-request-unknown-token-alias | \
        d21-responder-update-on-publish-namespace | \
        d21-setup-known-key-value-malformed-value | \
        d21-subgroup-header-flags | \
        d21-subscribe-parameters-preserve-payload | \
        d21-subscribe-tracks-prefix-too-many-fields | \
        d21-subscriber-sends-publish-state-notify | \
        d21-successful-subscribe-response | \
        d21-unexpected-duplicate-message-parameter | \
        d21-unknown-message-parameter | \
        d21-update-on-track-status | \
        d21-request-single-period-namespace | \
        d21-session-namespace-empty-track-request | \
        d21-session-namespace-unknown-track-request | \
        d21-session-namespace-unknown-namespace-request | \
        d21-publish-namespace-ok-with-track-properties) return 0 ;;
    esac
    return 1
}

# moqxr's draft 22 paced overrides (d22_paced_overrides in adapters/moqxr/run.sh), copied unchanged:
# moqxr runs these --forward 0 --paced at draft 22 only, while their d21- twins keep --forward 1. They
# are probes in which the runner is the subscriber, so they announce and wait here too.
d22_moqxr_paced_overrides=(
    d22-subscribe-empty-namespace-field
    d22-subscribe-33-namespace-fields
    d22-subscribe-tracks-oversized-namespace
    d22-subscribe-oversized-full-track-name
    d22-request-undecodable-authorization-token
    d22-request-token-cache-overflow
    d22-request-alias-registration-with-default-zero-cache
    d22-fill-forbidden-nested-authorization
    d22-fill-forbidden-track-property-filter
    d22-fill-recursive-parameter
    d22-fill-invalid-group-order
    d22-unknown-unidirectional-stream-type
    d22-unknown-control-message
    d22-successful-subscribe-object-delivery
)

# imquic's exceptions to the derivation, decided from each scenario's source (draft 21 builder named):
#
#   d22- id (moqxr --forward 1 -> here no -X)       why the runner needs imquic to announce and wait
#   complete-subgroup-fin                          SUBSCRIBE FORWARD=1, judges Subgroup FIN (fin_spec)
#   subgroup-start-location-fin                    same, with a start filter (fin_spec)
#   object-datagram-flags                          SUBSCRIBE FORWARD=1, judges datagrams (+ -D datagram)
#   original-publisher-opens-new-subgroup          SUBSCRIBE, judges Objects (draft21_gap_a)
#   publish-track-with-mandatory-property          SUBSCRIBE, judges Objects (draft21_gap_a)
#   subscribe-single-subgroup                      SUBSCRIBE, judges Subgroups (draft21_gap_a)
#   subscribe-accepted                             SUBSCRIBE, scores the SUBSCRIBE_OK branch; with -X
#                                                  imquic always refuses (the -rejected twin keeps -X)
#   request-update-overrun                         SUBSCRIBE, then REQUEST_UPDATEs on it (session)
#   request-update-independent-streams             two SUBSCRIBEs, then REQUEST_UPDATEs (session)
#   publish-namespace-redirect-nonempty-track-name answers the publisher's PUBLISH_NAMESPACE (peer_close)
#   publisher-namespace-routing-announcement       needs an explicit PUBLISH_NAMESPACE (announcement)
#
# Every other moqxr --forward 1 id stays publish-first: the runner answers or refuses the publisher's
# PUBLISH (accepting_publishes, rejected_publish, peer_close publish probes, the typed announcement
# and SETUP scenarios), or the stimulus does not depend on the mode (FETCH-only probes, which
# moq-pub cannot serve in either mode; SETUP and transport probes; SUBSCRIBE_NAMESPACE probes).
d22_announce_overrides=(
    d22-complete-subgroup-fin
    d22-subgroup-start-location-fin
    d22-object-datagram-flags
    d22-original-publisher-opens-new-subgroup
    d22-publish-track-with-mandatory-property
    d22-subscribe-single-subgroup
    d22-subscribe-accepted
    d22-request-update-overrun
    d22-request-update-independent-streams
    d22-publish-namespace-redirect-nonempty-track-name
    d22-publisher-namespace-routing-announcement
)
#   d22- id (moqxr --forward 0 -> here -X)          why
#   publish-update-ok-with-track-properties        its first write answers the publisher's PUBLISH
#                                                  (peer_request_ready waits for PUBLISH Request ID 0)
d22_publish_overrides=(
    d22-publish-update-ok-with-track-properties
)

# The own draft 22 scenarios and the two unscored probes have no d21- twin; they are matched by their
# d22- id (closest draft 21 scenario in brackets), with the moqxr adapter's decisions:
#
#   publisher-location-filter-parameter  -X  answers the publisher's PUBLISH (courtesy Accept) and judges
#                                            LOCATION_FILTERs on it [d21-publisher-parameter-serialization]
#   every other one                      no -X  the runner subscribes (SUBSCRIBE, REQUEST_UPDATE, FETCH,
#                                            SUBSCRIBE_NAMESPACE) and judges the replies and Objects:
#     subscribe-bounded-location-range       [d21-update-subscription-location-range]
#     update-subscription-location-range     [d21-update-subscription-location-range]
#     fetch-bounded-location-range           [d21-fetch-datagram-preference]
#     discover-original-publisher-namespaces [d21-namespace-discovery-authorization]
#     request-stream-before-peer-setup       [d21-successful-subscribe-response]
#     location-filter-end-group-overflow     [d21-location-filter-end-group-overflow]
#     fill-location-filter-end-group-overflow [d21-location-filter-end-group-overflow]
#     location-filter-unknown-type (probe)   [d21-location-filter-end-group-overflow]
#     location-filter-absolute-origin (probe) [d21-successful-subscribe-response]
own22_publish_first=(
    d22-publisher-location-filter-parameter
)
own22_announce=(
    d22-subscribe-bounded-location-range
    d22-update-subscription-location-range
    d22-fetch-bounded-location-range
    d22-discover-original-publisher-namespaces
    d22-request-stream-before-peer-setup
    d22-location-filter-end-group-overflow
    d22-fill-location-filter-end-group-overflow
    d22-location-filter-unknown-type
    d22-location-filter-absolute-origin
)

listed() {
    local wanted=$1 entry
    shift
    for entry in "$@"; do
        [[ "$entry" == "$wanted" ]] && return 0
    done
    return 1
}

impl_id=$scenario_id
[[ "$scenario_id" == d22-* ]] && impl_id="d21-${scenario_id#d22-}"
publish_first=1
announce_wait_impl_ids "$impl_id" && publish_first=0
if listed "$scenario_id" "${d22_moqxr_paced_overrides[@]}" "${d22_announce_overrides[@]}"; then
    publish_first=0
elif listed "$scenario_id" "${d22_publish_overrides[@]}"; then
    publish_first=1
fi
if listed "$scenario_id" "${own22_publish_first[@]}"; then
    publish_first=1
elif listed "$scenario_id" "${own22_announce[@]}"; then
    publish_first=0
fi

args=(-M 22 "${namespace_args[@]}" -N "$track_name" -r "$host" -R "$port" "${connection_args[@]}")
((publish_first)) && args+=(-X)
# The one emission option: the datagram-flags probe judges Object datagrams, which moq-pub sends only
# with -D datagram.
case "$impl_id" in
    d21-object-datagram-flags) args+=(-D datagram) ;;
esac
args+=(-d 4)

exec timeout --foreground --preserve-status -k 2 -s TERM "$publisher_timeout" \
    "$publisher_bin" "${args[@]}" >"$log_dir/publisher.log" 2>&1
