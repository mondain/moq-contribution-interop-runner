#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
draft18_digest=$(jq -r '."18"' "$root_dir/requirements/draft-digests.json")
draft21_digest=$(jq -r '."21"' "$root_dir/requirements/draft-digests.json")
source_revision=$(git -C "$root_dir" rev-parse --verify 'HEAD^{commit}')
test_dir=$(mktemp -d /tmp/moq-interop-release-contract.XXXXXX)
cleanup() {
    case "$test_dir" in
        /tmp/moq-interop-release-contract.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT

make_report() {
    local static_complete=$1
    local stages=$2
    jq -n --argjson complete "$static_complete" --argjson stages "$stages" \
        --arg digest18 "$draft18_digest" --arg digest21 "$draft21_digest" \
        --arg revision "$source_revision" '{
        schema_version: 1,
        source_revision: $revision,
        drafts: [
            {draft: 18, source_sha256: $digest18, static_complete: $complete},
            {draft: 21, source_sha256: $digest21, static_complete: $complete}
        ],
        stages: $stages
    }' >"$test_dir/report.json"
}

make_report true '[]'
if bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/report.json" \
    >"$test_dir/check.log" 2>&1; then
    printf 'missing release stages were accepted\n' >&2
    exit 1
fi
rg -q 'missing stage: native_suite' "$test_dir/check.log"
rg -q 'missing stage: docker_d21_webtransport' "$test_dir/check.log"

stages=$(jq -n '[
    "native_suite", "asan_ubsan", "fuzz_smoke",
    "docker_d18_native", "docker_d18_webtransport",
    "docker_d21_native", "docker_d21_webtransport",
    "moqxr_d18_webtransport", "moqxr_d21_webtransport",
    "audit_d18", "audit_d21"
] | map({id: ., command: "verified-command", status: "pass", exit_code: 0})')
make_report false "$stages"
if bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/report.json" \
    >"$test_dir/check.log" 2>&1; then
    printf 'incomplete draft catalog was accepted\n' >&2
    exit 1
fi
rg -q 'draft 18 static gate incomplete' "$test_dir/check.log"
rg -q 'draft 21 static gate incomplete' "$test_dir/check.log"

make_report true "$stages"
bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/report.json"

jq '(.drafts[] | select(.draft == 18) | .source_sha256) =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"' \
    "$test_dir/report.json" >"$test_dir/drifted.json"
if bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/drifted.json" \
    >"$test_dir/check.log" 2>&1; then
    printf 'drifted draft digest was accepted\n' >&2
    exit 1
fi
rg -q 'draft 18 digest drift' "$test_dir/check.log"

jq '.source_revision = "dddddddddddddddddddddddddddddddddddddddd"' \
    "$test_dir/report.json" >"$test_dir/revision-drift.json"
if bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/revision-drift.json" \
    >"$test_dir/check.log" 2>&1; then
    printf 'drifted source revision was accepted\n' >&2
    exit 1
fi
rg -q 'source revision drift' "$test_dir/check.log"

failed_stages=$(jq -n --argjson stages "$stages" '$stages | map(if .id == "fuzz_smoke" then .status = "timeout" | .exit_code = 124 else . end)')
make_report true "$failed_stages"
if bash "$root_dir/tests/e2e/release-audit.sh" check "$test_dir/report.json" \
    >"$test_dir/check.log" 2>&1; then
    printf 'timed-out fuzz stage was accepted\n' >&2
    exit 1
fi
rg -q 'stage fuzz_smoke: timeout' "$test_dir/check.log"

if [[ "${1:-}" == --check-only ]]; then
    printf 'release audit check contract passed\n'
    exit 0
fi

mkdir "$test_dir/occupied"
touch "$test_dir/occupied/previous-audit.json"
if bash "$root_dir/tests/e2e/release-audit.sh" run "$test_dir/occupied" \
    "$root_dir/build/moq-interop-audit" >"$test_dir/run.log" 2>&1; then
    printf 'release audit overwrote an existing artifact directory\n' >&2
    exit 1
fi
rg -q 'output directory already exists' "$test_dir/run.log"
[[ -f "$test_dir/occupied/previous-audit.json" ]]

if bash "$root_dir/tests/e2e/release-audit.sh" run "$test_dir/run" \
    "$root_dir/build/moq-interop-audit" >"$test_dir/run.log" 2>&1; then
    printf 'incomplete live release audit was accepted\n' >&2
    exit 1
fi
jq -e '.drafts | length == 2' "$test_dir/run/release-audit.json" >/dev/null
jq -e '.stages | any(.[]; .id == "audit_d18" and .status == "fail")' \
    "$test_dir/run/release-audit.json" >/dev/null
jq -e '.stages | any(.[]; .id == "native_suite" and .status == "pass" and .exit_code == 0)' \
    "$test_dir/run/release-audit.json" >/dev/null
jq -e '.stages | any(.[]; .id == "docker_d21_webtransport" and .status == "missing")' \
    "$test_dir/run/release-audit.json" >/dev/null
rg -q 'stage docker_d21_webtransport: missing' "$test_dir/run.log"

printf 'release audit contract passed\n'
