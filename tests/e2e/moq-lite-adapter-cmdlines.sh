#!/usr/bin/env bash
# Pin the command lines adapters/moq-lite/run.sh runs: the pipeline
#   timeout ... ffmpeg <testsrc2 -> fMP4 options> - | timeout ... MOQ_CLI_BIN <dial options> import fmp4
#
# Usage: moq-lite-adapter-cmdlines.sh [--update]
#   --update  rewrite tests/golden/moq-lite-cmdlines.txt from the live adapter instead of comparing.
#             MOQ_UPDATE_GOLDEN=1 in the environment does the same. Without one of them the golden
#             file is never written: a mismatch fails with a unified diff.
#
# For every executable moq-lite-06 scenario id (tests/golden/executable-ids-d106.txt, which
# tests/unit/executable_ids_golden_test.cpp keeps equal to the registry; this script also compares it
# with kLiteExecutableScenarios in include/moq/interop/app/lite_scenarios.h) on both transports, the
# adapter runs against a deterministic request with stub `timeout`, `ffmpeg` and `moq` binaries first
# on PATH. The stub `timeout` records its arguments and then runs the command it was given, so the
# stub ffmpeg and moq run as in a real pipeline and record their own arguments too (checked equal to
# what `timeout` was told). One line per pair:
#   `<scenario_id> <transport> <ffmpeg timeout argv> | <moq timeout argv>`
# with temporary paths normalized to @TMP@. Needs only bash and jq; no network.
#
# The adapter has no per-scenario table: the moq CLI's command line does not depend on the scenario
# (the scenarios differ only in what the runner does), so this script also checks that all ids give
# the same command line on a transport.
set -euo pipefail
# A refused request must stop the script, also inside command_line's command substitution.
shopt -s inherit_errexit
# Byte-order sorting: the golden must not depend on locale.
export LC_ALL=C

update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moq-lite/run.sh"
ids_file="$root_dir/tests/golden/executable-ids-d106.txt"
golden="$root_dir/tests/golden/moq-lite-cmdlines.txt"
header="$root_dir/include/moq/interop/app/lite_scenarios.h"

