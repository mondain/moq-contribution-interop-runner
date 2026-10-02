#!/usr/bin/env bash
# Unit-test an adapter without the runner, using a capture stub as the publisher.
#
#   bash examples/harness/test-adapter.sh [path/to/adapter]
#
# The stub prints every argument it receives inside <>, so the test can assert
# the exact argument vector, spaces included, without starting any network code.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
adapter=${1:-$here/adapter.sh}
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT

touch "$test_dir/fixture with spaces.mp4" "$test_dir/ca cert.pem"
cat >"$test_dir/publisher binary" <<'STUB'
#!/usr/bin/env bash
printf '<%s>\n' "$@"
STUB
chmod +x "$test_dir/publisher binary"

# Build a contract-v1 request file. Arguments: draft transport scenario [overrides].
make_request() {
    local draft=$1 transport=$2 scenario=$3 namespace=${4:-6d65646961} \
          track=${5:-766964655f31} ca=${6-$test_dir/ca cert.pem} endpoint=${7:-}
    if [[ -z "$endpoint" ]]; then
        if [[ "$transport" == webtransport ]]; then endpoint=https://127.0.0.1:4443/moq
        else endpoint=moqt://127.0.0.1:4443/moq; fi
    fi
    jq -n --arg endpoint "$endpoint" --arg transport "$transport" \
        --arg scenario "$scenario" --arg fixture "$test_dir/fixture with spaces.mp4" \
        --arg ca "$ca" --arg log_dir "$test_dir" --arg namespace "$namespace" \
        --arg track "$track" --argjson draft "$draft" '{
            schema_version: 1, run_id: "run 1", scenario_id: $scenario,
            endpoint: $endpoint, draft: $draft, transport: $transport,
            namespace_hex: [$namespace], track_name_hex: $track,
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: 2500, process_timeout_ms: 4000
        }' >"$test_dir/request.json"
}

run_adapter() {
    ACME_PUB_BIN="$test_dir/publisher binary" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter"
}
expect() {  # expect SUBSTRING OUTPUT
    [[ "$2" == *"$1"* ]] || { printf 'missing %s in:\n%s\n' "$1" "$2" >&2; exit 1; }
}
refuse() {  # refuse DESCRIPTION
    if run_adapter >/dev/null 2>&1; then
        printf 'unexpectedly accepted: %s\n' "$1" >&2
        exit 1
    fi
}

# Accepted requests: the argument vector is translated and survives spaces.
for transport in native_quic webtransport; do
    for draft in 18 21; do
        make_request "$draft" "$transport" some-scenario
        output=$(run_adapter)
        expect "<$test_dir/fixture with spaces.mp4>" "$output"
        expect "<$test_dir/ca cert.pem>" "$output"
        expect "<$draft>" "$output"
        expect "<media>" "$output"
        expect "<vide_1>" "$output"
        expect "<3>" "$output"          # 2500 ms rounds up to 3 s
        expect "<serve>" "$output"
        if [[ "$transport" == webtransport ]]; then expect "<wt>" "$output"
        else expect "<quic>" "$output"; fi
    done
done

# Scenario-specific option: this scenario makes the publisher announce itself.
make_request 21 native_quic d21-publisher-request-stream-placement
expect "<announce>" "$(run_adapter)"

# Requests the adapter cannot map must be refused with a nonzero exit.
make_request 18 native_quic some-scenario 00 ;                     refuse 'binary namespace'
make_request 18 native_quic some-scenario 6d65646961 2f ;          refuse 'slash in track name'
make_request 18 native_quic some-scenario 6d65646961 766964655f31 '' ; refuse 'missing TLS trust'
make_request 19 native_quic some-scenario ;                        refuse 'unsupported draft'
make_request 18 native_quic some-scenario 6d65646961 766964655f31 "$test_dir/ca cert.pem" \
    https://127.0.0.1:4443/moq ;                                   refuse 'endpoint scheme mismatch'
if MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" ACME_PUB_BIN="$test_dir/publisher binary" \
    "$adapter" >/dev/null 2>&1; then
    printf 'missing contract version unexpectedly accepted\n' >&2
    exit 1
fi
printf 'adapter contract passed: %s\n' "$adapter"
