#!/bin/sh
set -eu

test "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION-}" = 1
test -r "${MOQ_INTEROP_DRIVER_REQUEST_FILE-}"

case "${1-}" in
    success)
        printf 'publisher ok\n'
        ;;
    early-exit)
        printf 'publisher rejected startup\n' >&2
        exit 7
        ;;
    invalid-utf8)
        printf '\377\376\000' >&2
        ;;
    sleep)
        exec sleep 5
        ;;
    ignore-term)
        trap '' TERM
        while :; do :; done
        ;;
    *)
        exit 2
        ;;
esac
