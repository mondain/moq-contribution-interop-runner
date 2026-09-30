#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s check REPORT_JSON | run OUTPUT_DIR AUDIT_BIN [IMAGE NATIVE_PEER MOQXR_BIN MP4_FIXTURE]\n' "$0" >&2
    exit 2
}

required_stages=(
    native_suite asan_ubsan fuzz_smoke
    docker_d18_native docker_d18_webtransport
    docker_d21_native docker_d21_webtransport
    moqxr_d18_webtransport moqxr_d21_webtransport
    audit_d18 audit_d21
)

if [[ ( $# -eq 3 || $# -eq 7 ) && "$1" == run ]]; then
    output_dir=$2
    if [[ -e "$output_dir" ]]; then
        printf 'output directory already exists: %s\n' "$output_dir" >&2
        exit 2
    fi
    audit_bin=$(realpath "$3")
    root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
    revision=$(git -C "$root_dir" rev-parse --verify 'HEAD^{commit}')
    mkdir -p -- "$output_dir"
    stages=$(printf '%s\n' "${required_stages[@]}" | jq -R -s \
        'split("\n") | map(select(length > 0) |
         {id: ., command: "not executed", status: "missing", exit_code: null})')
    run_stage() {
        local id=$1
        shift
        local command status stage_status
        printf -v command '%q ' "$@"
        set +e
        "$@" >"$output_dir/$id.log" 2>&1
        status=$?
        set -e
        stage_status=fail
        if [[ "$status" -eq 0 ]]; then stage_status=pass; fi
        if [[ "$status" -eq 124 ]]; then stage_status=timeout; fi
        stages=$(jq -n --argjson previous "$stages" --arg id "$id" \
            --arg command "$command" --arg status "$stage_status" \
            --argjson exit_code "$status" '
                {id: $id, command: $command, status: $status,
                 exit_code: $exit_code} as $receipt |
                if any($previous[]; .id == $id) then
                    $previous | map(if .id == $id then $receipt else . end)
                else $previous + [$receipt] end')
    }
    run_stage native_suite ctest --test-dir "$root_dir/build" \
        --output-on-failure --parallel 2
    run_stage asan_ubsan timeout 900 bash \
        "$root_dir/tests/e2e/sanitizer-smoke.sh"
    run_stage fuzz_smoke timeout 900 bash \
        "$root_dir/tests/e2e/fuzz-smoke.sh"
    publisher=null
    if [[ $# -eq 7 ]]; then
        image=$4
        native_peer=$(realpath "$5")
        publisher_bin=$(realpath "$6")
        fixture=$(realpath "$7")
        publisher_version=$("$publisher_bin" --version 2>&1) || publisher_version=
        publisher_hash=$(sha256sum "$publisher_bin")
        publisher_hash=${publisher_hash%% *}
        fixture_hash=$(sha256sum "$fixture")
        fixture_hash=${fixture_hash%% *}
        publisher=$(jq -n --arg version "$publisher_version" \
            --arg binary_sha256 "$publisher_hash" \
            --arg fixture_sha256 "$fixture_hash" \
            '{version: $version, binary_sha256: $binary_sha256,
              fixture_sha256: $fixture_sha256}')
        for draft in 18 21; do
            for transport in native_quic webtransport; do
                stage_transport=$transport
                if [[ "$transport" == native_quic ]]; then stage_transport=native; fi
                stage_id="docker_d${draft}_${stage_transport}"
                run_stage "$stage_id" timeout 120 env \
                    "MOQ_INTEROP_CASE_ARTIFACT_DIR=$output_dir/$stage_id" bash \
                    "$root_dir/tests/e2e/docker-publisher-case.sh" \
                    "$draft" "$transport" "$image" "$native_peer" \
                    "$publisher_bin" "$fixture"
            done
            stage_id="moqxr_d${draft}_webtransport"
            run_stage "$stage_id" timeout 180 env \
                "MOQ_INTEROP_REPEAT_ARTIFACT_DIR=$output_dir/$stage_id" bash \
                "$root_dir/tests/e2e/repeatability.sh" "$draft" \
                "$root_dir/build/moq-interop-runner" "$audit_bin" \
                "$publisher_bin" "$fixture"
        done
    fi
    drafts='[]'
    for draft in 18 21; do
        command="$audit_bin --draft $draft --format json --docs $root_dir/docs --requirements $root_dir/requirements"
        set +e
        "$audit_bin" --draft "$draft" --format json --docs "$root_dir/docs" \
            --requirements "$root_dir/requirements" \
            >"$output_dir/draft-$draft.json" 2>"$output_dir/draft-$draft.log"
        status=$?
        set -e
        digest=$(jq -r '.source_sha256 // ""' "$output_dir/draft-$draft.json" 2>/dev/null) || digest=
        complete=$(jq -r '.static_complete // false' "$output_dir/draft-$draft.json" 2>/dev/null) || complete=false
        drafts=$(jq -n --argjson previous "$drafts" --argjson draft "$draft" \
            --arg digest "$digest" --argjson complete "$complete" \
            '$previous + [{draft: $draft, source_sha256: $digest,
                           static_complete: $complete}]')
        stage_status=fail
        if [[ "$status" -eq 0 ]]; then stage_status=pass; fi
        stages=$(jq -n --argjson previous "$stages" --arg id "audit_d$draft" \
            --arg command "$command" --arg status "$stage_status" \
            --argjson exit_code "$status" '$previous | map(
                if .id == $id then {id: $id, command: $command,
                    status: $status, exit_code: $exit_code} else . end)')
    done
    jq -n --arg revision "$revision" --argjson drafts "$drafts" \
        --argjson stages "$stages" --argjson publisher "$publisher" \
        '{schema_version: 1, source_revision: $revision,
          publisher: $publisher, drafts: $drafts, stages: $stages}' \
        >"$output_dir/release-audit.json"
    exec bash "$0" check "$output_dir/release-audit.json"
fi

[[ $# -eq 2 && "$1" == check ]] || usage
report=$2
[[ -f "$report" ]] || usage
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

failed=0
if ! jq -e '.schema_version == 1 and
    (.source_revision | test("^[0-9a-f]{40}$")) and
    (.stages | type == "array") and
    (.drafts | type == "array")' "$report" >/dev/null; then
    printf 'invalid release audit report header\n' >&2
    failed=1
fi
expected_revision=$(git -C "$root_dir" rev-parse --verify 'HEAD^{commit}')
actual_revision=$(jq -r '.source_revision // ""' "$report")
if [[ "$actual_revision" != "$expected_revision" ]]; then
    printf 'source revision drift\n' >&2
    failed=1
fi

for draft in 18 21; do
    if ! jq -e --argjson draft "$draft" '[.drafts[] | select(.draft == $draft)] | length == 1' \
        "$report" >/dev/null; then
        printf 'missing or duplicate draft: %s\n' "$draft" >&2
        failed=1
        continue
    fi
    if ! jq -e --argjson draft "$draft" '.drafts[] | select(.draft == $draft) |
        (.source_sha256 | test("^[0-9a-f]{64}$")) and .static_complete == true' \
        "$report" >/dev/null; then
        printf 'draft %s static gate incomplete or digest invalid\n' "$draft" >&2
        failed=1
    fi
    expected_digest=$(jq -r --arg draft "$draft" '.[$draft]' \
        "$root_dir/requirements/draft-digests.json")
    actual_digest=$(jq -r --argjson draft "$draft" \
        '.drafts[] | select(.draft == $draft) | .source_sha256' "$report")
    if [[ "$actual_digest" != "$expected_digest" ]]; then
        printf 'draft %s digest drift\n' "$draft" >&2
        failed=1
    fi
done

for id in "${required_stages[@]}"; do
    count=$(jq --arg id "$id" '[.stages[] | select(.id == $id)] | length' "$report")
    if [[ "$count" -eq 0 ]]; then
        printf 'missing stage: %s\n' "$id" >&2
        failed=1
        continue
    fi
    if [[ "$count" -ne 1 ]]; then
        printf 'duplicate stage: %s\n' "$id" >&2
        failed=1
        continue
    fi
    status=$(jq -r --arg id "$id" '.stages[] | select(.id == $id) | .status' "$report")
    if ! jq -e --arg id "$id" '.stages[] | select(.id == $id) |
        .status == "pass" and .exit_code == 0 and
        (.command | type == "string" and length > 0)' "$report" >/dev/null; then
        printf 'stage %s: %s\n' "$id" "$status" >&2
        failed=1
    fi
done

while IFS= read -r id; do
    known=0
    for required in "${required_stages[@]}"; do
        if [[ "$id" == "$required" ]]; then known=1; break; fi
    done
    if [[ "$known" == 0 ]]; then
        printf 'unregistered stage: %s\n' "$id" >&2
        failed=1
    fi
done < <(jq -r '.stages[] | .id // ""' "$report")

if jq -e 'any(.stages[]; (.id | startswith("moqxr_")) and
    .status == "pass")' "$report" >/dev/null; then
    if ! jq -e '.publisher | (.version | type == "string" and length > 0) and
        (.binary_sha256 | test("^[0-9a-f]{64}$")) and
        (.fixture_sha256 | test("^[0-9a-f]{64}$"))' "$report" >/dev/null; then
        printf 'publisher identity missing or invalid\n' >&2
        failed=1
    fi
fi

if [[ "$failed" -ne 0 ]]; then
    exit 1
fi
printf 'release audit gate passed\n'
