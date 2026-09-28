#!/usr/bin/env bash
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly HEAD_REVISION="$(git -C "${ROOT}" rev-parse HEAD)"
readonly ERROR_FILE="$(mktemp /tmp/moq-interop-build-contract.XXXXXX)"
readonly DIRTY_PROBE="${ROOT}/.moq-interop-dirty-probe-$$-${RANDOM}.tmp"
readonly BASELINE_STATUS="$(git -C "${ROOT}" status --porcelain=v1 --untracked-files=all)"

cleanup() {
    rm -f "${ERROR_FILE}" "${DIRTY_PROBE}"
}
trap cleanup EXIT

if env -u MOQ_INTEROP_SOURCE_REVISION docker compose \
    --project-directory "${ROOT}" config --quiet 2>"${ERROR_FILE}"; then
    printf 'Compose unexpectedly accepted a missing source revision\n' >&2
    exit 1
fi
grep -F 'MOQ_INTEROP_SOURCE_REVISION is missing a value' "${ERROR_FILE}" >/dev/null

if docker build --quiet --target builder \
    --build-arg SOURCE_REVISION=not-a-git-object "${ROOT}" 2>"${ERROR_FILE}"; then
    printf 'Dockerfile unexpectedly accepted a malformed source revision\n' >&2
    exit 1
fi
grep -F 'SOURCE_REVISION must be a full lowercase Git object ID' "${ERROR_FILE}" >/dev/null

if MOQ_INTEROP_SOURCE_REVISION=0000000000000000000000000000000000000000 \
    "${ROOT}/scripts/container-build.sh" config --quiet 2>"${ERROR_FILE}"; then
    printf 'build wrapper unexpectedly accepted a conflicting source revision\n' >&2
    exit 1
fi
grep -F "conflicts with repository HEAD ${HEAD_REVISION}" "${ERROR_FILE}" >/dev/null

touch "${DIRTY_PROBE}"
if "${ROOT}/scripts/container-build.sh" config --quiet 2>"${ERROR_FILE}"; then
    printf 'build wrapper unexpectedly accepted a dirty repository\n' >&2
    exit 1
fi
grep -F 'repository worktree is dirty; commit or remove relevant changes before building' \
    "${ERROR_FILE}" >/dev/null
rm -f "${DIRTY_PROBE}"

if [[ -z "${BASELINE_STATUS}" ]]; then
    "${ROOT}/scripts/container-build.sh" config --quiet
else
    if "${ROOT}/scripts/container-build.sh" config --quiet 2>"${ERROR_FILE}"; then
        printf 'build wrapper unexpectedly accepted the dirty development tree\n' >&2
        exit 1
    fi
    grep -F 'repository worktree is dirty; commit or remove relevant changes before building' \
        "${ERROR_FILE}" >/dev/null
fi
printf '[foundation-build-contract] PASS: source identity inputs are checked\n'
