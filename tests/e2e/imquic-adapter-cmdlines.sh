#!/usr/bin/env bash
# Pin the command line adapters/imquic/run.sh execs: `timeout ... IMQUIC_PUB_BIN <moq-pub options>`.
#
# Usage: imquic-adapter-cmdlines.sh [--update]
#   --update  rewrite tests/golden/imquic-cmdlines-d22.txt from the live adapter instead of
#             comparing. MOQ_UPDATE_GOLDEN=1 in the environment does the same. Without one of
#             them the golden file is never written: a mismatch fails with a unified diff.
#
# For every executable draft 22 scenario id (tests/golden/executable-ids-d22.txt), the two unscored
# probes and one id no table names, on both transports, the adapter runs against a deterministic
# request with a stub `timeout` first on PATH that prints its arguments (the real publisher is never
# started). One line per pair: `<scenario_id> <transport> <arguments>`, temporary paths normalized
# to @TMP@. Needs only bash and jq; no network.
#
# Besides the golden, three relationships are checked:
#
# 1. Twin parity. run.sh keys its tables on the normalized id (d22-X -> d21-X), so every shared draft
#    22 id (kSharedScenarios in include/moq/interop/requirements/draft22_lineage_data.h) gets exactly
#    the command line of a draft 22 request naming its d21- twin, except for the ids in run.sh's three
#    d22- keyed override lists, d22_moqxr_paced_overrides, d22_announce_overrides and
#    d22_publish_overrides (copied below and compared with run.sh): for those the d22- id flips -X
#    relative to the twin (drops it, or adds it for d22_publish_overrides) and nothing else changes.
# 2. moqxr derivation. The publish-first versus announce-and-wait choice is derived from the moqxr
#    adapter: an id whose moqxr draft 22 command line (tests/golden/moqxr-cmdlines-d22.txt) has
#    `--forward 1` gets -X, one with `--forward 0` gets none, except the ids of d22_announce_overrides
#    (moqxr `--forward 1`, imquic without -X) and d22_publish_overrides (moqxr `--forward 0`, imquic
#    with -X); see run.sh for the reasons.
# 3. Explicit entries. The own draft 22 scenarios (kOwnScenarios22) and the two probes are exactly
#    the ids of run.sh's own22_publish_first and own22_announce lists.
set -euo pipefail
# A refused request must stop the script, also inside command_line's command substitution.
shopt -s inherit_errexit

update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/imquic/run.sh"
ids_file="$root_dir/tests/golden/executable-ids-d22.txt"
golden="$root_dir/tests/golden/imquic-cmdlines-d22.txt"
moqxr_golden="$root_dir/tests/golden/moqxr-cmdlines-d22.txt"
lineage="$root_dir/include/moq/interop/requirements/draft22_lineage_data.h"

