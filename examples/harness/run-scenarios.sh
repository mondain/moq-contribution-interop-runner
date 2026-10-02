#!/usr/bin/env bash
# Create one run on a running interop runner, wait for it to finalize, print the
# verdict and the failed rows, and save the JSON and TAP exports.
#
#   examples/harness/run-scenarios.sh DRAFT TRANSPORT MODE SCENARIO[,SCENARIO...]
#
#   DRAFT      18 or 21
#   TRANSPORT  native-quic or webtransport
#   MODE       observed or driven
#
# Environment (all optional):
#   RUNNER_URL          runner base URL                  (default http://127.0.0.1:8080)
#   TRACK_NAMESPACE     namespace fields separated by /  (default media)
#   TRACK_NAME          track name                       (default vide_1)
#   TIMEOUT_MS          timeout per scenario context     (default 10000)
#   OUT_DIR             where to save results            (default ./results)
#   FAIL_ON_INCOMPLETE  set to 1 to treat "incomplete" as a failure
#
# In observed mode the script prints the publisher endpoint and waits; start your
# publisher against it. Exit status: 0 for pass (or incomplete), 1 for fail,
# 2 for error, usage problems or a run that did not finalize in time.
set -euo pipefail

(($# == 4)) || { sed -n '2,20p' "$0" >&2; exit 2; }
draft=$1 transport=$2 mode=$3 scenarios=$4
runner=${RUNNER_URL:-http://127.0.0.1:8080}
timeout_ms=${TIMEOUT_MS:-10000}
out_dir=${OUT_DIR:-results}

to_hex() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }

fields=()
IFS=/ read -r -a parts <<<"${TRACK_NAMESPACE:-media}"
for part in "${parts[@]}"; do fields+=("$(to_hex "$part")"); done
namespace_json=$(printf '%s\n' "${fields[@]}" | jq -R . | jq -s .)
request=$(jq -n --argjson draft "$draft" --arg transport "$transport" --arg mode "$mode" \
    --arg scenarios "$scenarios" --argjson timeout "$timeout_ms" \
    --argjson namespace "$namespace_json" --arg name "$(to_hex "${TRACK_NAME:-vide_1}")" \
    '{draft: $draft, transport: $transport, mode: $mode,
      scenarios: ($scenarios | split(",")), timeout_ms: $timeout,
      track: {namespace_hex: $namespace, name_hex: $name}}')

created=$(curl --silent --show-error --write-out '\n%{http_code}' -X POST \
    "$runner/api/v1/runs" -H 'Content-Type: application/json' -d "$request")
status=${created##*$'\n'}
body=${created%$'\n'*}
if [[ "$status" != 201 ]]; then
    printf 'run rejected (HTTP %s): %s\n' "$status" "$body" >&2
    exit 2
fi
run_id=$(jq -r '.run.id' <<<"$body")
printf 'run %s\npublisher endpoint: %s\n' "$run_id" "$(jq -c '.publisher_endpoint' <<<"$body")"

# Allow every context its full timeout, plus slack for process start and stop.
contexts=$(awk -F, '{print NF}' <<<"$scenarios")
deadline=$((SECONDS + contexts * (timeout_ms / 1000 + 3) + 10))
state=active
while ((SECONDS < deadline)); do
    run=$(curl --silent --show-error --fail "$runner/api/v1/runs/$run_id")
    state=$(jq -r '.run.state' <<<"$run")
    [[ "$state" == finalized ]] && break
    sleep 0.5
done
if [[ "$state" != finalized ]]; then
    printf 'run %s did not finalize in time\n' "$run_id" >&2
    exit 2
fi

mkdir -p -- "$out_dir"
curl --silent --show-error --fail "$runner/results/$run_id.json" >"$out_dir/$run_id.json"
curl --silent --show-error --fail "$runner/results/$run_id.tap" >"$out_dir/$run_id.tap"
verdict=$(jq -r '.run.verdict' <<<"$run")
printf 'verdict: %s\n' "$verdict"
jq -r '.run.score | "required \(.required.earned)/\(.required.possible)  weighted \(.weighted.earned)/\(.weighted.possible)  coverage \(.coverage.earned)/\(.coverage.possible)"' <<<"$run"
jq -r '.run.outcomes | group_by(.state) | map("\(.[0].state)=\(length)") | join("  ")' <<<"$run"
jq -r '.requirements[] | select(.outcome == "fail") |
    "FAIL \(.id) (\(.strength), draft section \(.source.section), lines \(.source.first_line)-\(.source.last_line)): \(.summary)"' \
    "$out_dir/$run_id.json"
printf 'saved %s/%s.json and .tap; report: %s/results/%s\n' "$out_dir" "$run_id" "$runner" "$run_id"

case "$verdict" in
    pass) exit 0 ;;
    incomplete) [[ "${FAIL_ON_INCOMPLETE:-0}" == 1 ]] && exit 1 || exit 0 ;;
    fail) exit 1 ;;
    *) exit 2 ;;
esac
