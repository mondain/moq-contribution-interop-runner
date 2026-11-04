#!/usr/bin/env bash
# Stand-in for the moq-interop runner binary in tests/e2e/moqxr-matrix-plan.sh.
#
# Appends the invocation to $MOQ_STUB_DIR/calls.log in the same format as the --dry-run plan of
# tests/e2e/driven-moqxr.sh (`runner: MOQXR_BIN=<publisher> <runner> <arg>...`), writes the retained
# request contract the driven script checks for (run id `run-<http port>`, matching
# tests/support/stub_curl/curl), marks itself ready for the stub health check, and then idles until
# the driven script kills it. It never starts a publisher and opens no socket.
set -euo pipefail
{
    printf 'runner: MOQXR_BIN=<%s> <%s>' "${MOQXR_BIN:-}" "$0"
    printf ' <%s>' "$@"
    printf '\n'
} >>"$MOQ_STUB_DIR/calls.log"
log_root=
port=
while (($#)); do
    case "$1" in
        --driver-log-root) log_root=$2; shift ;;
        --port) port=$2; shift ;;
    esac
    shift
done
[[ -n "$log_root" && -n "$port" ]] || { printf 'stub runner: missing --port or --driver-log-root\n' >&2; exit 2; }
mkdir -p "$log_root/run-$port"
printf '{}\n' >"$log_root/run-$port/request.json"
for scenario in d23-publisher-smoke d23-direct-subscribe-smoke d23-publish-smoke \
    d23-probe-unknown-control-message \
    d23-probe-bad-first-bidi-message \
    d23-probe-length-mismatch \
    d23-probe-oversize-control-message \
    d23-probe-server-authority \
    d23-probe-server-path \
    d23-probe-unknown-uni-stream \
    d23-probe-subscribe-reserved-namespace \
    d23-probe-subscribe-session-namespace \
    d23-probe-empty-track-name \
    d23-probe-control-stream-closed \
    d23-probe-wrong-request-id-parity \
    d23-probe-duplicate-request-id \
    d23-probe-setup-delta-overflow \
    d23-probe-setup-malformed-value \
    d23-probe-reason-phrase-too-long \
    d23-probe-namespace-empty-field \
    d23-probe-namespace-too-many-fields \
    d23-probe-namespace-too-long \
    d23-probe-unknown-setup-option \
    d23-probe-duplicate-unknown-setup-option \
    d23-probe-request-ok-with-properties \
    d23-probe-unknown-error-detail \
    d23-probe-redirect-without-detail \
    d23-probe-redirect-track-name-in-namespace-request \
    d23-probe-unknown-request-error-code \
    d23-probe-publish-state-notify-on-announce \
    d23-probe-publish-state-notify-to-publisher \
    d23-probe-unknown-message-parameter \
    d23-probe-parameter-out-of-scope \
    d23-probe-group-order-out-of-range \
    d23-probe-paused-out-of-range \
    d23-probe-include-properties-out-of-range \
    d23-probe-subgroup-header-reserved-mode \
    d23-probe-datagram-unknown-type \
    d23-probe-datagram-reserved-bit \
    d23-probe-datagram-status-end-of-group \
    d23-probe-datagram-properties-length-zero \
    d23-probe-filter-ranges-over-limit \
    d23-probe-location-filter-unknown-type \
    d23-probe-location-filter-overflow \
    d23-probe-fill-parameters-invalid \
    d23-probe-padding-stream \
    d23-probe-padding-datagram \
    d23-probe-goaway-twice \
    d23-probe-goaway-uri-too-long \
    d23-probe-goaway-on-subscribe-stream \
    d23-probe-request-update-wrong-direction \
    d23-probe-second-announce-response \
    d23-probe-second-publish-response \
    d23-probe-duplicate-parameter \
    d23-probe-datagram-status-properties-not-normal; do
    mkdir -p "$log_root/run-$port/$scenario"
    printf '{}\n' >"$log_root/run-$port/$scenario/request.json"
done
touch "$MOQ_STUB_DIR/ready-$port"
exec sleep 60