work=$(mktemp -d /tmp/moq-lite-adapter-cmdlines.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/bin" "$work/log" "$work/record"
touch "$work/fixture.mp4"

# The stub `timeout`: records its arguments in record/timeout-<command name>, skips its own options
# and the duration, and runs the command.
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
printf '<%s>\n' "${all[@]}" >"$STUB_RECORD_DIR/timeout-${1##*/}"
exec "$@"
STUB
# The stub ffmpeg: records its arguments and writes a few bytes to the pipe.
cat >"$work/bin/ffmpeg" <<'STUB'
#!/usr/bin/env bash
printf '<%s>\n' "$@" >"$STUB_RECORD_DIR/ffmpeg"
printf 'fmp4 bytes\n'
STUB
# The stub moq: answers the adapter's --version and --help checks like moq 0.14.1, otherwise records
# its arguments and reads standard input to its end, as `moq ... import fmp4` does.
cat >"$work/bin/moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf 'Usage: moq [FLAGS] <SUBCOMMAND>\n      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
printf '<%s>\n' "$@" >"$STUB_RECORD_DIR/moq"
cat >"$STUB_RECORD_DIR/moq-stdin"
STUB
chmod +x "$work/bin/timeout" "$work/bin/ffmpeg" "$work/bin/moq"

# The executable ids: the golden id list must equal the C++ list (in any order).
from_header=$(sed -n '/kLiteExecutableScenarios = std::to_array/,/^});/p' "$header" |
              sed -n 's/^ *{"\(l06-[a-z0-9-]*\)", \(true\|false\)},$/\1/p' | sort)
from_golden=$(grep -v '^$' "$ids_file" | sort)
if [[ -z "$from_header" || "$from_header" != "$from_golden" ]]; then
    printf 'the ids in %s differ from kLiteExecutableScenarios in %s:\n' "$ids_file" "$header" >&2
    diff <(printf '%s\n' "$from_golden") <(printf '%s\n' "$from_header") >&2 || true
    exit 1
fi
id_count=$(wc -l <<<"$from_golden")
((id_count == 19)) || { printf 'expected 19 executable moq-lite-06 ids, found %s\n' "$id_count" >&2; exit 1; }

# The two recorded `timeout` command lines for one scenario id and transport, `<ffmpeg> | <moq>`.
command_line() {
    local id=$1 transport=$2 endpoint ffmpeg_line moq_line
    if [[ "$transport" == webtransport ]]; then
        endpoint='https://127.0.0.1:4443/moq?token=l1d'
    else
        endpoint='moql://127.0.0.1:4443/moq?token=l1d'
    fi
    jq -n --arg id "$id" --arg transport "$transport" --arg endpoint "$endpoint" \
        --arg fixture "$work/fixture.mp4" --arg log_dir "$work/log" '{
            schema_version: 1, run_id: "run-1", scenario_id: $id,
            endpoint: $endpoint, draft: "moq-lite-06", transport: $transport,
            namespace_hex: ["696e7465726f702e68616e67"], track_name_hex: "302e6d3473",
            fixture: $fixture, tls_ca: "", log_dir: $log_dir,
            scenario_timeout_ms: 2500, process_timeout_ms: 6000
        }' >"$work/request.json"
    rm -f -- "$work/record/"* "$work/log/"*
    env -u MOQ_FFMPEG_BIN -u MOQ_LITE_TLS_INSECURE PATH="$work/bin:$PATH" MOQ_CLI_BIN="$work/bin/moq" \
        STUB_RECORD_DIR="$work/record" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$work/request.json" "$adapter" >/dev/null 2>"$work/err" ||
        { printf 'adapter refused %s %s: %s\n' "$id" "$transport" "$(cat "$work/err")" >&2; return 1; }
    for name in ffmpeg moq; do
        [[ -f "$work/record/timeout-$name" && -f "$work/record/$name" ]] ||
            { printf '%s %s: %s did not run under timeout\n' "$id" "$transport" "$name" >&2; return 1; }
        # What the stub received equals the command `timeout` was given (its last arguments).
        local tail
        tail=$(tail -n "$(wc -l <"$work/record/$name")" "$work/record/timeout-$name")
        [[ "$tail" == "$(cat "$work/record/$name")" ]] ||
            { printf '%s %s: %s received other arguments than timeout ran\n' "$id" "$transport" "$name" >&2; return 1; }
    done
    [[ "$(cat "$work/record/moq-stdin")" == 'fmp4 bytes' ]] ||
        { printf '%s %s: ffmpeg output did not reach moq standard input\n' "$id" "$transport" >&2; return 1; }
    ffmpeg_line=$(tr '\n' ' ' <"$work/record/timeout-ffmpeg")
    moq_line=$(tr '\n' ' ' <"$work/record/timeout-moq")
    local line="${ffmpeg_line% } | ${moq_line% }"
    printf '%s' "${line//"$work"/@TMP@}"
}

live="$work/live.txt"
: >"$live"
declare -A first_line=()
while IFS= read -r id; do
    for transport in native_quic webtransport; do
        args=$(command_line "$id" "$transport")
        if [[ -z "${first_line[$transport]:-}" ]]; then
            first_line[$transport]=$args
        elif [[ "$args" != "${first_line[$transport]}" ]]; then
            printf '%s %s has a command line of its own; the moq-lite adapter has no per-scenario options:\n  got:  %s\n  want: %s\n' \
                "$id" "$transport" "$args" "${first_line[$transport]}" >&2
            exit 1
        fi
        printf '%s %s %s\n' "$id" "$transport" "$args" >>"$live"
    done
done <<<"$from_golden"
printf 'moq-lite-06: %s ids on two transports, one command line per transport\n' "$id_count"

if ((update)); then
    cp -- "$live" "$golden"
    printf 'updated %s (%s lines)\n' "$golden" "$(wc -l <"$golden")"
    exit 0
fi
[[ -f "$golden" ]] || { printf 'missing golden %s (run with --update)\n' "$golden" >&2; exit 1; }
if ! diff -u "$golden" "$live"; then
    printf 'moq-lite adapter command lines differ from %s\n' "$golden" >&2
    exit 1
fi
printf 'moq-lite adapter command lines match (%s lines)\n' "$(wc -l <"$golden")"
