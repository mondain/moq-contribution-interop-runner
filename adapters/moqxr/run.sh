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
    (.draft == 18 or .draft == 21 or .draft == 22) and
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
' "$request_file" >/dev/null || fail 'unsupported or malformed request (supported drafts: 18, 21, 22)'

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

scenario_id=$(jq -r '.scenario_id' "$request_file")
# Draft 22 runs moqxr with --draft 22 (typed LOCATION_FILTER, draft 21 publishing semantics) and
# otherwise exactly as draft 21. A shared draft 22 scenario is the draft 21 scenario of the same name
# under a d22- prefix (the lineage table pairs each d22-X with d21-X), so its d21- id, impl_id, selects
# the draft 21 options below unchanged. The own draft 22 scenarios and the unscored probes have no
# d21- twin: they are matched by their d22- id first and get the explicit options in this table
# (decided from what each raw probe asks of the publisher; the closest draft 21 scenario is named):
#
#   d22- id                                  options                         closest d21 scenario
#   subscribe-bounded-location-range         --forward 0 --paced, timeout+3  d21-update-subscription-location-range (*)
#   update-subscription-location-range       --forward 0 --paced, timeout+3  d21-update-subscription-location-range
#   fetch-bounded-location-range             --forward 0 --paced, timeout+3  d21-fetch-datagram-preference
#   discover-original-publisher-namespaces   --forward 0 --paced, timeout+3  d21-namespace-discovery-authorization (*)
#   publisher-location-filter-parameter      --forward 1                     d21-publisher-parameter-serialization
#   request-stream-before-peer-setup         --forward 0 --paced, timeout+3  d21-successful-subscribe-response
#   location-filter-end-group-overflow       --forward 0 --paced, timeout+3  d21-location-filter-end-group-overflow
#   fill-location-filter-end-group-overflow  --forward 0 --paced, timeout+3  d21-location-filter-end-group-overflow (*)
#   location-filter-unknown-type (probe)     --forward 0 --paced, timeout+3  d21-location-filter-end-group-overflow
#   location-filter-absolute-origin (probe)  --forward 0 --paced, timeout+3  d21-successful-subscribe-response
#
# (*) The d21- namesake runs with --forward 1; these draft 22 probes deliberately do not copy that.
# At drafts 21 and 22, --forward 1 makes moqxr send its own PUBLISH and block until it is answered,
# giving up after about 2 seconds; while blocked it serves none of the runner's requests. So every
# probe in which the runner is the subscriber (SUBSCRIBE, REQUEST_UPDATE, FETCH, SUBSCRIBE_NAMESPACE)
# and does not answer that PUBLISH gets --forward 0 --paced: moqxr waits for the runner's requests,
# keeps Subgroup streams open, and outlives the context so the runner ends it. Only
# publisher-location-filter-parameter keeps --forward 1: it answers moqxr's PUBLISH (courtesy Accept)
# and judges the LOCATION_FILTER moqxr sends on it. This chooses moqxr CLI options; it never changes
# what a scenario expects.
impl_id=$scenario_id
own22=
if [[ "$draft" == 22 ]]; then
    case "$scenario_id" in
        d22-publisher-location-filter-parameter)
            own22=push ;;
        d22-subscribe-bounded-location-range|d22-update-subscription-location-range|\
        d22-fetch-bounded-location-range|d22-discover-original-publisher-namespaces|\
        d22-request-stream-before-peer-setup|d22-location-filter-end-group-overflow|\
        d22-fill-location-filter-end-group-overflow|d22-location-filter-unknown-type|\
        d22-location-filter-absolute-origin)
            own22=await-paced ;;
        d22-*) impl_id="d21-${scenario_id#d22-}" ;;
    esac
fi
# These options describe moqxr's CLI, not MOQT conformance expectations.
args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
      --namespace media --draft "$draft" --forward 0
      --timeout "$timeout_seconds" --ca "$ca_cert")
if [[ "$draft" == 21 || "$draft" == 22 ]]; then
    # Scenarios in which the runner subscribes to the track (rather than observing
    # the publisher's own PUBLISH) need moqxr to wait for that SUBSCRIBE: --forward 0
    # selects its await-subscribe mode, whereas --forward 1 pushes PUBLISH requests
    # the runner does not answer in those contexts.
    forward=1
    case "$impl_id" in
        d21-overlapping-subscriptions-*|d21-forward-location-and-range-filter-conjunction|\
        d21-fill-fails-before-first-object|\
        d21-cancel-subscription-with-concurrent-fill-streams|\
        d21-subscribe-tracks-publish-skipped-then-capacity-recovers|\
        d21-subgroup-completion-withheld-acknowledgments|d21-request-well-formed-invalid-token|\
        d21-expired-token-alias-lifetime) forward=0 ;;
    esac
    args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
          --namespace media --draft "$draft" --forward "$forward"
          --timeout "$timeout_seconds" --ca "$ca_cert")
fi
# Scenario-specific CLI options. These only make moqxr emit the publisher-
# initiated messages a scenario observes; they never describe expectations.
case "$impl_id" in
    # moqxr only originates PUBLISH for the catalog track, and only on request.
    publish-track-under-single-period-namespace|\
    application-publish-track-in-session-namespace|\
    publish-distinct-content-tracks-in-same-scope)
        args+=(--publish-catalog)
        ;;
esac
# Raw-probe contexts act as the subscriber: the runner sends the requests and
# moqxr must serve them. Only its await-subscribe mode (--forward 0) does that
# without a PUBLISH of its own, and --paced keeps Subgroup streams open between
# Objects so cancellation and update probes have an open stream to act on. Its
# own timeout outlasts the context so an idle publisher is stopped by the runner
# rather than exiting with a failure status first.
# The d21-* ids listed after the first group are probes in which the runner is the
# subscriber: with --forward 1 moqxr also pushes a PUBLISH of its own that those
# probes do not answer, so it waits and exits with a failure status.
# This chooses moqxr CLI options; it never changes what a scenario expects.
if [[ "$draft" == 21 || "$draft" == 22 ]]; then
    case "$impl_id" in
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
        d21-publish-namespace-ok-with-track-properties)
            args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
                  --namespace media --draft "$draft" --forward 0 --paced
                  --timeout "$((timeout_seconds + 3))" --ca "$ca_cert")
            ;;
    esac
fi
# The own draft 22 scenarios and probes (table above).
case "$own22" in
    push)
        args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
              --namespace media --draft "$draft" --forward 1
              --timeout "$timeout_seconds" --ca "$ca_cert")
        ;;
    await-paced)
        args=(--input "$fixture" --endpoint "$endpoint" --transport "$publisher_transport"
              --namespace media --draft "$draft" --forward 0 --paced
              --timeout "$((timeout_seconds + 3))" --ca "$ca_cert")
        ;;
esac
exec "$publisher_bin" "${args[@]}"
