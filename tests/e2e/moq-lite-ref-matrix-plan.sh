#!/usr/bin/env bash
# Pin the plan of tests/e2e/moq-lite-ref-matrix.sh (its --dry-run output) and of the driven script for the reference
# adapter. Usage: moq-lite-ref-matrix-plan.sh [--update]
set -euo pipefail
export LC_ALL=C
update=0
[[ "${1:-}" == --update || "${MOQ_UPDATE_GOLDEN:-}" == 1 ]] && update=1
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
golden="$root_dir/tests/golden/moq-lite-ref-matrix-plan.txt"
actual=$({
    bash "$root_dir/tests/e2e/moq-lite-ref-matrix.sh" --dry-run /nonexistent/moq-interop-runner
    MOQ_LITE_ADAPTER=moq-lite-ref MOQ_LITE_REF_BIN=/nonexistent/ref \
        bash "$root_dir/tests/e2e/driven-moq-lite.sh" --dry-run native_quic /nonexistent/moq-interop-runner l06-setup-stream |
        sed "s|$root_dir|@ROOT@|g"
})
if ((update)); then printf '%s\n' "$actual" >"$golden"; printf 'updated %s\n' "$golden"; exit 0; fi
diff -u "$golden" <(printf '%s\n' "$actual") || { printf 'plan differs from %s (rerun with --update after review)\n' "$golden" >&2; exit 1; }
printf 'moq-lite-ref matrix plan: ok\n'
