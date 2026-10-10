#!/usr/bin/env bash
# Exercise the argument handling and wiring of tests/e2e/moq-lite-matrix.sh and driven-moq-lite.sh
# without a runner or a publisher.
#
# Usage: moq-lite-matrix-plan.sh [--update]
#   --update  rewrite tests/golden/moq-lite-matrix-plan.txt from the live scripts instead of comparing
#             (MOQ_UPDATE_GOLDEN=1 does the same). Otherwise a mismatch fails with a unified diff.
#
# 1. The matrix --dry-run plan (runner invocation, one run request per executable moq-lite-06
#    scenario, one group run of row L06-4-4-MUST-027's scenarios, the audit; per transport) equals the
#    golden; the repository root shows as @ROOT@.
# 2. Every executable moq-lite-06 id (tests/golden/executable-ids-d106.txt) is requested alone exactly
#    once on each transport, and one last request holds exactly the scenarios of row L06-4-4-MUST-027
#    (requirements/moq-lite-06.json); all in driven mode, with draft "moq-lite-06" and the fixture of
#    adapters/moq-lite/adapter.json; the ports are 19231-19234 (the driven default 19235/19236) and
#    none collides with another script's fixed port.
# 3. --transport prints exactly that transport's section of the full plan, and the driven script's
#    own --dry-run prints the same lines when given the transport's ports.
# 4. The real (not dry) matrix, run with a stub runner (tests/support/stub_runner_moq_lite.sh), a stub
#    curl first on PATH (tests/support/stub_curl_moq_lite/curl), a stub audit CLI and stub publisher
#    binaries, starts the runner with exactly the planned arguments, posts exactly the planned
#    requests, runs the planned audit and reports one row per run.
# 5. Without MOQ_CLI_BIN, or with a missing runner, the matrix and the driven script skip (exit 77);
#    bad arguments exit 2.
set -euo pipefail
shopt -s inherit_errexit

update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
matrix="$root_dir/tests/e2e/moq-lite-matrix.sh"
driven="$root_dir/tests/e2e/driven-moq-lite.sh"
golden="$root_dir/tests/golden/moq-lite-matrix-plan.txt"
ids_d106="$root_dir/tests/golden/executable-ids-d106.txt"
adapter_json="$root_dir/adapters/moq-lite/adapter.json"

work=$(mktemp -d /tmp/moq-lite-matrix-plan.XXXXXX)
trap 'rm -rf -- "$work"' EXIT

fail() { printf 'moq-lite-matrix-plan: %s\n' "$*" >&2; exit 1; }
normalize() {
    sed -e "s#$work#@WORK@#g" -e "s#/tmp/moq-interop-driven\.[A-Za-z0-9]*#@TMP@#g" \
        -e "s#$root_dir#@ROOT@#g"
}

# 1. The full plan with placeholder binaries that do not exist.
fake_runner=/opt/moq/bin/moq-interop-runner
fake_cli=/opt/moq/bin/moq
MOQ_CLI_BIN=$fake_cli bash "$matrix" --dry-run "$fake_runner" | normalize >"$work/plan.txt"
if ((update)); then
    cp "$work/plan.txt" "$golden"
    printf 'updated %s\n' "$golden"
else
    [[ -f "$golden" ]] || fail "missing golden $golden (run with --update)"
    diff -u "$golden" "$work/plan.txt" || fail 'dry-run plan differs from the golden'
fi

# 2. Ids, fixture and ports.
fixture=$(jq -c '{namespace_hex: .supported_namespace_hex, name_hex: .supported_track_name_hex}' "$adapter_json")
for entry in "native-quic 19231" "webtransport 19233"; do
    read -r api_transport http_port <<<"$entry"
    bodies=$(grep "^request: POST http://127.0.0.1:$http_port/api/v1/runs " "$work/plan.txt" |
        sed -e 's/^request: POST [^ ]* //')
    [[ $(wc -l <<<"$bodies") -eq $(($(wc -l <"$ids_d106") + 1)) ]] ||
        fail "expected one request per id and the row 027 group run on $api_transport"
    diff -u "$ids_d106" <(jq -r 'select((.scenarios | length) == 1) | .scenarios[]' <<<"$bodies" | LC_ALL=C sort) ||
        fail "the $api_transport single requests are not exactly the executable moq-lite-06 ids"
    group=$(tail -n 1 <<<"$bodies" | jq -c '.scenarios | sort')
    [[ "$group" == "$(jq -c '[.requirements[] | select(.id == "L06-4-4-MUST-027") | .scenarios[]] | sort' \
        "$root_dir/requirements/moq-lite-06.json")" ]] ||
        fail "the last $api_transport request is not the row 027 group run: $group"
    jq -e --arg transport "$api_transport" --argjson fixture "$fixture" -s '
        all(.[]; .draft == "moq-lite-06" and .transport == $transport and .mode == "driven" and
            .timeout_ms == 30000 and .track == $fixture)' \
        <<<"$bodies" >/dev/null || fail "a $api_transport request has the wrong draft, mode, timeout or fixture"
