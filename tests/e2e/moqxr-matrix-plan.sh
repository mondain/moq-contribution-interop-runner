#!/usr/bin/env bash
# Exercise the argument handling and wiring of tests/e2e/moqxr-matrix.sh and driven-moqxr.sh
# without a runner or a publisher.
#
# Usage: moqxr-matrix-plan.sh [--update]
#   --update  rewrite tests/golden/moqxr-matrix-plan.txt from the live scripts instead of comparing
#             (MOQ_UPDATE_GOLDEN=1 does the same). Otherwise a mismatch fails with a unified diff.
#
# 1. The matrix --dry-run plan (runner invocation and run request per draft and transport, drafts
#    18, 21 and 22) equals the golden; the repository root shows as @ROOT@.
# 2. Every draft 22 scenario in the plan is an executable draft 22 id
#    (tests/golden/executable-ids-d22.txt) and the draft 22 pairs use ports 19209-19212.
# 3. --pair prints exactly that pair's section of the full plan, and the driven script's own
#    --dry-run prints the same lines when given the pair's ports.
# 4. The real (not dry) matrix, run with a stub runner (tests/support/stub_runner.sh) and a stub
#    curl first on PATH (tests/support/stub_curl/curl), starts the runner with exactly the planned
#    arguments, posts exactly the planned requests and reports one row per pair: what the plan says
#    is what a run does.
# 5. Bad arguments exit 2.
set -euo pipefail
shopt -s inherit_errexit

update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
matrix="$root_dir/tests/e2e/moqxr-matrix.sh"
driven="$root_dir/tests/e2e/driven-moqxr.sh"
golden="$root_dir/tests/golden/moqxr-matrix-plan.txt"
ids_d22="$root_dir/tests/golden/executable-ids-d22.txt"

work=$(mktemp -d /tmp/moqxr-matrix-plan.XXXXXX)
trap 'rm -rf -- "$work"' EXIT

fail() { printf 'moqxr-matrix-plan: %s\n' "$*" >&2; exit 1; }
normalize() {
    sed -e "s#$work#@WORK@#g" -e "s#/tmp/moq-interop-driven\.[A-Za-z0-9]*#@TMP@#g" \
        -e "s#$root_dir#@ROOT@#g"
}

# 1. The full plan with placeholder binaries that do not exist.
fake=(/opt/moq/bin/moq-interop /opt/moq/bin/moqxr /opt/moq/fixture.mp4)
bash "$matrix" --dry-run "${fake[@]}" | normalize >"$work/plan.txt"
if ((update)); then
    cp "$work/plan.txt" "$golden"
    printf 'updated %s\n' "$golden"
else
    [[ -f "$golden" ]] || fail "missing golden $golden (run with --update)"
    diff -u "$golden" "$work/plan.txt" || fail 'dry-run plan differs from the golden'
fi

# 2. Draft 22 ids and ports.
d22_requests=$(grep '^request: .*"draft":22' "$work/plan.txt" || true)
[[ $(wc -l <<<"$d22_requests") -eq 2 ]] || fail 'expected two draft 22 requests'
while IFS= read -r scenario; do
    grep -qxF "$scenario" "$ids_d22" || fail "draft 22 scenario $scenario is not executable"
done < <(sed -e 's/^request: POST [^ ]* //' <<<"$d22_requests" | jq -r '.scenarios[]')
for expected in '19209/api/v1/runs {"draft":22,"transport":"native-quic"' \
                '19211/api/v1/runs {"draft":22,"transport":"webtransport"'; do
    grep -qF "$expected" <<<"$d22_requests" || fail "no draft 22 request on $expected"
done
grep -qF '<--port> <19209> ' "$work/plan.txt" && grep -qF '<--publisher-port-start> <19210> ' "$work/plan.txt" &&
    grep -qF '<--port> <19211> ' "$work/plan.txt" && grep -qF '<--publisher-port-start> <19212> ' "$work/plan.txt" ||
    fail 'draft 22 runner ports are not 19209/19210 and 19211/19212'

