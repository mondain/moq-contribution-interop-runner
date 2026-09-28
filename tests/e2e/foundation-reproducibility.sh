#!/usr/bin/env bash
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly TOKEN="$(date +%s)-$$-${RANDOM}"
readonly IMAGE_A="moq-contribution-interop-runner:repro-a-${TOKEN}"
readonly IMAGE_B="moq-contribution-interop-runner:repro-b-${TOKEN}"

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
build_image "${IMAGE_B}"

metadata_a="$(metadata "${IMAGE_A}")"
metadata_b="$(metadata "${IMAGE_B}")"
[[ "${metadata_a}" == "${metadata_b}" ]]
[[ "$(history_digest "${IMAGE_A}")" == "$(history_digest "${IMAGE_B}")" ]]
[[ "$(runtime_digests "${IMAGE_A}")" == "$(runtime_digests "${IMAGE_B}")" ]]

printf '[foundation-reproducibility] PASS: %s\n' "${metadata_a}"
