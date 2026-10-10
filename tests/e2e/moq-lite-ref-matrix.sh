#!/usr/bin/env bash
set -euo pipefail

# Usage: moq-lite-ref-matrix.sh [--dry-run] [--transport TRANSPORT] RUNNER_BIN
# The conforming sweep (L2c): every executable moq-lite-06 scenario, one driven run each, on native_quic and
# webtransport, against the CONFORMING reference publisher (adapters/moq-lite-ref, MOQ_LITE_REF_BIN), plus one group
# run of the five scenarios of row L06-4-4-MUST-027 and l06-probe-report again with --probe-level none (the run that
# judges row 075, which a Report-advertising publisher makes not applicable). Expectations, per transport:
#   - no judged row fails and no run errors;
#   - the rows 075, 077, 105 and 186 the moq CLI cannot reach all pass (in the runs named above);
#   - the audit of the run database reports status 0.
# Ports: native_quic 19241/19242, webtransport 19243/19244. Without MOQ_LITE_REF_BIN, jq, curl, openssl, timeout or
# the runner the sweep skips (exit 77). --dry-run prints the plan and starts nothing.
dry_run=0
only_transport=
while [[ "${1:-}" == --dry-run || "${1:-}" == --transport ]]; do
    if [[ "$1" == --dry-run ]]; then dry_run=1; shift
    else
        [[ $# -ge 2 ]] || { printf 'Usage: --transport TRANSPORT\n' >&2; exit 2; }
        only_transport=$2
        [[ "$only_transport" == native_quic || "$only_transport" == webtransport ]] || exit 2
        shift 2
    fi
done
[[ $# -eq 1 ]] || { printf 'Usage: %s [--dry-run] [--transport TRANSPORT] RUNNER_BIN\n' "$0" >&2; exit 2; }
runner_bin=$1
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# "scenario|operator flags": the flags the reference publisher needs for the scenario.
plan=(
    'l06-setup-stream|' 'l06-setup-unknown-parameter|' 'l06-setup-duplicate-parameter|'
    'l06-setup-duplicate-stream|' 'l06-setup-server-path|' 'l06-setup-server-role|' 'l06-setup-client-path|'
    'l06-announce-prefix|' 'l06-announce-lifecycle|' 'l06-session-stream-close|' 'l06-subscribe-latest|'
    'l06-subscribe-refused|' 'l06-subscribe-invalid-frame-bounds|' 'l06-subscribe-group-floor|'
    'l06-subscribe-abutting-frame-start|' 'l06-errors-unknown-stream-type|' 'l06-errors-unknown-reset-code|'
    'l06-errors-reserved-reset-code|' 'l06-errors-code-space|'
    'l06-track-info|' 'l06-fetch-group|' 'l06-fetch-unknown-group|' 'l06-probe-report|'
    'l06-probe-report|--probe-level none'
    'l06-datagram-size|--datagrams --frames-per-group 1'
    'l06-goaway-single|' 'l06-goaway-duplicate|' 'l06-goaway-oversize|'
    'l06-errors-code-space,l06-setup-duplicate-stream,l06-setup-duplicate-parameter,l06-setup-server-path,l06-setup-server-role|'
)

if ((!dry_run)); then
    missing=
    [[ -n "${MOQ_LITE_REF_BIN:-}" && -x "${MOQ_LITE_REF_BIN:-}" ]] || missing+=' MOQ_LITE_REF_BIN'
    for tool in jq curl openssl timeout; do command -v "$tool" >/dev/null 2>&1 || missing+=" $tool"; done
    [[ -x "$runner_bin" ]] || missing+=' runner'
    if [[ -n "$missing" ]]; then printf 'SKIP: missing%s\n' "$missing" >&2; exit 77; fi
fi

failures=0
for transport in native_quic webtransport; do
    [[ -z "$only_transport" || "$transport" == "$only_transport" ]] || continue
    if [[ "$transport" == native_quic ]]; then http_port=19241; udp_port=19242
    else http_port=19243; udp_port=19244; fi
    printf 'ref matrix draft=moq-lite-06 transport=%s\n' "$transport"
    declare -A judged=()
    for entry in "${plan[@]}"; do
        scenario=${entry%%|*}
        flags=${entry#*|}
        if ((dry_run)); then
            printf 'run: %s flags=<%s>\n' "$scenario" "$flags"
            continue
        fi
        out=$(MOQ_LITE_ADAPTER=moq-lite-ref MOQ_LITE_REF_ARGS="$flags" \
            MOQ_INTEROP_TEST_HTTP_PORT="$http_port" MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
            bash "$script_dir/driven-moq-lite.sh" "$transport" "$runner_bin" "$scenario" 2>&1) || {
            printf '%s\n' "$out"; printf 'FAIL: driven run %s %s exited non-zero\n' "$scenario" "$transport" >&2
            failures=$((failures + 1)); continue; }
        printf '%s\n' "$out" | grep -E '^scenario=|^  (harness_error|context_skipped)|^audit status' || true
        grep -qE '^audit status=0$' <<<"$out" || { printf 'FAIL: audit not clean for %s\n' "$scenario" >&2; failures=$((failures + 1)); }
        if grep -qE '^  row .* fail$' <<<"$out"; then
            printf 'FAIL: a row failed for %s (%s)\n' "$scenario" "$flags" >&2
            grep -E '^  row .* fail$' <<<"$out" >&2
            failures=$((failures + 1))
        fi
        if grep -qE '^  harness_error' <<<"$out"; then
            printf 'FAIL: harness error in %s\n' "$scenario" >&2; failures=$((failures + 1))
        fi
        while read -r _ row state; do [[ "$state" == pass ]] && judged[$row]=1; done < <(grep -E '^  row ' <<<"$out")
    done
    if ((!dry_run)); then
        for row in L06-5-1-5-MUST-075 L06-5-1-6-MUST-NOT-077 L06-6-4-MUST-NOT-105 L06-7-18-MUST-186; do
            [[ -n "${judged[$row]:-}" ]] || { printf 'FAIL: %s did not pass on %s\n' "$row" "$transport" >&2; failures=$((failures + 1)); }
        done
    fi
    unset judged
done
if ((dry_run)); then printf 'ref matrix dry run completed; nothing was started\n'; exit 0; fi
((failures == 0)) || { printf 'ref matrix: %s failure(s)\n' "$failures" >&2; exit 1; }
printf 'ref matrix completed: no failing row, rows 075 077 105 186 pass, audits clean\n'
