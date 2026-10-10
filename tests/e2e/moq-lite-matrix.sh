#!/usr/bin/env bash
set -euo pipefail

# Usage: moq-lite-matrix.sh [--dry-run] [--transport TRANSPORT] RUNNER_BIN
# Runs the moq-lite adapter contract, then tests/e2e/driven-moq-lite.sh with every executable
# moq-lite-06 scenario (one driven run each), then one group run of the five scenarios of row
# L06-4-4-MUST-027 (the row settles only in a run holding all five), then the audit of the database,
# over native_quic and webtransport, each transport on its own fixed ports (HTTP/UDP):
#   native_quic 19231/19232   webtransport 19233/19234
#   --transport TRANSPORT  run only that transport (on its matrix ports).
#   --dry-run              print the plan (the driven script's --dry-run output per transport) and
#                          start nothing, not even the adapter contract. Off by default.
# The publisher is the moq CLI named by MOQ_CLI_BIN (moq-dev/moq rs/moq-cli, binary `moq`), through
# adapters/moq-lite/run.sh with ffmpeg's test pattern as the source. Without MOQ_CLI_BIN, ffmpeg, jq,
# curl, openssl or the runner binary the matrix skips (exit 77) before running anything.
dry_run=0
only_transport=
while [[ "${1:-}" == --dry-run || "${1:-}" == --transport ]]; do
    if [[ "$1" == --dry-run ]]; then
        dry_run=1
        shift
    else
        [[ $# -ge 2 ]] || { printf 'Usage: --transport TRANSPORT\n' >&2; exit 2; }
        only_transport=$2
        [[ "$only_transport" == native_quic || "$only_transport" == webtransport ]] || exit 2
        shift 2
    fi
done
if [[ $# -ne 1 ]]; then
    printf 'Usage: %s [--dry-run] [--transport TRANSPORT] RUNNER_BIN\n' "$0" >&2
    exit 2
fi
runner_bin=$1
# The runs per transport: each executable scenario alone (the order of driven-moq-lite.sh), then the
# row 027 group run.
matrix_runs=(
    l06-setup-stream l06-setup-unknown-parameter l06-setup-duplicate-parameter
    l06-setup-duplicate-stream l06-setup-server-path l06-setup-server-role l06-setup-client-path
    l06-announce-prefix l06-announce-lifecycle l06-session-stream-close l06-subscribe-latest
    l06-subscribe-refused l06-subscribe-invalid-frame-bounds l06-subscribe-group-floor
    l06-subscribe-abutting-frame-start l06-errors-unknown-stream-type l06-errors-unknown-reset-code
    l06-errors-reserved-reset-code l06-errors-code-space
    l06-errors-code-space,l06-setup-duplicate-stream,l06-setup-duplicate-parameter,l06-setup-server-path,l06-setup-server-role
)
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
driven_args=()
if ((dry_run)); then driven_args=(--dry-run); fi

if ((dry_run)); then
    printf 'contract: bash %s\n' "$script_dir/moq-lite-adapter-contract.sh"
else
    missing=
    [[ -n "${MOQ_CLI_BIN:-}" && -x "${MOQ_CLI_BIN:-}" ]] || missing+=' MOQ_CLI_BIN'
    if [[ -n "${MOQ_FFMPEG_BIN:-}" ]]; then
        [[ -x "$MOQ_FFMPEG_BIN" ]] || missing+=' ffmpeg'
    else
        command -v ffmpeg >/dev/null 2>&1 || missing+=' ffmpeg'
    fi
    for tool in jq curl openssl timeout; do command -v "$tool" >/dev/null 2>&1 || missing+=" $tool"; done
    [[ -x "$runner_bin" ]] || missing+=' runner'
    if [[ -n "$missing" ]]; then
        printf 'SKIP: missing%s\n' "$missing" >&2
        exit 77
    fi
    bash "$script_dir/moq-lite-adapter-contract.sh"
fi
for transport in native_quic webtransport; do
    if [[ -n "$only_transport" && "$transport" != "$only_transport" ]]; then
        continue
    fi
    if [[ "$transport" == native_quic ]]; then
        http_port=19231
        udp_port=19232
    else
        http_port=19233
        udp_port=19234
    fi
    printf 'matrix draft=moq-lite-06 transport=%s\n' "$transport"
    MOQ_INTEROP_TEST_HTTP_PORT="$http_port" \
        MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
        bash "$script_dir/driven-moq-lite.sh" "${driven_args[@]}" "$transport" "$runner_bin" "${matrix_runs[@]}"
done
if ((dry_run)); then
    printf 'matrix dry run completed; nothing was started\n'
else
    printf 'matrix harness completed; publisher exits, judged rows and the audit are reported above\n'
fi