work=$(mktemp -d /tmp/imquic-adapter-cmdlines.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/bin" "$work/log"
touch "$work/fixture.mp4" "$work/ca.pem" "$work/publisher"
chmod +x "$work/publisher"
# The stub `timeout` prints its arguments, which include the publisher and its options.
cat >"$work/bin/timeout" <<'STUB'
#!/usr/bin/env bash
printf '<%s>\n' "$@"
STUB
chmod +x "$work/bin/timeout"

probes=(d22-location-filter-unknown-type d22-location-filter-absolute-origin)
# An id no table names takes the fallback (publish-first, as moqxr's --forward 1 default).
extra_ids=("${probes[@]}" scenario-without-special-options)
ids=$(sort -u < <(cat "$ids_file"; printf '%s\n' "${extra_ids[@]}"))

# The arguments run.sh passes `timeout` for one scenario id and transport.
command_line() {
    local id=$1 transport=$2 endpoint args
    if [[ "$transport" == webtransport ]]; then
        endpoint=https://127.0.0.1:4443/moq
    else
        endpoint=moqt://127.0.0.1:4443/moq
    fi
    jq -n --arg id "$id" --arg transport "$transport" --arg endpoint "$endpoint" \
        --arg fixture "$work/fixture.mp4" --arg ca "$work/ca.pem" --arg log_dir "$work/log" '{
            schema_version: 1, run_id: "run-1", scenario_id: $id,
            endpoint: $endpoint, draft: 22, transport: $transport,
            namespace_hex: ["6d65646961"], track_name_hex: "766964655f31",
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: 2500, process_timeout_ms: 3500
        }' >"$work/request.json"
    rm -f -- "$work/log/publisher.log"
    PATH="$work/bin:$PATH" IMQUIC_PUB_BIN="$work/publisher" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" >/dev/null
    args=$(tr '\n' ' ' <"$work/log/publisher.log")
    args=${args//"$work"/@TMP@}
    printf '%s' "${args% }"
}

# The entries of a bash array NAME=( ... ) in run.sh, one d22- id per line, sorted.
adapter_list() {
    sed -n "/^$1=(/,/^)/p" "$adapter" | sed -n 's/^ *\(d22-[a-z0-9-]*\)$/\1/p' | sort
}
# Fails unless run.sh's list NAME equals the ids given after it.
same_list() {
    local name=$1
    shift
    local from_adapter
    from_adapter=$(adapter_list "$name")
    if [[ -z "$from_adapter" || "$from_adapter" != "$(printf '%s\n' "$@" | sort)" ]]; then
        printf 'the list %s in %s differs from this test:\n' "$name" "$adapter" >&2
        diff <(printf '%s\n' "$@" | sort) <(printf '%s\n' "$from_adapter") >&2 || true
        exit 1
    fi
}

# {"d22-...", "d21-..."} rows of kSharedScenarios, and the quoted ids of kOwnScenarios22.
declare -A twin_of=()
declare -A own=()
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

# Shared ids whose command line differs from the twin's, kept equal to run.sh's lists.
# The moqxr adapter's draft 22 paced overrides (moqxr runs them --forward 0 --paced at draft 22 only).
moqxr_paced_overrides=(
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
# imquic's own overrides: moqxr runs them --forward 1, imquic must announce and wait (run.sh).
announce_overrides=(
    d22-complete-subgroup-fin
    d22-subgroup-start-location-fin
    d22-object-datagram-flags
    d22-original-publisher-opens-new-subgroup
    d22-publish-track-with-mandatory-property
    d22-subscribe-single-subgroup
    d22-subscribe-accepted
    d22-request-update-overrun
    d22-request-update-independent-streams
    d22-publish-namespace-redirect-nonempty-track-name
    d22-publisher-namespace-routing-announcement
)
# imquic's reverse override: moqxr runs it --forward 0, imquic must publish first (run.sh).
publish_overrides=(
    d22-publish-update-ok-with-track-properties
)
same_list d22_moqxr_paced_overrides "${moqxr_paced_overrides[@]}"
same_list d22_announce_overrides "${announce_overrides[@]}"
same_list d22_publish_overrides "${publish_overrides[@]}"
# The moqxr list is a copy: it must still equal the moqxr adapter's own.
moqxr_list=$(sed -n '/^d22_paced_overrides=(/,/^)/p' "$root_dir/adapters/moqxr/run.sh" |
             sed -n 's/^ *\(d22-[a-z0-9-]*\)$/\1/p' | sort)
[[ "$moqxr_list" == "$(adapter_list d22_moqxr_paced_overrides)" ]] ||
    { printf 'd22_moqxr_paced_overrides no longer equals d22_paced_overrides in adapters/moqxr/run.sh\n' >&2; exit 1; }
declare -A override=()
declare -A announce_override=()
declare -A publish_override=()
for id in "${moqxr_paced_overrides[@]}" "${announce_overrides[@]}" "${publish_overrides[@]}"; do
    [[ -n "${twin_of[$id]:-}" ]] || { printf 'override %s is not a shared scenario\n' "$id" >&2; exit 1; }
    grep -qxF -- "$id" "$ids_file" || { printf 'override %s is not executable\n' "$id" >&2; exit 1; }
    [[ -z "${override[$id]:-}" ]] || { printf 'override %s is listed twice\n' "$id" >&2; exit 1; }
    override[$id]=1
done
for id in "${announce_overrides[@]}"; do
    announce_override[$id]=1
done
for id in "${publish_overrides[@]}"; do
    publish_override[$id]=1
done

# Explicit entries for the own scenarios and the probes, and nothing else, in the own tables.
expected_own=$( (printf '%s\n' "${!own[@]}"; printf '%s\n' "${probes[@]}") | sort)
listed_own=$( (adapter_list own22_publish_first; adapter_list own22_announce) | sort)
if [[ "$listed_own" != "$expected_own" ]]; then
    printf 'own22_publish_first and own22_announce in %s are not exactly the own scenarios and probes:\n' \
        "$adapter" >&2
    diff <(printf '%s\n' "$expected_own") <(printf '%s\n' "$listed_own") >&2 || true
    exit 1
fi
[[ -z "$(comm -12 <(adapter_list own22_publish_first) <(adapter_list own22_announce))" ]] ||
    { printf 'an own id is both publish-first and announce-and-wait\n' >&2; exit 1; }

# moqxr's draft 22 --forward value per "<id> <transport>".
declare -A moqxr_forward=()
while read -r id transport rest; do
    case "$rest" in
        *"<--forward> <1>"*) moqxr_forward["$id $transport"]=1 ;;
        *"<--forward> <0>"*) moqxr_forward["$id $transport"]=0 ;;
    esac
