#!/usr/bin/env bash
# Stand-in for the moq-interop runner binary in tests/e2e/moq-lite-matrix-plan.sh.
#
# Appends the invocation to $MOQ_STUB_DIR/calls.log in the same format as the --dry-run plan of
# tests/e2e/driven-moq-lite.sh (`runner: MOQ_CLI_BIN=<publisher> <runner> <arg>...`), records its
# --driver-log-root for tests/support/stub_curl_moq_lite/curl (which writes each run's retained
# request contract there), marks itself ready for the stub health check, and then idles until the
# driven script kills it. It never starts a publisher and opens no socket.
set -euo pipefail
{
    printf 'runner: MOQ_CLI_BIN=<%s> <%s>' "${MOQ_CLI_BIN:-}" "$0"
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
printf '%s\n' "$log_root" >"$MOQ_STUB_DIR/log-root-$port"
touch "$MOQ_STUB_DIR/ready-$port"
exec sleep 300
