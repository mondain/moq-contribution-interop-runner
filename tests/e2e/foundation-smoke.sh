#!/usr/bin/env bash
set -euo pipefail

readonly IMAGE="moq-contribution-interop-runner:local"
readonly UNIQUE_SUFFIX="$(date +%s)-$$-${RANDOM}"
readonly CONTAINER="moq-interop-smoke-${UNIQUE_SUFFIX}"
readonly VOLUME="moq-interop-smoke-${UNIQUE_SUFFIX}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly RETAIN_ARTIFACTS="${MOQ_INTEROP_RETAIN_SMOKE_ARTIFACTS:-0}"

active_container=""

log() {
    printf '[foundation-smoke] %s\n' "$*"
}

cleanup() {
    if [[ "${RETAIN_ARTIFACTS}" == "1" ]]; then
        log "retaining container ${active_container:-none} and volume ${VOLUME}"
        return
    fi
    if [[ -n "${active_container}" ]]; then
        docker rm -f "${active_container}" >/dev/null 2>&1 || true
    fi
    docker volume rm -f "${VOLUME}" >/dev/null 2>&1 || true
}

failure_diagnostics() {
    local status=$?
    if [[ ${status} -ne 0 && -n "${active_container}" ]]; then
        log "failure diagnostics for ${active_container}"
        docker inspect "${active_container}" 2>/dev/null || true
        docker logs "${active_container}" 2>/dev/null || true
    fi
    cleanup
    exit "${status}"
}
trap failure_diagnostics EXIT

require_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        printf 'required host tool not found: %s\n' "$1" >&2
        exit 1
    }
}

require_tool docker
require_tool curl
require_tool jq

container_port() {
    docker port "$1" 8080/tcp | awk -F: 'NR == 1 {print $NF}'
}

wait_for_ready() {
    local container=$1
    local port=$2
    local attempt health
    for attempt in $(seq 1 60); do
        health="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "${container}")"
        if [[ "${health}" == "healthy" ]] &&
           curl --fail --silent --show-error --max-time 2 \
               "http://127.0.0.1:${port}/healthz" >/dev/null; then
            return
        fi
        if [[ "$(docker inspect --format '{{.State.Running}}' "${container}")" != "true" ]]; then
            printf 'container stopped before becoming ready\n' >&2
            return 1
        fi
        sleep 1
    done
    printf 'container did not become ready within 60 seconds\n' >&2
    return 1
}

start_container() {
    local name=$1
    active_container="${name}"
    docker run --detach --name "${name}" \
        --user 10001:10001 \
        --read-only \
        --tmpfs /tmp:rw,noexec,nosuid,size=16m \
        --security-opt no-new-privileges:true \
        --mount "type=volume,source=${VOLUME},target=/var/lib/moq-interop" \
        --publish 127.0.0.1::8080 \
        "${IMAGE}" >/dev/null
}

assert_runtime_hardening() {
    local container=$1
    [[ "$(docker inspect --format '{{.HostConfig.ReadonlyRootfs}}' "${container}")" == "true" ]]
    [[ "$(docker inspect --format '{{.HostConfig.SecurityOpt}}' "${container}")" == "[no-new-privileges:true]" ]]
    [[ "$(docker exec "${container}" id -u)" == "10001" ]]
    docker exec "${container}" sh -ec \
        'test -w /var/lib/moq-interop &&
         test ! -w /usr/share/moq-interop/docs/draft-ietf-moq-transport-18.txt &&
         test ! -w /usr/share/moq-interop/requirements/draft18.json &&
         touch /var/lib/moq-interop/smoke-write &&
         rm /var/lib/moq-interop/smoke-write'
}

log "building ${IMAGE}"
source_revision="$(git -C "${ROOT}" rev-parse HEAD)"
docker build --quiet --build-arg "SOURCE_REVISION=${source_revision}" \
    --tag "${IMAGE}" "${ROOT}" >/dev/null
docker volume create "${VOLUME}" >/dev/null

start_container "${CONTAINER}"
port="$(container_port "${CONTAINER}")"
[[ "${port}" =~ ^[0-9]+$ ]]
wait_for_ready "${CONTAINER}" "${port}"

base_url="http://127.0.0.1:${port}"
health="$(curl --fail --silent --show-error "${base_url}/healthz")"
jq -e '.schema_version == 1 and .status == "ok" and .database.ready == true' \
    <<<"${health}" >/dev/null

drafts="$(curl --fail --silent --show-error "${base_url}/api/v1/drafts")"
jq -e '
    .schema_version == 1 and
    (.drafts | length) == 2 and
    any(.drafts[]; .draft == 18 and .complete == true and .requirement_count == 598) and
    any(.drafts[]; .draft == 21 and .complete == true and .requirement_count == 638)
' <<<"${drafts}" >/dev/null

requirements="$(curl --fail --silent --show-error \
    "${base_url}/api/v1/requirements?draft=18&limit=2&offset=1")"
jq -e '
    .schema_version == 1 and .draft == 18 and
    .pagination.limit == 2 and .pagination.offset == 1 and
    .pagination.total == 598 and .pagination.next_offset == 3 and
    (.items | length) == 2
' <<<"${requirements}" >/dev/null

curl --fail --silent --show-error "${base_url}/results" >/dev/null

run_request='{"draft":21,"transport":"native-quic","mode":"observed","scenarios":["foundation/persistence"],"timeout_ms":30000}'
created="$(curl --fail --silent --show-error \
    --header 'Content-Type: application/json' --data "${run_request}" \
    "${base_url}/api/v1/runs")"
run_id="$(jq -er '
    select(.schema_version == 1) |
    select(.run.config.draft == 21) |
    select(.run.config.transport == "native-quic") |
    select(.run.config.mode == "observed") |
    select(.run.config.scenarios == ["foundation/persistence"]) |
    .run.id | select(length > 0)
' <<<"${created}")"

runs="$(curl --fail --silent --show-error "${base_url}/api/v1/runs")"
jq -e --arg id "${run_id}" 'any(.items[]; .id == $id)' <<<"${runs}" >/dev/null
assert_runtime_hardening "${CONTAINER}"

docker stop --time 10 "${CONTAINER}" >/dev/null
docker rm "${CONTAINER}" >/dev/null
active_container=""

readonly REPLACEMENT="${CONTAINER}-replacement"
start_container "${REPLACEMENT}"
replacement_port="$(container_port "${REPLACEMENT}")"
[[ "${replacement_port}" =~ ^[0-9]+$ ]]
wait_for_ready "${REPLACEMENT}" "${replacement_port}"

persisted="$(curl --fail --silent --show-error \
    "http://127.0.0.1:${replacement_port}/api/v1/runs/${run_id}")"
jq -e --arg id "${run_id}" '
    .schema_version == 1 and .run.id == $id and
    .run.config == {
        "draft": 21,
        "transport": "native-quic",
        "mode": "observed",
        "scenarios": ["foundation/persistence"],
        "timeout_ms": 30000
    }
' <<<"${persisted}" >/dev/null
assert_runtime_hardening "${REPLACEMENT}"

if [[ "${RETAIN_ARTIFACTS}" != "1" ]]; then
    cleanup
    active_container=""
    trap - EXIT
    ! docker container inspect "${CONTAINER}" >/dev/null 2>&1
    ! docker container inspect "${REPLACEMENT}" >/dev/null 2>&1
    ! docker volume inspect "${VOLUME}" >/dev/null 2>&1
else
    trap - EXIT
    log "retaining container ${REPLACEMENT} and volume ${VOLUME}"
fi

log "PASS: persisted run ${run_id} across container replacement"
