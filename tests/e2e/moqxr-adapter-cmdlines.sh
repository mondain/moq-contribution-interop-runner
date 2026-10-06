#!/usr/bin/env bash
# Pin the argument list adapters/moqxr/run.sh would exec the moqxr publisher with.
#
# Usage: moqxr-adapter-cmdlines.sh DRAFT [--update]
#   DRAFT     18, 21 or 22.
#   --update  rewrite tests/golden/moqxr-cmdlines-dDRAFT.txt from the live adapter instead of
#             comparing. MOQ_UPDATE_GOLDEN=1 in the environment does the same. Without one of
#             them the golden file is never written: a mismatch fails with a unified diff.
#
# For every executable scenario id of the draft (tests/golden/executable-ids-dDRAFT.txt, kept equal
# to the registry by the executable-ids-golden unit test), plus the ids run.sh special-cases, and for
# both transports, the adapter runs against a deterministic request with a stub MOQXR_BIN that
# prints its arguments. One line per pair: `<scenario_id> <transport> <arguments>`. Temporary
# paths are normalized to @TMP@. Needs only bash and jq; no network.
#
# Draft 22 also checks a relationship, not only the golden: every executable draft 22 id that the
# lineage table (include/moq/interop/requirements/draft22_lineage_data.h, kSharedScenarios) pairs
# with a draft 21 scenario must get exactly the draft 21 command line of that twin, except for the
# `--draft` value. The lineage twin must be the id with `d22-` replaced by `d21-` (run.sh relies on
# that), and every other executable id must be one of the own scenarios (kOwnScenarios22), whose
# command lines, like those of the two unscored probes, are pinned by the golden only.
#
# The only shared ids exempt from that equality are the draft 22 overrides enumerated in
# paced_overrides below (run.sh: "Draft 22 overrides of the draft 21 option lists"). The list must
# equal run.sh's d22_paced_overrides, and each of them must get exactly its twin's command line with
# `--forward 1 --timeout T` replaced by `--forward 0 --paced --timeout T+3` (and `--draft 22`).
set -euo pipefail
# A refused request must stop the script, also inside command_line's command substitution.
shopt -s inherit_errexit

draft=${1:-}
[[ "$draft" == 18 || "$draft" == 21 || "$draft" == 22 ]] ||
    { printf 'usage: %s 18|21|22 [--update]\n' "$0" >&2; exit 2; }