done
grep -qF '<--port> <19231> ' "$work/plan.txt" && grep -qF '<--publisher-port-start> <19232> ' "$work/plan.txt" &&
    grep -qF '<--port> <19233> ' "$work/plan.txt" && grep -qF '<--publisher-port-start> <19234> ' "$work/plan.txt" ||
    fail 'runner ports are not 19231/19232 and 19233/19234'
[[ $(grep -c "^runner: MOQ_CLI_BIN=<$fake_cli> <$fake_runner> " "$work/plan.txt") -eq 2 ]] ||
    fail 'MOQ_CLI_BIN or the runner missing from the plan'
[[ $(grep -c '<--driver-executable> <@ROOT@/adapters/moq-lite/run.sh>' "$work/plan.txt") -eq 2 ]] ||
    fail 'driver executable is not the moq-lite adapter'
[[ $(grep -c '^audit: </opt/moq/bin/moq-interop-audit> <--draft> <moq-lite-06> <--database> <@TMP@/runs.sqlite3> ' \
    "$work/plan.txt") -eq 2 ]] || fail 'the audit of the run database is missing from the plan'
default_ports=$(MOQ_CLI_BIN=$fake_cli bash "$driven" --dry-run native_quic "$fake_runner" l06-setup-stream)
grep -qF '<--port> <19235> ' <<<"$default_ports" && grep -qF '<--publisher-port-start> <19236> ' <<<"$default_ports" ||
    fail 'driven default ports are not 19235/19236'
