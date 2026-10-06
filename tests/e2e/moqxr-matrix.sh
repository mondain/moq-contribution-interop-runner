#!/usr/bin/env bash
set -euo pipefail

# Usage: moqxr-matrix.sh [--dry-run] [--pair DRAFT TRANSPORT] RUNNER_BIN MOQXR_BIN MP4_FIXTURE
# Runs the adapter contract, then tests/e2e/driven-moqxr.sh for drafts 18, 21 and 22 over
# native_quic and webtransport, each pair on its own fixed ports (HTTP/UDP):
#   18 native_quic 19201/19202   21 native_quic 19203/19204   22 native_quic 19209/19210
#   18 webtransport 19205/19206  21 webtransport 19207/19208  22 webtransport 19211/19212
#   --pair DRAFT TRANSPORT  run only that pair (on its matrix ports).
#   --dry-run               print the plan (the driven script's --dry-run output per pair) and
#                           start nothing, not even the adapter contract.
dry_run=0
only_draft=
only_transport=
while [[ "${1:-}" == --dry-run || "${1:-}" == --pair ]]; do
    if [[ "$1" == --dry-run ]]; then
        dry_run=1
        shift
    else
        [[ $# -ge 3 ]] || { printf 'Usage: --pair DRAFT TRANSPORT\n' >&2; exit 2; }
        only_draft=$2
        only_transport=$3
        [[ "$only_draft" == 18 || "$only_draft" == 21 || "$only_draft" == 22 ]] || exit 2
        [[ "$only_transport" == native_quic || "$only_transport" == webtransport ]] || exit 2
        shift 3
    fi
done
if [[ $# -ne 3 ]]; then
    printf 'Usage: %s RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
runner_bin=$1
publisher_bin=$2
fixture=$3
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
driven_args=()
if ((dry_run)); then driven_args=(--dry-run); fi

if ((dry_run)); then
    printf 'contract: bash %s\n' "$script_dir/moqxr-adapter-contract.sh"
else
    bash "$script_dir/moqxr-adapter-contract.sh"
fi
for draft in 18 21 22; do
    for transport in native_quic webtransport; do
        if [[ -n "$only_draft" ]] &&
            [[ "$draft" != "$only_draft" || "$transport" != "$only_transport" ]]; then
            continue
        fi
        if [[ "$draft" == 18 && "$transport" == native_quic ]]; then
            http_port=19201
            udp_port=19202
        elif [[ "$draft" == 21 && "$transport" == native_quic ]]; then
            http_port=19203
            udp_port=19204
        elif [[ "$draft" == 22 && "$transport" == native_quic ]]; then
            http_port=19209
            udp_port=19210
        elif [[ "$draft" == 18 ]]; then
            http_port=19205
            udp_port=19206
        elif [[ "$draft" == 21 ]]; then
            http_port=19207
            udp_port=19208
        else
            http_port=19211
            udp_port=19212
        fi
        printf 'matrix draft=%s transport=%s\n' "$draft" "$transport"
        MOQ_INTEROP_TEST_HTTP_PORT="$http_port" \
            MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
            bash "$script_dir/driven-moqxr.sh" "${driven_args[@]}" \
            "$draft" "$transport" "$runner_bin" "$publisher_bin" "$fixture"
    done
done
if ((dry_run)); then
    printf 'matrix dry run completed; nothing was started\n'
else
    printf 'matrix harness completed; publisher exits and scored rows are reported above\n'
fi
