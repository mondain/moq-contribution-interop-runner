#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'Usage: %s PICOQUIC_RUNNER LEGACY_RUNNER QUICHE_PEER\n' "$0" >&2
    exit 2
fi

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d /tmp/moq-interop-native-parity.XXXXXX)
cleanup() {
    case "$test_dir" in
        /tmp/moq-interop-native-parity.*)
            rm -f -- "$test_dir/18-picoquic.json" \
                "$test_dir/18-legacy.json" \
                "$test_dir/21-picoquic.json" \
                "$test_dir/21-legacy.json"
            rmdir -- "$test_dir"
            ;;
    esac
}
trap cleanup EXIT

for draft in 18 21; do
    MOQ_INTEROP_NATIVE_PARITY_OUTPUT="$test_dir/$draft-picoquic.json" \
        bash "$root_dir/native-peer.sh" "$1" "$3" "$draft"
    MOQ_INTEROP_NATIVE_PARITY_OUTPUT="$test_dir/$draft-legacy.json" \
        bash "$root_dir/native-peer.sh" "$2" "$3" "$draft"
    diff -u "$test_dir/$draft-legacy.json" \
        "$test_dir/$draft-picoquic.json"
done

printf 'draft-18 and draft-21 native evidence and scores match\n'