done <"$moqxr_golden"

live="$work/live.txt"
: >"$live"
shared_checked=0
overrides_checked=0
own_checked=0
derived_checked=0
while IFS= read -r id; do
    [[ -n "$id" ]] || continue
    if [[ -z "${twin_of[$id]:-}" && -z "${own[$id]:-}" ]] && grep -qxF -- "$id" "$ids_file"; then
        printf 'executable draft 22 id %s is neither shared nor own in %s\n' "$id" "$lineage" >&2
        exit 1
    fi
    for transport in native_quic webtransport; do
        args=$(command_line "$id" "$transport")
        publishes=0
        [[ " $args " == *" <-X> "* ]] && publishes=1
        if [[ -n "${twin_of[$id]:-}" ]]; then
            expected=$(command_line "${twin_of[$id]}" "$transport")
            compared=$args
            if [[ -n "${override[$id]:-}" ]]; then
                # The override flips -X relative to the twin and changes nothing else.
                twin_publishes=0
                [[ " $expected " == *" <-X> "* ]] && twin_publishes=1
                want_twin=1
                [[ -n "${publish_override[$id]:-}" ]] && want_twin=0
                [[ "$twin_publishes" == "$want_twin" && "$publishes" != "$want_twin" ]] ||
                    { printf 'override %s %s: twin %s has -X=%s, the d22- id -X=%s\n' "$id" "$transport" \
                          "${twin_of[$id]}" "$twin_publishes" "$publishes" >&2; exit 1; }
                expected=${expected/" <-X>"/}
                compared=${args/" <-X>"/}
                overrides_checked=$((overrides_checked + 1))
            fi
            if [[ "$compared" != "$expected" ]]; then
                printf 'draft 22 %s %s does not match its twin %s:\n  got:  %s\n  want: %s\n' \
                    "$id" "$transport" "${twin_of[$id]}" "$args" "$expected" >&2
                exit 1
            fi
            shared_checked=$((shared_checked + 1))
        elif [[ "$id" == d22-* ]]; then
            own_checked=$((own_checked + 1))
        fi
        forward=${moqxr_forward["$id $transport"]:-}
        if [[ -n "$forward" ]]; then
            want=$forward
            if [[ -n "${announce_override[$id]:-}" ]]; then
                [[ "$forward" == 1 ]] ||
                    { printf 'announce override %s %s: moqxr no longer runs --forward 1\n' "$id" "$transport" >&2; exit 1; }
                want=0
            elif [[ -n "${publish_override[$id]:-}" ]]; then
                [[ "$forward" == 0 ]] ||
                    { printf 'publish override %s %s: moqxr no longer runs --forward 0\n' "$id" "$transport" >&2; exit 1; }
                want=1
            fi
            if [[ "$publishes" != "$want" ]]; then
                printf '%s %s: -X is %s, but moqxr --forward %s maps to %s\n' \
                    "$id" "$transport" "$publishes" "$forward" "$want" >&2
                exit 1
            fi
            derived_checked=$((derived_checked + 1))
        elif [[ -n "${twin_of[$id]:-}" ]]; then
            printf 'shared id %s %s has no moqxr draft 22 command line\n' "$id" "$transport" >&2
            exit 1
        fi
        printf '%s %s %s\n' "$id" "$transport" "$args" >>"$live"
    done
done <<<"$ids"
if ((overrides_checked != 2 * ${#override[@]})); then
    printf 'checked %s override lines, expected %s\n' "$overrides_checked" "$((2 * ${#override[@]}))" >&2
    exit 1
fi
printf 'imquic draft 22: %s shared command lines equal their twin (%s of them overrides); %s own or probe lines pinned; %s modes match the moqxr derivation\n' \
    "$shared_checked" "$overrides_checked" "$own_checked" "$derived_checked"

if ((update)); then
    cp -- "$live" "$golden"
    printf 'updated %s (%s lines)\n' "$golden" "$(wc -l <"$golden")"
    exit 0
fi
[[ -f "$golden" ]] || { printf 'missing golden %s (run with --update)\n' "$golden" >&2; exit 1; }
if ! diff -u "$golden" "$live"; then
    printf 'imquic adapter command lines for draft 22 differ from %s\n' "$golden" >&2
    exit 1
fi
printf 'imquic adapter command lines for draft 22 match (%s lines)\n' "$(wc -l <"$golden")"
