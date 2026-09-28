#!/usr/bin/env bash
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly TOKEN="$(date +%s)-$$-${RANDOM}"
readonly IMAGE_A="moq-contribution-interop-runner:repro-a-${TOKEN}"
readonly IMAGE_B="moq-contribution-interop-runner:repro-b-${TOKEN}"
image_a_owned=false
image_b_owned=false
image_a_id=''
image_b_id=''

cleanup_image() {
    local image=$1
    local expected_id=$2
    local owned=$3
    local current_id

    [[ "${owned}" == true ]] || return 0
    current_id="$(docker image inspect --format '{{.Id}}' "${image}" 2>/dev/null || true)"
    if [[ -n "${current_id}" && "${current_id}" == "${expected_id}" ]]; then
        docker image rm "${image}" >/dev/null
    fi
}

cleanup() {
    cleanup_image "${IMAGE_A}" "${image_a_id}" "${image_a_owned}"
    cleanup_image "${IMAGE_B}" "${image_b_id}" "${image_b_owned}"
}
trap cleanup EXIT

for image in "${IMAGE_A}" "${IMAGE_B}"; do
    if docker image inspect "${image}" >/dev/null 2>&1; then
        printf 'image tag collision: %s\n' "${image}" >&2
        exit 1
    fi
done

build_image() {
    MOQ_INTEROP_IMAGE="$1" "${ROOT}/scripts/container-build.sh" \
        build --no-cache --quiet
}

metadata() {
    docker image inspect "$1" --format \
        '{{.Id}}|{{.Created}}|{{.Size}}|{{index .Config.Labels "org.opencontainers.image.revision"}}|{{index .Config.Labels "org.moq-interop.debian-snapshot"}}'
}

history_digest() {
    docker history --no-trunc --format \
        '{{.ID}}|{{.CreatedAt}}|{{.CreatedBy}}|{{.Size}}' "$1" | sha256sum | cut -d' ' -f1
}

runtime_digests() {
    docker run --rm --entrypoint sh "$1" -ec \
        'dpkg-query -W | sort | sha256sum; sha256sum /usr/local/bin/moq-interop-runner'
}

build_image "${IMAGE_A}"
image_a_id="$(docker image inspect --format '{{.Id}}' "${IMAGE_A}")"
image_a_owned=true
build_image "${IMAGE_B}"
image_b_id="$(docker image inspect --format '{{.Id}}' "${IMAGE_B}")"
image_b_owned=true

metadata_a="$(metadata "${IMAGE_A}")"
metadata_b="$(metadata "${IMAGE_B}")"
[[ "${metadata_a}" == "${metadata_b}" ]]
[[ "$(history_digest "${IMAGE_A}")" == "$(history_digest "${IMAGE_B}")" ]]
[[ "$(runtime_digests "${IMAGE_A}")" == "$(runtime_digests "${IMAGE_B}")" ]]

printf '[foundation-reproducibility] PASS: %s\n' "${metadata_a}"
