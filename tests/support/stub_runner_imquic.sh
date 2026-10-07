#!/usr/bin/env bash
# Stand-in for the moq-interop runner binary in tests/e2e/imquic-matrix-plan.sh.
#
# Appends the invocation to $MOQ_STUB_DIR/calls.log in the same format as the --dry-run plan of
# tests/e2e/driven-imquic.sh (`runner: IMQUIC_PUB_BIN=<publisher> <runner> <arg>...`), writes the retained
# request contract the driven script checks for (run id `run-<http port>`, matching
# tests/support/stub_curl/curl), marks itself ready for the stub health check, and then idles until
# the driven script kills it. It never starts a publisher and opens no socket.
set -euo pipefail
{
    printf 'runner: IMQUIC_PUB_BIN=<%s> <%s>' "${IMQUIC_PUB_BIN:-}" "$0"
    printf ' <%s>' "$@"
    printf '\n'
} >>"$MOQ_STUB_DIR/calls.log"
log_root=
port=
while (($#)); do
    case "$1" in
        --driver-log-root) log_root=$2; shift ;;
        --port) port=$2; shift ;;
    esac
    shift
done
[[ -n "$log_root" && -n "$port" ]] || { printf 'stub runner: missing --port or --driver-log-root\n' >&2; exit 2; }
mkdir -p "$log_root/run-$port"
printf '{}\n' >"$log_root/run-$port/request.json"
touch "$MOQ_STUB_DIR/ready-$port"
exec sleep 60
