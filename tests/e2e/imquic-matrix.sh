#!/usr/bin/env bash
set -euo pipefail

# Usage: imquic-matrix.sh [--dry-run] [--pair 22 TRANSPORT] RUNNER_BIN IMQUIC_PUB_BIN MP4_FIXTURE
# Runs the imquic adapter contract, then tests/e2e/driven-imquic.sh for draft 22 (the only draft the
# imquic adapter supports) over native_quic and webtransport, each on its own fixed ports (HTTP/UDP):
#   22 native_quic 19221/19222   22 webtransport 19223/19224
#   --pair 22 TRANSPORT  run only that pair (on its matrix ports).
#   --dry-run            print the plan (the driven script's --dry-run output per pair) and start
#                        nothing, not even the adapter contract. Off by default.
# The publisher is imquic's moq-pub example (IMQUIC_PUB_BIN for the adapter); the fixture is passed to
# the runner but imquic publishes a clock and reads none.
dry_run=0
only_transport=
while [[ "${1:-}" == --dry-run || "${1:-}" == --pair ]]; do
    if [[ "$1" == --dry-run ]]; then
        dry_run=1
        shift
    else
        [[ $# -ge 3 ]] || { printf 'Usage: --pair 22 TRANSPORT\n' >&2; exit 2; }
        [[ "$2" == 22 ]] || exit 2
        only_transport=$3
        [[ "$only_transport" == native_quic || "$only_transport" == webtransport ]] || exit 2
        shift 3
    fi
done
if [[ $# -ne 3 ]]; then
    printf 'Usage: %s [--dry-run] [--pair 22 TRANSPORT] RUNNER_BIN IMQUIC_PUB_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
runner_bin=$1
publisher_bin=$2
fixture=$3
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
driven_args=()
if ((dry_run)); then driven_args=(--dry-run); fi

if ((dry_run)); then
    printf 'contract: bash %s\n' "$script_dir/imquic-adapter-contract.sh"
else
    bash "$script_dir/imquic-adapter-contract.sh"
fi
for transport in native_quic webtransport; do
    if [[ -n "$only_transport" && "$transport" != "$only_transport" ]]; then
        continue
    fi
    if [[ "$transport" == native_quic ]]; then
        http_port=19221
        udp_port=19222
    else
        http_port=19223
        udp_port=19224
    fi
    printf 'matrix draft=22 transport=%s\n' "$transport"
    MOQ_INTEROP_TEST_HTTP_PORT="$http_port" \
        MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
        bash "$script_dir/driven-imquic.sh" "${driven_args[@]}" \
        22 "$transport" "$runner_bin" "$publisher_bin" "$fixture"
done
if ((dry_run)); then
    printf 'matrix dry run completed; nothing was started\n'
else
    printf 'matrix harness completed; publisher exits and scored rows are reported above\n'
fi