# No other fixed port in the repository's scripts or CMake file uses 19231-19236.
others=$(grep -nE '1923[1-6]' "$root_dir"/tests/e2e/*.sh "$root_dir/CMakeLists.txt" |
    grep -vE 'tests/e2e/(moq-lite-matrix|driven-moq-lite|moq-lite-matrix-plan)\.sh' || true)
[[ -z "$others" ]] || fail "port collision: $others"

# 3. --transport and the driven script's own dry run.
pair_ports=("native_quic 19231 19232" "webtransport 19233 19234")
for entry in "${pair_ports[@]}"; do
    read -r transport http_port udp_port <<<"$entry"
    section=$(awk -v head="matrix draft=moq-lite-06 transport=$transport" '
        $0 == head {on = 1; print; next} /^matrix / {on = 0} on' "$work/plan.txt")
    [[ $(wc -l <<<"$section") -eq $(($(wc -l <"$ids_d106") + 4)) ]] || fail "no plan section for $transport"
    mapfile -t runs < <(grep '^request: ' <<<"$section" | sed -e 's/^request: POST [^ ]* //' |
        jq -r '.scenarios | join(",")')
    expected=$(head -n 1 "$work/plan.txt"; printf '%s\n' "$section"; tail -n 1 "$work/plan.txt")
    actual=$(MOQ_CLI_BIN=$fake_cli bash "$matrix" --dry-run --transport "$transport" "$fake_runner" | normalize)
    [[ "$actual" == "$expected" ]] || fail "--transport $transport differs from its plan section"
    actual=$(MOQ_CLI_BIN=$fake_cli MOQ_INTEROP_TEST_HTTP_PORT=$http_port MOQ_INTEROP_TEST_UDP_PORT=$udp_port \
        bash "$driven" --dry-run "$transport" "$fake_runner" "${runs[@]}" | normalize)
    [[ "$actual" == "$(tail -n +2 <<<"$section")" ]] ||
        fail "driven --dry-run $transport differs from its plan section"
done
# A scenario subset keeps its order.
subset=$(MOQ_CLI_BIN=$fake_cli bash "$driven" --dry-run webtransport "$fake_runner" \
    l06-subscribe-latest l06-setup-stream | grep '^request: ' | sed -e 's/^request: POST [^ ]* //' |
    jq -r '.scenarios[0]' | paste -sd ' ')
[[ "$subset" == 'l06-subscribe-latest l06-setup-stream' ]] || fail "scenario subset: $subset"

# 4. The real matrix against stubs does what its plan says.
mkdir -p "$work/bin" "$work/stub"
ln -s "$root_dir/tests/support/stub_runner_moq_lite.sh" "$work/bin/runner"
cat >"$work/bin/moq-interop-audit" <<'STUB'
#!/usr/bin/env bash
{
    printf 'audit: <%s>' "$0"
    printf ' <%s>' "$@"
    printf '\n'
} >>"$MOQ_STUB_DIR/calls.log"
# The audit of a run database (--database): the staged audit and the execution audit of the runs.
[[ " $* " == *' --database '* ]] || { printf 'stub audit: no --database\n' >&2; exit 2; }
printf 'Execution audit: consistent (19 runs, 19 scored rows, 0 findings)\n'
printf 'STAGED: incomplete catalog (not a pass)\n'
STUB
# The stub publisher binaries only have to exist and be executable: the matrix's adapter contract
# brings its own stubs, and the stub runner starts no driver.
printf '#!/bin/sh\nexit 0\n' >"$work/bin/moq"
printf '#!/bin/sh\nexit 0\n' >"$work/bin/ffmpeg"
chmod +x "$work/bin/moq-interop-audit" "$work/bin/moq" "$work/bin/ffmpeg"
MOQ_CLI_BIN="$work/bin/moq" bash "$matrix" --dry-run "$work/bin/runner" | normalize |
    grep -E '^(runner|request|audit): ' >"$work/stub-plan.txt"
MOQ_STUB_DIR="$work/stub" MOQ_CLI_BIN="$work/bin/moq" MOQ_FFMPEG_BIN="$work/bin/ffmpeg" \
    PATH="$root_dir/tests/support/stub_curl_moq_lite:$PATH" \
    bash "$matrix" "$work/bin/runner" >"$work/stub-out.txt" 2>"$work/stub-err.txt" ||
    fail "matrix against stubs exited $?: $(cat "$work/stub-out.txt" "$work/stub-err.txt")"
normalize <"$work/stub/calls.log" >"$work/stub-calls.txt"
diff -u "$work/stub-plan.txt" "$work/stub-calls.txt" || fail 'stub run differs from its dry-run plan'
for entry in "native_quic 19231" "webtransport 19233"; do
    read -r transport http_port <<<"$entry"
    count=0
    while IFS= read -r scenario; do
        count=$((count + 1))
        grep -qxF "scenario=$scenario transport=$transport run=run-$http_port-$count verdict=incomplete publisher=stopped pass=1 fail=0 not_run=1" \
            "$work/stub-out.txt" || fail "no reported row for $scenario on $transport"
    done < <(grep "^request: POST http://127.0.0.1:$http_port/" "$work/stub-plan.txt" |
        sed -e 's/^request: POST [^ ]* //' | jq -r '.scenarios | join(",")')
done
[[ $(grep -cxF '  row L06-STUB-001 pass' "$work/stub-out.txt") -eq $((2 * ($(wc -l <"$ids_d106") + 1))) ]] ||
    fail 'judged rows are not reported per run'
[[ $(grep -cxF 'audit status=0' "$work/stub-out.txt") -eq 2 ]] || fail 'audit status not reported per transport'
[[ $(grep -cxF 'Execution audit: consistent (19 runs, 19 scored rows, 0 findings)' "$work/stub-out.txt") -eq 2 ]] ||
    fail 'the execution audit of each run database is not reported'
grep -q 'execution audit: unavailable' "$work/stub-out.txt" && fail 'the script still reports the old execution audit fallback'
# A comma-joined SCENARIO is one group run of those scenarios, in order.
group=$(MOQ_CLI_BIN=$fake_cli bash "$driven" --dry-run native_quic "$fake_runner" \
    l06-setup-stream,l06-setup-server-role l06-subscribe-latest | grep '^request: ' |
    sed -e 's/^request: POST [^ ]* //' | jq -c '.scenarios')
[[ "$group" == $'["l06-setup-stream","l06-setup-server-role"]\n["l06-subscribe-latest"]' ]] || fail "group run: $group"
expect_group_refusal() {
    local status=0
    MOQ_CLI_BIN=$fake_cli bash "$driven" --dry-run native_quic "$fake_runner" "$1" >/dev/null 2>&1 || status=$?
    [[ $status -eq 2 ]] || fail "group '$1' not refused (status $status)"
}
expect_group_refusal l06-setup-stream,l06-nope
expect_group_refusal l06-setup-stream,
expect_group_refusal ,
[[ $(tail -n 1 "$work/stub-out.txt") == \
    'matrix harness completed; publisher exits, judged rows and the audit are reported above' ]] ||
    fail 'matrix did not report completion'

# 5. Skips and bad arguments.
expect_status() {
    local expected=$1 status=0
    shift
    "$@" >/dev/null 2>&1 || status=$?
    [[ $status -eq $expected ]] || fail "expected exit $expected from: $* (got $status)"
}
expect_status 77 env -u MOQ_CLI_BIN bash "$matrix" "$work/bin/runner"
expect_status 77 env -u MOQ_CLI_BIN bash "$driven" native_quic "$work/bin/runner"
expect_status 77 env MOQ_CLI_BIN="$work/bin/moq" bash "$driven" native_quic "$work/no-runner"
expect_status 77 env MOQ_CLI_BIN="$work/bin/moq" MOQ_FFMPEG_BIN="$work/no-ffmpeg" \
    bash "$driven" native_quic "$work/bin/runner"
expect_status 2 bash "$driven" --dry-run quic "$fake_runner"
expect_status 2 bash "$driven" --dry-run native_quic
expect_status 2 bash "$driven" --dry-run native_quic "$fake_runner" l06-no-such-scenario
expect_status 2 bash "$matrix" --dry-run --transport h3 "$fake_runner"
expect_status 2 bash "$matrix" --dry-run --transport
expect_status 2 bash "$matrix" --dry-run "$fake_runner" extra

printf 'moq-lite matrix plan passed\n'
