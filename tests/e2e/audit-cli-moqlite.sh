#!/usr/bin/env bash
# The staged moq-lite-06 audit: `moq-interop-audit --draft moq-lite-06` reports the incomplete
# catalog (rows, reviewed, unreviewed, planned scenarios) and the coverage of its reviewed rows by the
# L1d executable bindings (every Applicable + Testable row covered, no blocking finding), and is never a
# pass. The expected counts are computed independently from requirements/moq-lite-06.json.
# With FIXTURE_BIN (moq-interop-lite-audit-fixture), the execution audit of stored moq-lite-06 runs
# (`--database PATH`): a clean database is consistent, a run whose passed row lost its declared evidence
# is a finding (status 1), a missing database is refused (status 2), and the MoQ Transport audits see no
# lite run in the same database.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    printf 'Usage: %s AUDIT_BIN [FIXTURE_BIN]\n' "$0" >&2
    exit 2
fi
audit_bin=$1
fixture_bin=${2:-}
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if ! command -v python3 >/dev/null 2>&1 || ! command -v jq >/dev/null 2>&1; then
    printf 'SKIP: python3 and jq are required\n' >&2
    exit 77
fi
test_dir=$(mktemp -d /tmp/moq-interop-auditlite.XXXXXX)
cleanup() {
    case "$test_dir" in
        /tmp/moq-interop-auditlite.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT
common=(--docs "$root_dir/docs" --requirements "$root_dir/requirements")

# Independent expectations.
python3 - "$root_dir/requirements/moq-lite-06.json" >"$test_dir/expected.json" <<'PY'
import json, sys
catalog = json.load(open(sys.argv[1]))
rows = catalog["requirements"]
reviewed = [r for r in rows if r.get("reviewed", True)]
unreviewed = [r for r in rows if not r.get("reviewed", True)]
required = ("Must", "MustNot")
scenarios = {s for r in rows for s in r["scenarios"]}
at = [r for r in reviewed if r["applicability"] == "Applicable" and r["testability"] == "Testable"]
print(json.dumps({
    "sha": catalog["source_sha256"],
    "rows": len(rows),
    "reviewed": len(reviewed),
    "unreviewed": len(unreviewed),
    "unreviewed_required": sum(1 for r in unreviewed if r["strength"] in required),
    "required_at": sum(1 for r in at if r["strength"] in required),
    "optional_at": sum(1 for r in at if r["strength"] not in required),
    "scenarios": len(scenarios),
}))
PY
exp() { jq -r ".$1" "$test_dir/expected.json"; }
[[ "$(exp rows)" -gt 0 && "$(exp unreviewed)" -gt 0 ]] || { echo "bad expectations" >&2; exit 1; }

set +e
"$audit_bin" --draft moq-lite-06 "${common[@]}" >"$test_dir/text.out" 2>"$test_dir/text.err"
status=$?
set -e
[[ "$status" -eq 0 ]] || { echo "unexpected status $status" >&2; cat "$test_dir/text.err" >&2; exit 1; }
check_line() { grep -qxF -- "$1" "$test_dir/text.out" || { echo "missing text line: $1" >&2; cat "$test_dir/text.out" >&2; exit 1; }; }
check_line "Draft moq-lite-06 source $(exp sha)"
check_line "Rows: $(exp rows)"
check_line "Reviewed: $(exp reviewed)"
check_line "Unreviewed: $(exp unreviewed)"
check_line "Unreviewed required (MUST/MUST NOT): $(exp unreviewed_required)"
check_line "Required applicable testable (reviewed rows): $(exp required_at)"
check_line "Planned scenarios: $(exp scenarios)"
check_line "Required coverage (reviewed rows): $(exp required_at) of $(exp required_at)"
check_line "Optional coverage (reviewed rows): $(exp optional_at) of $(exp optional_at)"
check_line "STAGED: incomplete catalog (not a pass)"
grep -q '^Findings: [0-9]' "$test_dir/text.out"
grep -q 'non-blocking' "$test_dir/text.out"
if grep -q 'PASS' "$test_dir/text.out"; then echo "staged output must not say PASS" >&2; exit 1; fi

set +e
"$audit_bin" --draft moq-lite-06 --format json "${common[@]}" >"$test_dir/audit.json"
status=$?
set -e
[[ "$status" -eq 0 ]] || { echo "unexpected JSON status $status" >&2; exit 1; }
jq -e --slurpfile e "$test_dir/expected.json" '
    .draft == "moq-lite-06" and .source_sha256 == $e[0].sha and
    .rows == $e[0].rows and .reviewed == $e[0].reviewed and
    .unreviewed == $e[0].unreviewed and
    .unreviewed_required == $e[0].unreviewed_required and
    .required_applicable_testable == $e[0].required_at and
    .planned_scenarios == $e[0].scenarios and
    .staged == true and .complete == false and
    .verdict == "STAGED: incomplete catalog (not a pass)" and
    .source_audit.complete == true and .source_audit.missing_count == 0 and
    .source_audit.multiply_classified_count == 0 and
    ([.findings[] | select(.blocking)] | length) == 0 and
    ([.findings[] | select(.code == "unreviewed_rows")] | length) == 1 and
    .required_covered == $e[0].required_at and
    .optional_applicable_testable == $e[0].optional_at and
    .optional_covered == $e[0].optional_at and
    ([.findings[] | select(.code == "missing_required_evaluator")] | length) == 0 and
    ([.findings[] | select(.code == "missing_optional_evaluator")] | length) == 0 and
    (.findings | length) == 1' \
    "$test_dir/audit.json" >/dev/null || { echo "JSON shape mismatch" >&2; cat "$test_dir/audit.json" >&2; exit 1; }

# Spellings other than moq-lite-06 are refused with the usage message and status 2.
for bad in 106 moq-lite-05 moq-lite-6 22.0 MOQ-LITE-06; do
    set +e
    "$audit_bin" --draft "$bad" "${common[@]}" >"$test_dir/bad.out" 2>"$test_dir/bad.err"
    status=$?
    set -e
    [[ "$status" -eq 2 ]] || { echo "--draft $bad: unexpected status $status" >&2; exit 1; }
    grep -q '^Usage: moq-interop-audit --draft' "$test_dir/bad.err" ||
        { echo "--draft $bad: no usage message" >&2; exit 1; }
done
# The execution audit of stored moq-lite-06 runs.
if [[ -n "$fixture_bin" ]]; then
    "$fixture_bin" "$test_dir/runs.sqlite3" "$test_dir/tampered.sqlite3" >"$test_dir/fixture.out" ||
        { echo "fixture failed" >&2; exit 1; }
    set +e
    "$audit_bin" --draft moq-lite-06 --database "$test_dir/runs.sqlite3" "${common[@]}" \
        >"$test_dir/db.out" 2>"$test_dir/db.err"
    status=$?
    set -e
    [[ "$status" -eq 0 ]] || { echo "unexpected --database status $status" >&2; cat "$test_dir/db.out" "$test_dir/db.err" >&2; exit 1; }
    grep -Eqx 'Execution audit: consistent \(2 runs, [1-9][0-9]* scored rows, 0 findings\)' "$test_dir/db.out" ||
        { echo "no consistent execution audit line" >&2; cat "$test_dir/db.out" >&2; exit 1; }
    check_db_line() { grep -qxF -- "$1" "$test_dir/db.out" || { echo "missing --database line: $1" >&2; exit 1; }; }
    check_db_line "Rows: $(exp rows)"
    check_db_line "STAGED: incomplete catalog (not a pass)"
    if grep -q 'PASS' "$test_dir/db.out"; then echo "staged output must not say PASS" >&2; exit 1; fi
    "$audit_bin" --draft moq-lite-06 --database "$test_dir/runs.sqlite3" --format json "${common[@]}" \
        >"$test_dir/db.json"
    read -r clean_one clean_two < <(sed -n 's/^clean //p' "$test_dir/fixture.out")
    jq -e --arg one "$clean_one" --arg two "$clean_two" '
        .staged == true and .complete == false and
        .execution_audit.consistent == true and .execution_audit.run_count == 2 and
        .execution_audit.scored_rows > 0 and (.execution_audit.findings | length) == 0 and
        ([.execution_audit.runs[].run_id] == ([$one, $two] | sort)) and
        ([.execution_audit.runs[].transport] == ["native-quic", "native-quic"]) and
        ([.execution_audit.runs[].canonical_sha256 | length] == [64, 64])' "$test_dir/db.json" >/dev/null ||
        { echo "--database JSON shape mismatch" >&2; cat "$test_dir/db.json" >&2; exit 1; }
    # Without --database the JSON says there is no execution audit.
    jq -e '.execution_audit == null' "$test_dir/audit.json" >/dev/null ||
        { echo "execution_audit must be null without --database" >&2; exit 1; }
    # A passed row without its declared evidence is a finding: status 1.
    set +e
    "$audit_bin" --draft moq-lite-06 --database "$test_dir/tampered.sqlite3" --format json "${common[@]}" \
        >"$test_dir/tampered.json"
    status=$?
    set -e
    [[ "$status" -eq 1 ]] || { echo "tampered database: unexpected status $status" >&2; exit 1; }
    jq -e '.execution_audit.consistent == false and .execution_audit.run_count == 1 and
        ([.execution_audit.findings[] | select(.code == "missing_evaluator_evidence" and
            .requirement_id == "L06-3-1-MUST-014")] | length) == 1' "$test_dir/tampered.json" >/dev/null ||
        { echo "tampered finding missing" >&2; cat "$test_dir/tampered.json" >&2; exit 1; }
    set +e
    "$audit_bin" --draft moq-lite-06 --database "$test_dir/tampered.sqlite3" "${common[@]}" >"$test_dir/tampered.out"
    status=$?
    set -e
    [[ "$status" -eq 1 ]] || { echo "tampered text: unexpected status $status" >&2; exit 1; }
    grep -Eqx 'Execution audit: findings \(1 runs, [1-9][0-9]* scored rows, [1-9][0-9]* findings\)' \
        "$test_dir/tampered.out" || { echo "no findings line" >&2; cat "$test_dir/tampered.out" >&2; exit 1; }
    # A database path that does not exist is refused with the usage message.
    set +e
    "$audit_bin" --draft moq-lite-06 --database "$test_dir/absent.sqlite3" "${common[@]}" \
        >"$test_dir/absent.out" 2>"$test_dir/absent.err"
    status=$?
    set -e
    [[ "$status" -eq 2 ]] || { echo "absent database: unexpected status $status" >&2; exit 1; }
    grep -q 'run database does not exist' "$test_dir/absent.err" || { echo "absent database: no reason" >&2; exit 1; }
    [[ ! -e "$test_dir/absent.sqlite3" ]] || { echo "the audit created a database" >&2; exit 1; }
    # The draft 22 audit of the same database holds no draft 22 run.
    "$audit_bin" --draft 22 --database "$test_dir/runs.sqlite3" "${common[@]}" --format json \
        >"$test_dir/db22.json" || true
    jq -e '.execution_audit.run_count == 0' "$test_dir/db22.json" >/dev/null ||
        { echo "the draft 22 audit counted lite runs" >&2; exit 1; }
fi
printf 'audit CLI moq-lite-06 passed\n'
