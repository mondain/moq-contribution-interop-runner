#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir="$root_dir/build-fuzz"
cmake -S "$root_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DMOQ_INTEROP_BUILD_TESTS=OFF -DMOQ_INTEROP_BUILD_FUZZERS=ON \
    -DMOQ_INTEROP_BUILD_QUICHE_TEST_PEER=OFF
cmake --build "$build_dir" --parallel 2 --target \
    moq-interop-cursor-fuzz moq-interop-draft18-message-fuzz \
    moq-interop-draft18-object-fuzz moq-interop-webtransport-stream-fuzz

for target in moq-interop-cursor-fuzz moq-interop-draft18-message-fuzz \
              moq-interop-draft18-object-fuzz moq-interop-webtransport-stream-fuzz; do
    printf 'fuzz smoke: %s\n' "$target"
    timeout 30 "$build_dir/$target" -runs=500 -max_len=4096 \
        -timeout=5 -rss_limit_mb=1024 -print_final_stats=1 -verbosity=0 \
        -artifact_prefix="$build_dir/"
done
