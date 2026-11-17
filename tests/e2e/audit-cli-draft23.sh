#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 ]] || { printf 'Usage: %s AUDIT_BIN\n' "$0" >&2; exit 2; }
audit_bin=$1
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
common=(--docs "$root_dir/docs" --requirements "$root_dir/requirements")
output=$("$audit_bin" --draft 23 --staged --format json "${common[@]}")
jq -e '.draft == 23 and .staged == true and .complete == false and
    .rows == 532 and .reviewed == 532 and .unreviewed == 0 and
    .required_applicable_testable == 91 and .required_covered == 91 and .optional_applicable_testable == 9 and .optional_covered == 9 and
    ([.findings[] | select(.blocking == true)] | length) == 0 and
    ([.findings[] | select(.code == "missing_required_evaluator")] | length) == 0 and
    .source_audit.complete == true and (.verdict | startswith("STAGED:"))' <<<"$output" >/dev/null
text=$("$audit_bin" --draft 23 --staged "${common[@]}")
[[ "$text" == *'STAGED: incomplete catalog (not a pass)'* ]]
# The normal conformance gate must continue refusing an incomplete catalog.
set +e
"$audit_bin" --draft 23 "${common[@]}" >/dev/null 2>&1
status=$?
set -e
[[ $status == 2 ]] || { printf 'incomplete draft 23 catalog was accepted without --staged\n' >&2; exit 1; }
printf 'draft 23 staged audit passed\n'
