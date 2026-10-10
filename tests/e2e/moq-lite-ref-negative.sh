#!/usr/bin/env bash
set -euo pipefail

# Usage: moq-lite-ref-negative.sh [--update] [--dry-run] [--transport TRANSPORT] RUNNER_BIN
# The negative sweep (L2c): each named defect of the reference publisher (--defect) run on its scenario on native_quic
# and webtransport. A defect with an evaluator must make exactly its row fail; a defect without one (judged only by the
# scripted tests) must make no row fail. The rows that failed are recorded in tests/golden/moq-lite-ref-defects.txt
# (`defect scenario transport failing-rows`), so any other row failing, or this one not failing, is a diff.
#   --update   rewrite the golden after the run (the expectations below still have to hold)
#   --dry-run  print the plan and start nothing
# Ports: native_quic 19251/19252, webtransport 19253/19254. Skips (exit 77) without MOQ_LITE_REF_BIN, jq, curl, openssl,
# timeout or the runner.
update=0
dry_run=0
only_transport=
while [[ "${1:-}" == --update || "${1:-}" == --dry-run || "${1:-}" == --transport ]]; do
    case "$1" in
        --update) update=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        --transport) only_transport=${2:?}; shift 2 ;;
    esac
done
[[ $# -eq 1 ]] || { printf 'Usage: %s [--update] [--dry-run] [--transport T] RUNNER_BIN\n' "$0" >&2; exit 2; }
runner_bin=$1
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
golden="$script_dir/../golden/moq-lite-ref-defects.txt"

# defect|scenario|extra flags|the row that must fail ("none": no evaluator judges the defect)
plan=(
    'no-setup-stream|l06-setup-stream||L06-3-1-MUST-014'
    'silent-on-announce|l06-announce-prefix||L06-7-4-MUST-139'
    'ignore-unknown-streams|l06-errors-unknown-stream-type||L06-7-2-MUST-108'
    'close-on-invalid-subscribe|l06-subscribe-invalid-frame-bounds||L06-3-6-MUST-023'
    'offset-group-start|l06-subscribe-group-floor||none'
    'fetch-truncates-on-short-range|l06-fetch-group||L06-7-16-MUST-177'
    'fetch-ignores-unknown-group|l06-fetch-unknown-group||L06-5-1-3-MUST-066'
    'track-info-changes-between-requests|l06-track-info||L06-7-12-MUST-NOT-163'
    'track-info-zero-timescale|l06-track-info||L06-7-12-MUST-170'
    'probe-resets-on-target|l06-probe-report||L06-5-1-5-MUST-072'
    'probe-none-not-reset|l06-probe-report|--probe-level none|L06-5-1-5-MUST-075'
    'goaway-oversize-logged|l06-goaway-oversize||L06-7-18-MUST-179'
    'goaway-duplicate-ignored|l06-goaway-duplicate||L06-7-18-MUST-186'
    'goaway-closes-session-on-first|l06-goaway-single||none'
    'opens-streams-after-goaway|l06-goaway-single|--groups 14 --group-interval-polls 300|L06-5-1-6-MUST-NOT-077'
    'datagram-oversize|l06-datagram-size||L06-6-4-MUST-NOT-105'
    'datagram-unknown-subscribe-id|l06-datagram-size||none'
    'datagram-differs-from-stream|l06-datagram-size||none'
    'datagram-only|l06-datagram-size||none'
)

if ((!dry_run)); then
    missing=
    [[ -n "${MOQ_LITE_REF_BIN:-}" && -x "${MOQ_LITE_REF_BIN:-}" ]] || missing+=' MOQ_LITE_REF_BIN'
    for tool in jq curl openssl timeout; do command -v "$tool" >/dev/null 2>&1 || missing+=" $tool"; done
    [[ -x "$runner_bin" ]] || missing+=' runner'
    if [[ -n "$missing" ]]; then printf 'SKIP: missing%s\n' "$missing" >&2; exit 77; fi
fi

failures=0
lines=()
for transport in native_quic webtransport; do
    [[ -z "$only_transport" || "$transport" == "$only_transport" ]] || continue
    if [[ "$transport" == native_quic ]]; then http_port=19251; udp_port=19252
    else http_port=19253; udp_port=19254; fi
    for entry in "${plan[@]}"; do
        IFS='|' read -r defect scenario flags expected <<<"$entry"
        if ((dry_run)); then printf 'run: %s %s %s flags=<--defect %s %s> expect=%s\n' "$defect" "$scenario" "$transport" "$defect" "$flags" "$expected"; continue; fi
        out=$(MOQ_LITE_ADAPTER=moq-lite-ref MOQ_LITE_REF_ARGS="--defect $defect $flags" \
            MOQ_INTEROP_TEST_HTTP_PORT="$http_port" MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
            bash "$script_dir/driven-moq-lite.sh" "$transport" "$runner_bin" "$scenario" 2>&1) ||
            { printf '%s\n' "$out" >&2; printf 'FAIL: run %s %s %s exited non-zero\n' "$defect" "$scenario" "$transport" >&2
              failures=$((failures + 1)); continue; }
        failing=$(grep -E '^  row .* fail$' <<<"$out" | awk '{print $2}' | sort | paste -sd, - || true)
        grep -qE '^  harness_error' <<<"$out" && { printf 'FAIL: harness error: %s %s %s\n' "$defect" "$scenario" "$transport" >&2; failures=$((failures + 1)); }
        grep -qE '^audit status=0$' <<<"$out" || { printf 'FAIL: audit not clean: %s %s %s\n' "$defect" "$scenario" "$transport" >&2; failures=$((failures + 1)); }
        if [[ "$expected" == none ]]; then
            [[ -z "$failing" ]] || { printf 'FAIL: %s on %s %s failed %s, expected no row\n' "$defect" "$scenario" "$transport" "$failing" >&2; failures=$((failures + 1)); }
        else
            [[ ",$failing," == *",$expected,"* ]] || { printf 'FAIL: %s on %s %s did not fail %s (failed: %s)\n' "$defect" "$scenario" "$transport" "$expected" "${failing:-none}" >&2; failures=$((failures + 1)); }
        fi
        lines+=("$defect $scenario $transport ${failing:-none}")
        printf '%s %s %s -> %s\n' "$defect" "$scenario" "$transport" "${failing:-none}"
    done
done
if ((dry_run)); then printf 'negative sweep dry run completed; nothing was started\n'; exit 0; fi
((failures == 0)) || { printf 'negative sweep: %s failure(s)\n' "$failures" >&2; exit 1; }
actual=$(printf '%s\n' "${lines[@]}" | sort)
if ((update)); then printf '%s\n' "$actual" >"$golden"; printf 'updated %s\n' "$golden"; exit 0; fi
if [[ -n "$only_transport" ]]; then printf 'negative sweep (%s only): expectations hold; golden not compared\n' "$only_transport"; exit 0; fi
diff -u "$golden" <(printf '%s\n' "$actual") || { printf 'failing rows differ from %s (rerun with --update after review)\n' "$golden" >&2; exit 1; }
printf 'negative sweep completed: every defect fails exactly its recorded rows\n'
