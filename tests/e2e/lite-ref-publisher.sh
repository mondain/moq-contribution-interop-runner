#!/usr/bin/env bash
# The reference publisher's process contract (L2c): exit codes and the single JSON line.
#   $1  the built moq-interop-lite-ref-publisher
set -euo pipefail
bin=${1:?usage: lite-ref-publisher.sh PUBLISHER_BIN}

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# --help: exit 0, usage on stdout naming a defect.
out=$("$bin" --help) || fail "--help exited non-zero"
grep -q 'datagram-oversize' <<<"$out" || fail "--help does not list the defects"

# Usage errors: exit 2, nothing on stdout, a message on stderr.
for args in "" "--defect bogus --connect moql://127.0.0.1:1/moq" "--connect ftp://x/moq" "--connect moql://127.0.0.1:1/moq --probe-level loud"; do
    set +e
    # shellcheck disable=SC2086
    stdout=$("$bin" $args 2>/tmp/lite-ref-stderr.$$)
    code=$?
    set -e
    ((code == 2)) || fail "args '$args' exited $code, expected 2"
    [[ -z $stdout ]] || fail "args '$args' printed on stdout"
    [[ -s /tmp/lite-ref-stderr.$$ ]] || fail "args '$args' printed no message"
done
rm -f /tmp/lite-ref-stderr.$$

# Nothing listening: the handshake never completes, exit 1 within 15 s, never a conforming run.
start=$SECONDS
set +e
"$bin" --connect moql://127.0.0.1:9/moq?token=none >/dev/null 2>&1
code=$?
set -e
((code == 1)) || fail "an unreachable runner exited $code, expected 1"
((SECONDS - start < 15)) || fail "an unreachable runner took $((SECONDS - start)) s"
printf 'lite-ref-publisher: ok\n'
