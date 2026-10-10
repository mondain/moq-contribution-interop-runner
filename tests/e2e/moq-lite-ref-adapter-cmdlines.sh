#!/usr/bin/env bash
# Pin the command line adapters/moq-lite-ref/run.sh runs for every executable moq-lite-06 scenario on both transports.
#
# Usage: moq-lite-ref-adapter-cmdlines.sh [--update]
#   --update  rewrite tests/golden/moq-lite-ref-cmdlines.txt instead of comparing (MOQ_UPDATE_GOLDEN=1 does the same).
#
# A stub `timeout` records its arguments and runs the command; a stub publisher records its own. One line per pair:
#   `<scenario_id> <transport> <timeout argv>`
# with temporary paths normalized to @TMP@. Needs only bash and jq; no network. The command line must not depend on the
# scenario, and must change with MOQ_LITE_REF_ARGS only.
set -euo pipefail
shopt -s inherit_errexit
export LC_ALL=C

update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moq-lite-ref/run.sh"
ids_file="$root_dir/tests/golden/executable-ids-d106.txt"
golden="$root_dir/tests/golden/moq-lite-ref-cmdlines.txt"

work=$(mktemp -d /tmp/moq-lite-ref-cmdlines.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/bin" "$work/log" "$work/record"

cat >"$work/bin/timeout" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
all=("$@")
while (($#)); do
    case "$1" in
        --foreground|--preserve-status) shift ;;
        -k|-s) shift 2 ;;
        *) break ;;
    esac
done
shift
printf '<%s>\n' "${all[@]}" >"$STUB_RECORD_DIR/timeout"
exec "$@"
STUB
cat >"$work/bin/lite-ref" <<'STUB'
#!/usr/bin/env bash
printf '<%s>\n' "$@" >"$STUB_RECORD_DIR/publisher"
STUB
chmod +x "$work/bin/timeout" "$work/bin/lite-ref"

id_count=$(grep -vc '^$' "$ids_file")
((id_count == 27)) || { printf 'expected 27 executable moq-lite-06 ids, found %s\n' "$id_count" >&2; exit 1; }

command_line() {
    local id=$1 transport=$2 extra=$3 endpoint
    if [[ "$transport" == webtransport ]]; then endpoint='https://127.0.0.1:4443/moq?token=l1d'
    else endpoint='moql://127.0.0.1:4443/moq?token=l1d'; fi
    jq -n --arg id "$id" --arg transport "$transport" --arg endpoint "$endpoint" --arg log_dir "$work/log" '{
            schema_version: 1, run_id: "run-1", scenario_id: $id, endpoint: $endpoint, draft: "moq-lite-06",
            transport: $transport, namespace_hex: ["696e7465726f702e68616e67"], track_name_hex: "302e6d3473",
            fixture: "", tls_ca: "", log_dir: $log_dir, scenario_timeout_ms: 2500, process_timeout_ms: 6000
        }' >"$work/request.json"
    rm -f -- "$work/record/"* "$work/log/"*
    env PATH="$work/bin:$PATH" MOQ_LITE_REF_BIN="$work/bin/lite-ref" MOQ_LITE_REF_ARGS="$extra" \
        STUB_RECORD_DIR="$work/record" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" >/dev/null 2>"$work/err" ||
        { printf 'adapter refused %s %s: %s\n' "$id" "$transport" "$(cat "$work/err")" >&2; return 1; }
    [[ -f "$work/record/timeout" && -f "$work/record/publisher" ]] ||
        { printf '%s %s: the publisher did not run under timeout\n' "$id" "$transport" >&2; return 1; }
    tr '\n' ' ' <"$work/record/timeout" | sed "s|$work|@TMP@|g; s| \$||"
    printf '\n'
}

lines=()
for transport in native_quic webtransport; do
    first=
    while IFS= read -r id; do
        [[ -n "$id" ]] || continue
        line=$(command_line "$id" "$transport" "")
        lines+=("$id $transport $line")
        if [[ -z "$first" ]]; then first=$line
        elif [[ "$line" != "$first" ]]; then printf 'the command line depends on the scenario (%s %s)\n' "$id" "$transport" >&2; exit 1; fi
    done < <(sort "$ids_file")
done
# The operator's flags reach the publisher, in order, and nothing else changes.
with_flags=$(command_line l06-datagram-size native_quic "--datagrams --frames-per-group 1 --defect datagram-oversize")
[[ "$with_flags" == *"<--datagrams> <--frames-per-group> <1> <--defect> <datagram-oversize>" ]] ||
    { printf 'operator flags are not passed through: %s\n' "$with_flags" >&2; exit 1; }
# A request the adapter must refuse runs nothing.
if env MOQ_LITE_REF_BIN="$work/bin/lite-ref" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=2 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" >/dev/null 2>&1; then
    printf 'the adapter accepted an unsupported contract version\n' >&2; exit 1
fi
# adapter.json has the same keys as the moq CLI adapter's, and the same fixture.
jq -e --slurpfile other "$root_dir/adapters/moq-lite/adapter.json" \
    '(keys == ($other[0] | keys)) and .supported_namespace_hex == $other[0].supported_namespace_hex
     and .supported_track_name_hex == $other[0].supported_track_name_hex' \
    "$root_dir/adapters/moq-lite-ref/adapter.json" >/dev/null ||
    { printf 'adapters/moq-lite-ref/adapter.json differs in shape or fixture from adapters/moq-lite/adapter.json\n' >&2; exit 1; }

actual=$(printf '%s\n' "${lines[@]}" | sort)
if ((update)); then
    printf '%s\n' "$actual" >"$golden"
    printf 'updated %s\n' "$golden"
    exit 0
fi
if ! diff -u "$golden" <(printf '%s\n' "$actual"); then
    printf 'moq-lite-ref command lines differ from %s (rerun with --update after review)\n' "$golden" >&2
    exit 1
fi
printf 'moq-lite-ref adapter command lines: ok (%s)\n' "${#lines[@]}"
