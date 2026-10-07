#!/usr/bin/env bash
# The staged moq-lite-06 audit: `moq-interop-audit --draft moq-lite-06` reports the incomplete
# catalog (rows, reviewed, unreviewed, planned scenarios) and is never a pass. The expected counts
# are computed independently from requirements/moq-lite-06.json.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s AUDIT_BIN\n' "$0" >&2
    exit 2
fi
audit_bin=$1
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
check_line "Required applicable testable: $(exp required_at)"
check_line "Planned scenarios: $(exp scenarios)"
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
    ([.findings[] | select(.code == "missing_required_evaluator")] | length) == $e[0].required_at and
    ([.findings[] | select(.code == "missing_optional_evaluator")] | length) == $e[0].optional_at' \
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
printf 'audit CLI moq-lite-06 passed\n'
