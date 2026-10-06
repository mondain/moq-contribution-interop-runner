#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s AUDIT_BIN\n' "$0" >&2
    exit 2
fi
audit_bin=$1
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/moq-interop-audit22.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
common=(--docs "$root_dir/docs" --requirements "$root_dir/requirements")

set +e
"$audit_bin" --draft 22 "${common[@]}" >"$test_dir/text.out" 2>"$test_dir/text.err"
status=$?
set -e
grep -q '^Required executable coverage: 170/170$' "$test_dir/text.out"
# requirements/draft22.json is complete and every required row is bound, so the static gate
# passes with no blocking finding and the exit status is 0.
[[ "$status" -eq 0 ]] || { echo "unexpected status $status" >&2; exit 1; }
grep -q '^Static gate: PASS' "$test_dir/text.out"

set +e
"$audit_bin" --draft 22 --format json "${common[@]}" >"$test_dir/audit.json"
status=$?
set -e
[[ "$status" -eq 0 ]] || { echo "unexpected JSON status $status" >&2; exit 1; }
jq -e '.draft == 22 and .executable_coverage.required_covered == 170 and
    .executable_coverage.required_total == 170 and .static_complete == true and
    .source_audit.complete == true and
    ([.findings[] | select(.blocking)] | length) == 0' \
    "$test_dir/audit.json" >/dev/null

# --database with draft 22 is refused (audit_execution over stored draft 22 runs is D4).
: >"$test_dir/runs.sqlite3"
set +e
"$audit_bin" --draft 22 --database "$test_dir/runs.sqlite3" "${common[@]}" \
    >"$test_dir/db.out" 2>"$test_dir/db.err"
status=$?
set -e
# A refused option is a usage error (exit 2), distinct from a failed gate (exit 1).
[[ "$status" -eq 2 ]] || { echo "unexpected --database status $status" >&2; exit 1; }
grep -q 'stored draft 22 runs are not supported yet' "$test_dir/db.err"
printf 'audit CLI draft 22 passed\n'