# 3. --pair and the driven script's own dry run.
pair_ports=("18 native_quic 19201 19202" "18 webtransport 19205 19206"
            "21 native_quic 19203 19204" "21 webtransport 19207 19208"
            "22 native_quic 19209 19210" "22 webtransport 19211 19212")
for entry in "${pair_ports[@]}"; do
    read -r draft transport http_port udp_port <<<"$entry"
    section=$(awk -v head="matrix draft=$draft transport=$transport" '
        $0 == head {on = 1; print; next} /^matrix / {on = 0} on' "$work/plan.txt")
    [[ $(wc -l <<<"$section") -eq 3 ]] || fail "no plan section for $draft $transport"
    expected=$(head -n 1 "$work/plan.txt"; printf '%s\n' "$section"; tail -n 1 "$work/plan.txt")
    actual=$(bash "$matrix" --dry-run --pair "$draft" "$transport" "${fake[@]}" | normalize)
    [[ "$actual" == "$expected" ]] || fail "--pair $draft $transport differs from its plan section"
    actual=$(MOQ_INTEROP_TEST_HTTP_PORT=$http_port MOQ_INTEROP_TEST_UDP_PORT=$udp_port \
        bash "$driven" --dry-run "$draft" "$transport" "${fake[@]}" | normalize)
    [[ "$actual" == "$(tail -n 2 <<<"$section")" ]] ||
        fail "driven --dry-run $draft $transport differs from its plan section"
done

# 4. The real matrix against stubs does what its plan says.
mkdir -p "$work/bin" "$work/stub"
ln -s "$root_dir/tests/support/stub_runner.sh" "$work/bin/runner"
touch "$work/bin/moqxr" "$work/fixture.mp4"
stub_bins=("$work/bin/runner" "$work/bin/moqxr" "$work/fixture.mp4")
bash "$matrix" --dry-run "${stub_bins[@]}" | normalize | grep -E '^(runner|request): ' >"$work/stub-plan.txt"
MOQ_STUB_DIR="$work/stub" PATH="$root_dir/tests/support/stub_curl:$PATH" \
    bash "$matrix" "${stub_bins[@]}" >"$work/stub-out.txt" ||
    fail "matrix against stubs exited $?: $(cat "$work/stub-out.txt")"
normalize <"$work/stub/calls.log" >"$work/stub-calls.txt"
diff -u "$work/stub-plan.txt" "$work/stub-calls.txt" || fail 'stub run differs from its dry-run plan'
for entry in "${pair_ports[@]}"; do
    read -r draft transport http_port udp_port <<<"$entry"
    grep -qxF "draft=$draft transport=$transport run=run-$http_port verdict=fail publisher=0 pass=1 fail=1" \
        "$work/stub-out.txt" || fail "no reported row for $draft $transport"
done
[[ $(tail -n 1 "$work/stub-out.txt") == \
    'matrix harness completed; publisher exits and scored rows are reported above' ]] ||
    fail 'matrix did not report completion'

# 5. Bad arguments.
expect_usage() {
    local status=0
    "$@" >/dev/null 2>&1 || status=$?
    [[ $status -eq 2 ]] || fail "expected exit 2 from: $* (got $status)"
}
expect_usage bash "$driven" --dry-run 20 native_quic "${fake[@]}"
expect_usage bash "$driven" --dry-run 22 quic "${fake[@]}"
expect_usage bash "$driven" --dry-run 22 native_quic /opt/a /opt/b
expect_usage bash "$matrix" --dry-run --pair 23 native_quic "${fake[@]}"
expect_usage bash "$matrix" --dry-run --pair 22 h3 "${fake[@]}"
expect_usage bash "$matrix" --dry-run /opt/a /opt/b

printf 'moqxr matrix plan passed\n'
