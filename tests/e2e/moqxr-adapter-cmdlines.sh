#!/usr/bin/env bash
# Pin the argument list adapters/moqxr/run.sh would exec the moqxr publisher with.
#
# Usage: moqxr-adapter-cmdlines.sh DRAFT [--update]
#   DRAFT     18 or 21.
#   --update  rewrite tests/golden/moqxr-cmdlines-dDRAFT.txt from the live adapter instead of
#             comparing. MOQ_UPDATE_GOLDEN=1 in the environment does the same. Without one of
#             them the golden file is never written: a mismatch fails with a unified diff.
#
# For every executable scenario id of the draft (tests/golden/executable-ids-dDRAFT.txt, kept equal
# to the registry by the executable-ids-golden unit test), plus the ids run.sh special-cases, and for
# both transports, the adapter runs against a deterministic request with a stub MOQXR_BIN that
# prints its arguments. One line per pair: `<scenario_id> <transport> <arguments>`. Temporary
# paths are normalized to @TMP@. Needs only bash and jq; no network.
set -euo pipefail

draft=${1:-}
[[ "$draft" == 18 || "$draft" == 21 ]] || { printf 'usage: %s 18|21 [--update]\n' "$0" >&2; exit 2; }
update=0
[[ "${2:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moqxr/run.sh"
ids_file="$root_dir/tests/golden/executable-ids-d$draft.txt"
golden="$root_dir/tests/golden/moqxr-cmdlines-d$draft.txt"
stub_source="$root_dir/tests/support/capture_publisher.sh"

work=$(mktemp -d /tmp/moqxr-adapter-cmdlines.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
touch "$work/fixture.mp4" "$work/ca.pem"
ln -s "$stub_source" "$work/publisher"

# Ids run.sh special-cases by name; they are pinned even if a registry change drops them.
extra_ids=(publish-track-under-single-period-namespace
           application-publish-track-in-session-namespace
           publish-distinct-content-tracks-in-same-scope
           scenario-without-special-options)

ids=$(sort -u < <(cat "$ids_file"; printf '%s\n' "${extra_ids[@]}"))

live="$work/live.txt"
: >"$live"
while IFS= read -r id; do
    [[ -n "$id" ]] || continue
    for transport in native_quic webtransport; do
        if [[ "$transport" == webtransport ]]; then
            endpoint=https://127.0.0.1:4443/moq
        else
            endpoint=moqt://127.0.0.1:4443/moq
        fi
        jq -n --arg id "$id" --arg transport "$transport" --arg endpoint "$endpoint" \
            --arg fixture "$work/fixture.mp4" --arg ca "$work/ca.pem" --arg log_dir "$work" \
            --argjson draft "$draft" '{
                schema_version: 1, run_id: "run-1", scenario_id: $id,
                endpoint: $endpoint, draft: $draft, transport: $transport,
                namespace_hex: ["6d65646961"], track_name_hex: "766964655f31",
                fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
                scenario_timeout_ms: 2500, process_timeout_ms: 4000
            }' >"$work/request.json"
        args=$(MOQXR_BIN="$work/publisher" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
            MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" | tr '\n' ' ')
        args=${args//"$work"/@TMP@}
        printf '%s %s %s\n' "$id" "$transport" "${args% }" >>"$live"
    done
done <<<"$ids"

if ((update)); then
    cp -- "$live" "$golden"
    printf 'updated %s (%s lines)\n' "$golden" "$(wc -l <"$golden")"
    exit 0
fi
[[ -f "$golden" ]] || { printf 'missing golden %s (run with --update)\n' "$golden" >&2; exit 1; }
if ! diff -u "$golden" "$live"; then
    printf 'moqxr adapter command lines for draft %s differ from %s\n' "$draft" "$golden" >&2
    exit 1
fi
printf 'moqxr adapter command lines for draft %s match (%s lines)\n' "$draft" "$(wc -l <"$golden")"
