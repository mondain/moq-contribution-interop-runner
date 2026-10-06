#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moqxr/run.sh"
capture_source="$root_dir/tests/support/capture_publisher.sh"
test_dir=$(mktemp -d /tmp/moqxr-adapter-contract.XXXXXX)
trap 'rm -f -- "$test_dir/request.next" "$test_dir/request.json" "$test_dir/fixture with spaces.mp4" "$test_dir/ca cert.pem" "$test_dir/publisher binary" "$test_dir/draft23.err" "$test_dir/moq5.err"; rmdir -- "$test_dir"' EXIT
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
check_case 22 native_quic 1
check_case 22 webtransport 1

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
# Draft 22 runs: the shared scenarios reuse their draft 21 options (a d22- id gets the options of its
# d21- twin), and own scenarios have their own entries.
make_request 22 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
jq '.scenario_id = "d22-fill-fails-before-first-object"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<--draft>"$'\n'"<22>"* ]]
[[ "$output" == *"<--forward>"$'\n'"<0>"* && "$output" != *"<--paced>"* ]]
make_request 22 webtransport 6d65646961 766964655f31 "$test_dir/ca cert.pem"
jq '.scenario_id = "d22-update-subscription-location-range"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<--forward>"$'\n'"<0>"$'\n'"<--paced>"$'\n'"<--timeout>"$'\n'"<6>"* ]]
# An own draft 22 id takes its own entry, not the options of its d21- namesake: the draft 21
# d21-subscribe-bounded-location-range runs with --forward 1, the draft 22 own scenario of the same name
# with --forward 0 --paced.
make_request 21 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
jq '.scenario_id = "d21-subscribe-bounded-location-range"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<--forward>"$'\n'"<1>"$'\n'"<--timeout>"$'\n'"<3>"* && "$output" != *"<--paced>"* ]]
make_request 22 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
jq '.scenario_id = "d22-subscribe-bounded-location-range"' "$test_dir/request.json" >"$test_dir/request.next"
mv "$test_dir/request.next" "$test_dir/request.json"
output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<--forward>"$'\n'"<0>"$'\n'"<--paced>"$'\n'"<--timeout>"$'\n'"<6>"* ]]
# A draft 22 override of the draft 21 option lists: the shared d22- id runs paced while its d21- twin
# keeps --forward 1 at draft 21 (moqxr blocks on its own PUBLISH; draft 21 is frozen).
for transport in native_quic webtransport; do
    make_request 21 "$transport" 6d65646961 766964655f31 "$test_dir/ca cert.pem"
    jq '.scenario_id = "d21-subscribe-empty-namespace-field"' "$test_dir/request.json" >"$test_dir/request.next"
    mv "$test_dir/request.next" "$test_dir/request.json"
    output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
    [[ "$output" == *"<--draft>"$'\n'"<21>"$'\n'"<--forward>"$'\n'"<1>"$'\n'"<--timeout>"$'\n'"<3>"* ]]
    [[ "$output" != *"<--paced>"* ]]
    make_request 22 "$transport" 6d65646961 766964655f31 "$test_dir/ca cert.pem"
    jq '.scenario_id = "d22-subscribe-empty-namespace-field"' "$test_dir/request.json" >"$test_dir/request.next"
    mv "$test_dir/request.next" "$test_dir/request.json"
    output=$(MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
    [[ "$output" == *"<--draft>"$'\n'"<22>"$'\n'"<--forward>"$'\n'"<0>"$'\n'"<--paced>"$'\n'"<--timeout>"$'\n'"<6>"* ]]
done
# A draft the adapter does not list is refused (exit 64) before the publisher starts.
make_request 23 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
set +e
MOQXR_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>"$test_dir/draft23.err"
status=$?
set -e
[[ "$status" -eq 64 ]] || { printf 'draft 23 request not refused (status %s)\n' "$status" >&2; exit 1; }
grep -q 'moqxr adapter: unsupported or malformed request (supported drafts: 18, 21, 22)' "$test_dir/draft23.err"
# The moq5 adapter does not speak draft 22: it refuses with a message that says so (exit 64).
moq5_adapter="$root_dir/adapters/moq5/run.sh"
make_request 22 native_quic 6d65646961 766964655f31 "$test_dir/ca cert.pem"
set +e
MOQ5_MEDIA_SEND_BIN="$capture" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$moq5_adapter" >/dev/null 2>"$test_dir/moq5.err"
status=$?
set -e
[[ "$status" -eq 64 ]] || { printf 'moq5 draft 22 request not refused (status %s)\n' "$status" >&2; exit 1; }
grep -qx 'moq5 adapter: draft 22 is not supported by this adapter' "$test_dir/moq5.err"
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