update=0
[[ "${2:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moqxr/run.sh"
ids_file="$root_dir/tests/golden/executable-ids-d$draft.txt"
golden="$root_dir/tests/golden/moqxr-cmdlines-d$draft.txt"
stub_source="$root_dir/tests/support/capture_publisher.sh"
lineage="$root_dir/include/moq/interop/requirements/draft22_lineage_data.h"

work=$(mktemp -d /tmp/moqxr-adapter-cmdlines.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
touch "$work/fixture.mp4" "$work/ca.pem"
ln -s "$stub_source" "$work/publisher"

# Ids run.sh special-cases by name; they are pinned even if a registry change drops them.
extra_ids=(publish-track-under-single-period-namespace
           application-publish-track-in-session-namespace
           publish-distinct-content-tracks-in-same-scope
           scenario-without-special-options)
# Draft 22's unscored probes are executable by id but not listed by executable_scenarios(22).
if [[ "$draft" == 22 ]]; then
    extra_ids+=(d22-location-filter-unknown-type d22-location-filter-absolute-origin)
fi

ids=$(sort -u < <(cat "$ids_file"; printf '%s\n' "${extra_ids[@]}"))

# The arguments run.sh passes the publisher for one draft, scenario id and transport.
command_line() {
    local line_draft=$1 id=$2 transport=$3 endpoint args
    if [[ "$transport" == webtransport ]]; then
        endpoint=https://127.0.0.1:4443/moq
    else
        endpoint=moqt://127.0.0.1:4443/moq
    fi
    jq -n --arg id "$id" --arg transport "$transport" --arg endpoint "$endpoint" \
        --arg fixture "$work/fixture.mp4" --arg ca "$work/ca.pem" --arg log_dir "$work" \
        --argjson draft "$line_draft" '{
            schema_version: 1, run_id: "run-1", scenario_id: $id,
            endpoint: $endpoint, draft: $draft, transport: $transport,
            namespace_hex: ["6d65646961"], track_name_hex: "766964655f31",
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: 2500, process_timeout_ms: 4000
        }' >"$work/request.json"
    args=$(MOQXR_BIN="$work/publisher" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" | tr '\n' ' ')
    args=${args//"$work"/@TMP@}
    printf '%s' "${args% }"
}

declare -A twin_of=()
declare -A own=()
if [[ "$draft" == 22 ]]; then
    # {"d22-...", "d21-..."} rows of kSharedScenarios, and the quoted ids of kOwnScenarios22.
    while read -r d22 d21; do
        [[ "$d21" == "d21-${d22#d22-}" ]] ||
            { printf 'lineage twin of %s is %s, not the d21- prefix swap\n' "$d22" "$d21" >&2; exit 1; }
        twin_of[$d22]=$d21
    done < <(sed -n '/kSharedScenarios{{/,/^}};/p' "$lineage" |
             sed -n 's/^ *{"\(d22-[^"]*\)", "\(d21-[^"]*\)"},$/\1 \2/p')
    while read -r id; do
        own[$id]=1
    done < <(sed -n '/kOwnScenarios22{{/,/^}};/p' "$lineage" | sed -n 's/^ *"\(d22-[^"]*\)",$/\1/p')
    ((${#twin_of[@]} > 0 && ${#own[@]} > 0)) ||
        { printf 'could not read the lineage table %s\n' "$lineage" >&2; exit 1; }
fi

# Shared draft 22 ids whose command line deliberately differs from the draft 21 twin's (moqxr
# blocks on its own PUBLISH with --forward 1; draft 21 is frozen). Kept equal to run.sh's list.
paced_overrides=(
    d22-subscribe-empty-namespace-field
    d22-subscribe-33-namespace-fields
    d22-subscribe-tracks-oversized-namespace
    d22-subscribe-oversized-full-track-name
    d22-request-undecodable-authorization-token
    d22-request-token-cache-overflow
    d22-request-alias-registration-with-default-zero-cache
    d22-fill-forbidden-nested-authorization
    d22-fill-forbidden-track-property-filter
    d22-fill-recursive-parameter
    d22-fill-invalid-group-order
    d22-unknown-unidirectional-stream-type
    d22-unknown-control-message
    d22-successful-subscribe-object-delivery
)
declare -A paced_override=()
if [[ "$draft" == 22 ]]; then
    adapter_overrides=$(sed -n '/^d22_paced_overrides=(/,/^)/p' "$adapter" |
                        sed -n 's/^ *\(d22-[a-z0-9-]*\)$/\1/p' | sort)
    if [[ "$adapter_overrides" != "$(printf '%s\n' "${paced_overrides[@]}" | sort)" ]]; then
        printf 'the draft 22 override list differs from d22_paced_overrides in %s:\n' "$adapter" >&2
        diff <(printf '%s\n' "${paced_overrides[@]}" | sort) <(printf '%s\n' "$adapter_overrides") >&2 || true
        exit 1
    fi
    for id in "${paced_overrides[@]}"; do
        [[ -n "${twin_of[$id]:-}" ]] ||
            { printf 'draft 22 override %s is not a shared scenario\n' "$id" >&2; exit 1; }
        grep -qxF -- "$id" "$ids_file" ||
            { printf 'draft 22 override %s is not executable\n' "$id" >&2; exit 1; }
        paced_override[$id]=1
    done
fi

live="$work/live.txt"
: >"$live"
shared_checked=0
overrides_checked=0
own_checked=0
while IFS= read -r id; do
    [[ -n "$id" ]] || continue
    if [[ "$draft" == 22 && -z "${twin_of[$id]:-}" && -z "${own[$id]:-}" ]] &&
        grep -qxF -- "$id" "$ids_file"; then
        printf 'executable draft 22 id %s is neither shared nor own in %s\n' "$id" "$lineage" >&2
        exit 1
    fi
    for transport in native_quic webtransport; do
        args=$(command_line "$draft" "$id" "$transport")
        if [[ "$draft" == 22 && -n "${twin_of[$id]:-}" ]]; then
            expected=$(command_line 21 "${twin_of[$id]}" "$transport")
            expected=${expected/"<--draft> <21>"/"<--draft> <22>"}
            if [[ -n "${paced_override[$id]:-}" ]]; then
                [[ "$expected" == *"<--forward> <1> <--timeout> <3>"* ]] ||
                    { printf 'draft 21 twin %s %s no longer runs --forward 1 --timeout 3: %s\n' \
                          "${twin_of[$id]}" "$transport" "$expected" >&2; exit 1; }
                expected=${expected/"<--forward> <1> <--timeout> <3>"/"<--forward> <0> <--paced> <--timeout> <6>"}
                overrides_checked=$((overrides_checked + 1))
            fi
            if [[ "$args" != "$expected" ]]; then
                printf 'draft 22 %s %s does not match its draft 21 twin %s:\n  got:  %s\n  want: %s\n' \
                    "$id" "$transport" "${twin_of[$id]}" "$args" "$expected" >&2
                exit 1
            fi
            shared_checked=$((shared_checked + 1))
        elif [[ "$draft" == 22 && "$id" == d22-* ]]; then
            own_checked=$((own_checked + 1))
        fi
        printf '%s %s %s\n' "$id" "$transport" "$args" >>"$live"
    done
done <<<"$ids"
if [[ "$draft" == 22 ]]; then
    if ((overrides_checked != 2 * ${#paced_overrides[@]})); then
        printf 'checked %s override lines, expected %s\n' "$overrides_checked" "$((2 * ${#paced_overrides[@]}))" >&2
        exit 1
    fi
    printf 'draft 22: %s shared command lines equal their draft 21 twin (%s of them the paced overrides); %s own or probe lines pinned\n' \
        "$shared_checked" "$overrides_checked" "$own_checked"
fi

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
