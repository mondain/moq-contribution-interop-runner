#!/usr/bin/env bash
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly FIXTURE_LABEL="org.moq-interop.collision-fixture"
readonly TOKEN="$(date +%s)-$$-${RANDOM}"
readonly VOLUME_SUFFIX="collision-volume-${TOKEN}"
readonly VOLUME_NAME="moq-interop-smoke-${VOLUME_SUFFIX}"
readonly CONTAINER_SUFFIX="collision-container-${TOKEN}"
readonly CONTAINER_NAME="moq-interop-smoke-${CONTAINER_SUFFIX}"

fixture_container_owned=false
fixture_volume_owned=false

cleanup() {
    if [[ "${fixture_container_owned}" == "true" ]] &&
       [[ "$(docker container inspect --format "{{index .Config.Labels \"${FIXTURE_LABEL}\"}}" \
              "${CONTAINER_NAME}" 2>/dev/null || true)" == "${TOKEN}" ]]; then
        docker rm -f "${CONTAINER_NAME}" >/dev/null
    fi
    if [[ "${fixture_volume_owned}" == "true" ]] &&
       [[ "$(docker volume inspect --format "{{index .Labels \"${FIXTURE_LABEL}\"}}" \
              "${VOLUME_NAME}" 2>/dev/null || true)" == "${TOKEN}" ]]; then
        docker volume rm "${VOLUME_NAME}" >/dev/null
    fi
}
trap cleanup EXIT

docker volume create --label "${FIXTURE_LABEL}=${TOKEN}" "${VOLUME_NAME}" >/dev/null
fixture_volume_owned=true
if MOQ_INTEROP_SMOKE_SUFFIX="${VOLUME_SUFFIX}" bash "${ROOT}/tests/e2e/foundation-smoke.sh"; then
    printf 'smoke unexpectedly accepted a pre-existing volume\n' >&2
    exit 1
fi
docker volume inspect "${VOLUME_NAME}" >/dev/null
[[ "$(docker volume inspect --format "{{index .Labels \"${FIXTURE_LABEL}\"}}" \
       "${VOLUME_NAME}")" == "${TOKEN}" ]]
docker volume rm "${VOLUME_NAME}" >/dev/null
fixture_volume_owned=false

docker create --name "${CONTAINER_NAME}" \
    --label "${FIXTURE_LABEL}=${TOKEN}" \
    --entrypoint /bin/true debian:bookworm-slim >/dev/null
fixture_container_owned=true
if MOQ_INTEROP_SMOKE_SUFFIX="${CONTAINER_SUFFIX}" bash "${ROOT}/tests/e2e/foundation-smoke.sh"; then
    printf 'smoke unexpectedly accepted a pre-existing container\n' >&2
    exit 1
fi
docker container inspect "${CONTAINER_NAME}" >/dev/null
[[ "$(docker container inspect --format "{{index .Config.Labels \"${FIXTURE_LABEL}\"}}" \
       "${CONTAINER_NAME}")" == "${TOKEN}" ]]

printf '[foundation-collision-safety] PASS: pre-existing resources survived both collisions\n'
