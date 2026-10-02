#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir="$root_dir/build-asan"
cmake -S "$root_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-fsanitize=address,undefined \
    -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build "$build_dir" --parallel 2 --target \
    moq-interop-publisher-driver-tests moq-interop-run-store-tests \
    moq-interop-execution-audit-tests \
    moq-interop-webtransport-run-api-tests moq-interop-result-export-tests
ctest --test-dir "$build_dir" --output-on-failure --parallel 2 \
    -R '^(publisher-driver|run-store|execution-audit|webtransport-run-api|result-export)$'
