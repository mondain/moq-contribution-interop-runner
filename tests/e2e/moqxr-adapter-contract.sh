#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moqxr/run.sh"
capture_source="$root_dir/tests/support/capture_publisher.sh"
test_dir=$(mktemp -d /tmp/moqxr-adapter-contract.XXXXXX)
trap 'rm -f -- "$test_dir/request.next" "$test_dir/request.json" "$test_dir/fixture with spaces.mp4" "$test_dir/ca cert.pem" "$test_dir/publisher binary"; rmdir -- "$test_dir"' EXIT
touch "$test_dir/fixture with spaces.mp4" "$test_dir/ca cert.pem"
capture="$test_dir/publisher binary"
ln -s "$capture_source" "$capture"

make_request() {
    local draft=$1 transport=$2 namespace=$3 track=$4 ca=$5
    local endpoint=${6:-}
    if [[ -n "$endpoint" ]]; then
        :
    elif [[ "$transport" == webtransport ]]; then
        endpoint=https://127.0.0.1:4443/moq
    else
        endpoint=moqt://127.0.0.1:4443/moq
    fi
    jq -n --arg endpoint "$endpoint" --arg transport "$transport" \
        --arg fixture "$test_dir/fixture with spaces.mp4" --arg ca "$ca" \
        --arg log_dir "$test_dir" --arg namespace "$namespace" --arg track "$track" \
        --argjson draft "$draft" '{
            schema_version: 1, run_id: "run 1", scenario_id: "scenario 1",
            endpoint: $endpoint, draft: $draft, transport: $transport,
            namespace_hex: [$namespace], track_name_hex: $track,
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: 2500, process_timeout_ms: 4000
        }' >"$test_dir/request.json"
}

check_case() {
    local draft=$1 transport=$2 expected_forward=$3
    make_request "$draft" "$transport" 6d65646961 766964655f31 "$test_dir/ca cert.pem"
    local output
    output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
    [[ "$output" == *"<--input>"* ]]
    [[ "$output" == *"<$test_dir/fixture with spaces.mp4>"* ]]
    [[ "$output" == *"<--endpoint>"* ]]
    [[ "$output" == *"<--draft>"* ]]
    [[ "$output" == *"<$draft>"* ]]
    [[ "$output" == *"<--namespace>"* && "$output" == *"<media>"* ]]
    [[ "$output" == *"<--forward>"* && "$output" == *"<$expected_forward>"* ]]
    [[ "$output" == *"<--ca>"* && "$output" == *"<$test_dir/ca cert.pem>"* ]]
    [[ "$output" == *"<--timeout>"* && "$output" == *"<3>"* ]]
    if [[ "$transport" == webtransport ]]; then
        [[ "$output" == *"<webtransport>"* ]]
    else
        [[ "$output" == *"<raw>"* ]]
    fi
    [[ "$output" != *"<--preannounce-tracks>"* ]]
}

check_case 18 native_quic 0
check_case 21 native_quic 1
check_case 18 webtransport 0
check_case 21 webtransport 1

# Scenarios in which the runner subscribes make moqxr await that SUBSCRIBE.
make_request 21 webtransport 6d65646961 766964655f31 "$test_dir/ca cert.pem"
jq '.scenario_id = "d21-fill-fails-before-first-object"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<--forward>"* && "$output" == *"<0>"* && "$output" != *"<1>"* ]]

make_request 18 native_quic 00 766964655f31 "$test_dir/ca cert.pem"
if MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'binary namespace unexpectedly accepted\n' >&2
    exit 1
fi
make_request 18 native_quic 6d65646961 00 "$test_dir/ca cert.pem"
if MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'unsupported track unexpectedly accepted\n' >&2
    exit 1
fi
make_request 18 native_quic 6d65646961 766964655f31 ""
if MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'missing TLS trust unexpectedly accepted\n' >&2
    exit 1
fi
make_request 19 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
if MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'unsupported draft unexpectedly accepted\n' >&2
    exit 1
fi
make_request 18 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem" \
    "https://127.0.0.1:4443/moq"
if MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'mismatched transport endpoint unexpectedly accepted\n' >&2
    exit 1
fi
# Scenarios that observe a publisher-originated PUBLISH ask moqxr to publish its catalog track.
for scenario in publish-track-under-single-period-namespace application-publish-track-in-session-namespace \
                publish-distinct-content-tracks-in-same-scope scenario-without-publish; do
    make_request 18 webtransport 6d65646961 766964655f31 "$test_dir/ca cert.pem"
    jq --arg scenario "$scenario" '.scenario_id = $scenario' "$test_dir/request.json" >"$test_dir/request.next"
    mv "$test_dir/request.next" "$test_dir/request.json"
    output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
    if [[ "$scenario" == scenario-without-publish ]]; then
        [[ "$output" != *"<--publish-catalog>"* ]]
    else
        [[ "$output" == *"<--publish-catalog>"* ]]
    fi
done
printf 'moqxr adapter contract passed\n'
