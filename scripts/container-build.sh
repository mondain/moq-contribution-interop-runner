#!/usr/bin/env bash
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly EXPECTED_SOURCE_DATE_EPOCH="1790467200"
readonly HEAD_REVISION="$(git -C "${ROOT}" rev-parse --verify 'HEAD^{commit}')"

if [[ ! "${HEAD_REVISION}" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'repository HEAD is not a full lowercase Git object ID: %s\n' \
        "${HEAD_REVISION}" >&2
    exit 2
fi
if [[ -n "${MOQ_INTEROP_SOURCE_REVISION:-}" &&
      "${MOQ_INTEROP_SOURCE_REVISION}" != "${HEAD_REVISION}" ]]; then
    printf 'MOQ_INTEROP_SOURCE_REVISION %s conflicts with repository HEAD %s\n' \
        "${MOQ_INTEROP_SOURCE_REVISION}" "${HEAD_REVISION}" >&2
    exit 2
fi
if [[ -n "${SOURCE_DATE_EPOCH:-}" &&
      "${SOURCE_DATE_EPOCH}" != "${EXPECTED_SOURCE_DATE_EPOCH}" ]]; then
    printf 'SOURCE_DATE_EPOCH %s conflicts with required value %s\n' \
        "${SOURCE_DATE_EPOCH}" "${EXPECTED_SOURCE_DATE_EPOCH}" >&2
    exit 2
fi
if [[ $# -eq 0 || ( "$1" != "build" && "$1" != "config" ) ]]; then
    printf 'Usage: %s {build|config} [docker compose arguments...]\n' "$0" >&2
    exit 2
fi
if [[ -n "$(git -C "${ROOT}" status --porcelain=v1 --untracked-files=all)" ]]; then
    printf '%s\n' \
        'repository worktree is dirty; commit or remove relevant changes before building' >&2
    exit 2
fi

export MOQ_INTEROP_SOURCE_REVISION="${HEAD_REVISION}"
export SOURCE_DATE_EPOCH="${EXPECTED_SOURCE_DATE_EPOCH}"

command=$1
shift
if [[ "${command}" == "config" ]]; then
    exec docker compose --project-directory "${ROOT}" config "$@"
fi

bake_options=()
for argument in "$@"; do
    case "${argument}" in
        --no-cache|--pull) bake_options+=("${argument}") ;;
        --quiet) ;;
        *)
            printf 'unsupported build argument: %s\n' "${argument}" >&2
            exit 2
            ;;
    esac
done

docker compose --project-directory "${ROOT}" config --quiet
exec docker buildx bake --file "${ROOT}/compose.yaml" \
    --set 'validator.output=type=docker,rewrite-timestamp=true' \
    "${bake_options[@]}" validator
