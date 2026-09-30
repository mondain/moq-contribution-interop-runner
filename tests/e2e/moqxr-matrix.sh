#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'Usage: %s RUNNER_BIN MOQXR_BIN MP4_FIXTURE\n' "$0" >&2
    exit 2
fi
runner_bin=$1
publisher_bin=$2
fixture=$3
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

bash "$script_dir/moqxr-adapter-contract.sh"
for draft in 18 21; do
    for transport in native_quic webtransport; do
        if [[ "$draft" == 18 && "$transport" == native_quic ]]; then
            http_port=19201
            udp_port=19202
        elif [[ "$draft" == 21 && "$transport" == native_quic ]]; then
            http_port=19203
            udp_port=19204
        elif [[ "$draft" == 18 ]]; then
            http_port=19205
            udp_port=19206
        else
            http_port=19207
            udp_port=19208
        fi
        printf 'matrix draft=%s transport=%s\n' "$draft" "$transport"
        if [[ "$transport" == native_quic ]]; then
            MOQ_INTEROP_USE_MOQXR_ADAPTER=1 MOQ_INTEROP_ALLOW_PUBLISHER_FAILURE=1 \
                MOQ_INTEROP_TEST_HTTP_PORT="$http_port" \
                MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
                bash "$script_dir/draft18-native-moqxr.sh" \
                "$runner_bin" "$publisher_bin" "$fixture" "$draft"
        else
            MOQ_INTEROP_USE_MOQXR_ADAPTER=1 MOQ_INTEROP_ALLOW_PUBLISHER_FAILURE=1 \
                MOQ_INTEROP_TEST_HTTP_PORT="$http_port" \
                MOQ_INTEROP_TEST_UDP_PORT="$udp_port" \
                bash "$script_dir/webtransport-moqxr.sh" \
                "$draft" "$runner_bin" "$publisher_bin" "$fixture"
        fi
    done
done
printf 'matrix harness completed; publisher exits and scored rows are reported above\n'
