#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -x "$1" ]]; then
    printf 'Usage: %s RUNNER_BIN\n' "$0" >&2
    exit 2
fi

if matches=$(nm -C "$1" | rg ' [Tt] quiche_'); then
    printf 'production runner contains quiche symbols: %s\n' \
        "${matches%%$'\n'*}" >&2
    exit 1
fi

printf 'production runner has no quiche symbols\n'
