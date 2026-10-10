#!/usr/bin/env bash
# Contract test of adapters/moq-lite/run.sh, the adapter for the moq CLI of moq-dev/moq (moq-lite-06).
#
# Stub `ffmpeg` and `moq` binaries first on PATH record their arguments and the MOQ_* environment
# they see in a record directory; the stub ffmpeg writes to its standard output (the pipe) and
# standard error (ffmpeg.log), the stub moq copies its standard input to the record directory and
# writes to both of its outputs (publisher.log). coreutils `timeout` is the real one. Further stubs
# sleep until signalled or ignore SIGTERM, to check the supervisor and its `timeout -k 2` bound with
# a group SIGTERM like the runner's. Needs bash, jq, coreutils `timeout` and python3; no network.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adapter="$root_dir/adapters/moq-lite/run.sh"
manifest="$root_dir/adapters/moq-lite/adapter.json"
test_dir=$(mktemp -d /tmp/moq-lite-adapter-contract.XXXXXX)
cleanup() {
    chmod -R u+w -- "$test_dir" 2>/dev/null || true
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
log_dir="$test_dir/log dir"
record="$test_dir/record"
bin="$test_dir/bin"
mkdir -p "$log_dir" "$record" "$bin"
touch "$test_dir/fixture.mp4"
printf -- '-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n' >"$test_dir/runner ca.pem"

fail() {
    printf 'moq-lite adapter contract: %s\n' "$1" >&2
    exit 1
}

namespace_ok='["696e7465726f702e68616e67"]'   # interop.hang
track_ok=302e6d3473                             # 0.m4s

# The stub ffmpeg: records its arguments, writes to the pipe and to its standard error.
cat >"$bin/ffmpeg" <<'STUB'
#!/usr/bin/env bash
printf '<%s>\n' "$@" >"$STUB_RECORD_DIR/ffmpeg"
printf 'ffmpeg stub: stderr\n' >&2
printf 'fmp4 bytes\n'
exit "${STUB_FFMPEG_EXIT:-0}"
STUB
# The stub moq: answers --version and --help like moq 0.14.1; otherwise records its arguments and
# MOQ_* environment, copies standard input and writes to both outputs.
cat >"$bin/moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf 'Usage: moq [FLAGS] <SUBCOMMAND>\n      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
printf '<%s>\n' "$@" >"$STUB_RECORD_DIR/moq"
env | grep '^MOQ_' | sort >"$STUB_RECORD_DIR/moq-env" || true
printf '%s\n' "${NO_COLOR-unset}" >"$STUB_RECORD_DIR/moq-no-color"
cat >"$STUB_RECORD_DIR/moq-stdin"
printf 'moq stub: stdout\n'
printf 'moq stub: stderr\n' >&2
exit "${STUB_MOQ_EXIT:-0}"
STUB
chmod +x "$bin/ffmpeg" "$bin/moq"
moq_stub="$bin/moq"

# make_request DRAFT_JSON TRANSPORT [ENDPOINT] [NAMESPACE_JSON] [TRACK_HEX] [TIMEOUT_MS]
make_request() {
    local draft=$1 transport=$2 endpoint=${3:-} namespace=${4:-$namespace_ok} track=${5-$track_ok}
    local timeout_ms=${6:-2500}
    if [[ -z "$endpoint" ]]; then
        if [[ "$transport" == webtransport ]]; then
            endpoint='https://127.0.0.1:4443/moq?token=l1d'
        else
            endpoint='moql://127.0.0.1:4443/moq?token=l1d'
        fi
    fi
    jq -n --arg endpoint "$endpoint" --arg transport "$transport" \
        --arg fixture "$test_dir/fixture.mp4" --arg ca "$test_dir/runner ca.pem" --arg log_dir "$log_dir" \
        --argjson namespace "$namespace" --arg track "$track" --argjson draft "$draft" \
        --argjson timeout_ms "$timeout_ms" '{
            schema_version: 1, run_id: "run 1", scenario_id: "l06-subscribe-latest",
            endpoint: $endpoint, draft: $draft, transport: $transport,
            namespace_hex: $namespace, track_name_hex: $track,
            fixture: $fixture, tls_ca: $ca, log_dir: $log_dir,
            scenario_timeout_ms: $timeout_ms, process_timeout_ms: (2 * $timeout_ms + 1000)
        }' >"$test_dir/request.json"
}
# edit_request JQ_FILTER: rewrites request.json.
edit_request() {
    jq "$1" "$test_dir/request.json" >"$test_dir/request.next"
    mv "$test_dir/request.next" "$test_dir/request.json"
}

# run_adapter [MOQ_CLI_BIN] [ENV_ASSIGNMENT...]: runs the adapter on request.json with the stubs
# first on PATH. Sets `status`, `out`, `err`, `log` (publisher.log), `ffmpeg_log`.
run_adapter() {
    local moq_bin=${1-$moq_stub}
    (($#)) && shift
    rm -f -- "$log_dir/publisher.log" "$log_dir/ffmpeg.log" "$record/"*
    set +e
    env -u MOQ_FFMPEG_BIN -u MOQ_LITE_TLS_INSECURE -u MOQ_LITE_TLS_ROOT -u NO_COLOR PATH="$bin:$PATH" MOQ_CLI_BIN="$moq_bin" \
        STUB_RECORD_DIR="$record" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
        MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$@" \
        "$adapter" >"$test_dir/out" 2>"$test_dir/err"
    status=$?
    set -e
    out=$(cat "$test_dir/out")
    err=$(cat "$test_dir/err")
    log=
    ffmpeg_log=
    [[ -f "$log_dir/publisher.log" ]] && log=$(cat "$log_dir/publisher.log")
    [[ -f "$log_dir/ffmpeg.log" ]] && ffmpeg_log=$(cat "$log_dir/ffmpeg.log")
    return 0
}

# expect_refused MESSAGE_FRAGMENT: exit 64, a message naming the problem, nothing started.
expect_refused() {
    local what=$1
    [[ "$status" -eq 64 ]] || fail "expected exit 64 for $what, got $status (stderr: $err)"
    [[ "$err" == "moq-lite adapter: "*"$what"* ]] || fail "refusal message for $what: $err"
    [[ ! -e "$log_dir/publisher.log" && ! -e "$log_dir/ffmpeg.log" ]] ||
        fail "the pipeline started although $what was refused"
    [[ ! -e "$record/moq" && ! -e "$record/ffmpeg" ]] || fail "a stub ran although $what was refused"
}

args_of() { tr '\n' ' ' <"$record/$1" | sed 's/ $//'; }

ffmpeg_args() {
    printf '<-hide_banner> <-v> <error> <-re> <-f> <lavfi> <-i> <testsrc2=size=640x360:rate=30> <-t> <%s> ' "$1"
    printf '<-c:v> <libx264> <-preset> <veryfast> <-tune> <zerolatency> <-profile:v> <baseline> '
    printf '<-pix_fmt> <yuv420p> <-b:v> <200k> <-maxrate> <200k> <-bufsize> <400k> <-g> <30> '
    printf '<-keyint_min> <30> <-sc_threshold> <0> <-f> <mp4> '
    printf '<-movflags> <cmaf+separate_moof+delay_moov+skip_trailer> <-frag_duration> <1000> <->'
}
# moq_args ENDPOINT [TLS_ARGS...]
moq_args() {
    local endpoint=$1 tls
    shift
    if (($#)); then tls=$(printf '<%s> ' "$@"); tls=${tls% }; else tls='<--connect-tls-insecure>'; fi
    printf '<--log-level> <debug> <--connect-version> <moq-lite-06> <--connect-once> <--connect-timeout> <10s> '
    printf '%s <--connect> <%s> <--broadcast> <interop.hang> <import> <fmp4>' "$tls" "$endpoint"
}

# The manifest and the run.sh constants agree, and the broadcast is the namespace field.
[[ "$(jq -c '.supported_namespace_hex' "$manifest")" == "$namespace_ok" &&
   "$(jq -r '.supported_track_name_hex' "$manifest")" == "$track_ok" &&
   "$(jq -c '.supported_drafts' "$manifest")" == '["moq-lite-06"]' &&
   "$(jq -r '.publisher_binary_environment' "$manifest")" == MOQ_CLI_BIN &&
   "$(jq -r '.adapter_id' "$manifest")" == moq-lite ]] || fail "adapter.json does not describe the adapter"
grep -qxF "readonly fixture_namespace_hex='$namespace_ok'" "$adapter" &&
    grep -qxF "readonly fixture_track_name_hex='$track_ok'" "$adapter" &&
    grep -qxF "readonly fixture_broadcast='interop.hang'" "$adapter" ||
    fail "the fixture constants at the top of run.sh differ from adapter.json"
[[ "$(printf 'interop.hang' | od -An -tx1 | tr -d ' \n')" == 696e7465726f702e68616e67 ]] ||
    fail 'the broadcast is not the namespace field'

# Accepted on both transports: the pipeline is ffmpeg | moq, the endpoint goes to --connect unchanged,
# ffmpeg's output reaches moq's standard input, and the logs are kept apart.
for transport in native_quic webtransport; do
    make_request '"moq-lite-06"' "$transport"
    endpoint=$(jq -r .endpoint "$test_dir/request.json")
    run_adapter
    [[ "$status" -eq 0 ]] || fail "$transport request failed with $status: $err"
    [[ -z "$out" ]] || fail "output was not redirected into log_dir: $out"
    [[ "$(args_of ffmpeg)" == "$(ffmpeg_args 6)" ]] || fail "$transport ffmpeg arguments: $(args_of ffmpeg)"
    [[ "$(args_of moq)" == "$(moq_args "$endpoint")" ]] || fail "$transport moq arguments: $(args_of moq)"
    [[ "$(cat "$record/moq-stdin")" == 'fmp4 bytes' ]] || fail "ffmpeg's output did not reach moq"
    [[ "$log" == $'moq stub: stdout\nmoq stub: stderr' || "$log" == $'moq stub: stderr\nmoq stub: stdout' ]] ||
        fail "publisher.log: $log"
    [[ "$ffmpeg_log" == 'ffmpeg stub: stderr' ]] || fail "ffmpeg.log: $ffmpeg_log"
    [[ ! -s "$record/moq-env" ]] || fail "moq saw MOQ_* variables: $(cat "$record/moq-env")"
    # NO_COLOR=1 (non-empty: an empty NO_COLOR keeps the colours) gives a publisher.log without ANSI codes.
    [[ "$(cat "$record/moq-no-color")" == 1 ]] || fail "moq ran with NO_COLOR=$(cat "$record/moq-no-color")"
done
# An operator's empty NO_COLOR is replaced too.
run_adapter "$moq_stub" NO_COLOR=
[[ "$status" -eq 0 && "$(cat "$record/moq-no-color")" == 1 ]] || fail "empty NO_COLOR reached moq"
# The source runs for the scenario timeout rounded up to whole seconds plus 3.
make_request '"moq-lite-06"' native_quic "" "$namespace_ok" "$track_ok" 9001
run_adapter
[[ "$status" -eq 0 && "$(args_of ffmpeg)" == "$(ffmpeg_args 13)" ]] || fail "ffmpeg -t for 9001 ms: $(args_of ffmpeg)"
make_request '"moq-lite-06"' native_quic "" "$namespace_ok" "$track_ok" 1
run_adapter
[[ "$status" -eq 0 && "$(args_of ffmpeg)" == "$(ffmpeg_args 4)" ]] || fail "ffmpeg -t for 1 ms: $(args_of ffmpeg)"

# The moq CLI's own MOQ_* variables (it reads every flag from one) never reach it.
make_request '"moq-lite-06"' native_quic
run_adapter "$moq_stub" MOQ_CONNECT_TLS_ROOT=/etc/hostname MOQ_HOP=7 MOQ_CONNECT=moql://elsewhere:1
[[ "$status" -eq 0 && ! -s "$record/moq-env" ]] || fail "MOQ_* variables reached moq: $(cat "$record/moq-env" 2>/dev/null)"

# Endpoints are passed through verbatim, whatever they contain besides whitespace and control bytes:
# nothing in them is evaluated.
for endpoint in 'moql://[::1]:4443/moq?token=l1d' 'moqt://127.0.0.1:4443/moq?token=l1d' \
                'moql://localhost:1/a/b?x=$(touch${IFS}'"$test_dir"'/pwned)&y=`id`;z='"'"'"' \
                'moql://127.0.0.1:4443'; do
    make_request '"moq-lite-06"' native_quic "$endpoint"
    run_adapter
    [[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$endpoint")" ]] ||
        fail "native endpoint $endpoint: $(args_of moq 2>/dev/null) $err"
done
[[ ! -e "$test_dir/pwned" ]] || fail 'the endpoint was evaluated'
for endpoint in 'https://[::1]:443/moq?token=l1d' 'https://relay.example:4443/other/path?a=1&b=2'; do
    make_request '"moq-lite-06"' webtransport "$endpoint"
    run_adapter
    [[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$endpoint")" ]] ||
        fail "WebTransport endpoint $endpoint: $(args_of moq 2>/dev/null) $err"
done

# TLS: --connect-tls-insecure by default, also with a tls_ca (the runner always fills it, and the
# CLI refuses its usual CA:TRUE self-signed certificate as a root: CaUsedAsEndEntity), and with an
# empty tls_ca. MOQ_LITE_TLS_ROOT=1 opts in to --connect-tls-root <tls_ca>, which must then be a
# non-empty readable file. MOQ_LITE_TLS_INSECURE=0/1 is an accepted no-op alias of the default.
wt='https://127.0.0.1:4443/moq?token=l1d'
make_request '"moq-lite-06"' webtransport
[[ "$(jq -r .tls_ca "$test_dir/request.json")" == "$test_dir/runner ca.pem" ]] || fail 'the request has no tls_ca'
for setting in MOQ_LITE_TLS_ROOT=0 MOQ_LITE_TLS_INSECURE=1 MOQ_LITE_TLS_INSECURE=0 MOQ_LITE_TLS_ROOT=; do
    run_adapter "$moq_stub" "$setting"
    [[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$wt")" ]] ||
        fail "default TLS with tls_ca and $setting: $(args_of moq 2>/dev/null) $err"
done
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=1
[[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$wt" --connect-tls-root "$test_dir/runner ca.pem")" ]] ||
    fail "MOQ_LITE_TLS_ROOT=1: $(args_of moq 2>/dev/null) $err"
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=1 MOQ_LITE_TLS_INSECURE=0
[[ "$status" -eq 0 && "$(args_of moq)" == *"<--connect-tls-root>"* ]] ||
    fail "MOQ_LITE_TLS_ROOT=1 MOQ_LITE_TLS_INSECURE=0: $(args_of moq 2>/dev/null) $err"
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=1 MOQ_LITE_TLS_INSECURE=1
expect_refused 'MOQ_LITE_TLS_ROOT=1 and MOQ_LITE_TLS_INSECURE=1 contradict each other'
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=yes
expect_refused 'MOQ_LITE_TLS_ROOT must be 0 or 1'
run_adapter "$moq_stub" MOQ_LITE_TLS_INSECURE=yes
expect_refused 'MOQ_LITE_TLS_INSECURE must be 0 or 1'
edit_request '.tls_ca = "'"$test_dir"'/missing.pem"'
run_adapter
[[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$wt")" ]] || fail "default TLS with a missing tls_ca: $err"
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=1
expect_refused 'tls_ca is not a readable file'
edit_request '.tls_ca = ""'
run_adapter
[[ "$status" -eq 0 && "$(args_of moq)" == "$(moq_args "$wt")" ]] || fail "default TLS with an empty tls_ca: $err"
run_adapter "$moq_stub" MOQ_LITE_TLS_ROOT=1
expect_refused 'MOQ_LITE_TLS_ROOT=1 needs a non-empty tls_ca'

# MOQ_FFMPEG_BIN names another ffmpeg; it must be executable.
make_request '"moq-lite-06"' native_quic
cp "$bin/ffmpeg" "$test_dir/other ffmpeg"
run_adapter "$moq_stub" MOQ_FFMPEG_BIN="$test_dir/other ffmpeg"
[[ "$status" -eq 0 && "$(args_of ffmpeg)" == "$(ffmpeg_args 6)" ]] || fail "MOQ_FFMPEG_BIN: $status $err"
run_adapter "$moq_stub" MOQ_FFMPEG_BIN="$test_dir/fixture.mp4"
expect_refused 'MOQ_FFMPEG_BIN must name an executable'

# Drafts: numeric drafts (MoQ Transport) and every other string are refused by name.
for draft in 18 21 22 106; do
    make_request "$draft" native_quic
    run_adapter
    expect_refused "draft $draft is not supported (supported drafts: moq-lite-06)"
done
for draft in moq-lite-05 moq-lite-07-wip MOQ-LITE-06 106 ''; do
    make_request "\"$draft\"" native_quic
    run_adapter
    expect_refused "draft \"$draft\" is not supported (supported drafts: moq-lite-06)"
done
make_request null native_quic
run_adapter
expect_refused 'draft is missing or not a string or number'

# Malformed requests and fields, each named.
printf '{"schema_version": 1, "draft": "moq-lite-06"' >"$test_dir/request.json"
run_adapter
expect_refused 'request is not a JSON object'
printf '[]' >"$test_dir/request.json"
run_adapter
expect_refused 'request is not a JSON object'
make_request '"moq-lite-06"' native_quic
edit_request '.schema_version = 2'
run_adapter
expect_refused 'schema_version must be 1'
for transport in quic native-quic web_transport; do
    make_request '"moq-lite-06"' "$transport" 'moql://127.0.0.1:4443/moq?token=l1d'
    run_adapter
    expect_refused 'transport must be native_quic or webtransport'
done
make_request '"moq-lite-06"' native_quic
edit_request '.run_id = ""'
run_adapter
expect_refused 'run_id'
make_request '"moq-lite-06"' native_quic
edit_request '.scenario_id = "l06-setup-stream\nx"'
run_adapter
expect_refused 'scenario_id'
make_request '"moq-lite-06"' native_quic
edit_request '.fixture = 5'
run_adapter
expect_refused 'fixture'
make_request '"moq-lite-06"' native_quic
edit_request 'del(.tls_ca)'
run_adapter
expect_refused 'tls_ca'

# Endpoint: scheme and transport must match; empty, multi-line, whitespace and authority-less
# endpoints are refused.
for endpoint in 'https://127.0.0.1:4443/moq?token=l1d' 'ws://127.0.0.1:4443/moq' 'quic://127.0.0.1:4443'; do
    make_request '"moq-lite-06"' native_quic "$endpoint"
    run_adapter
    expect_refused 'native QUIC requires a moql:// or moqt:// endpoint'
done
for endpoint in 'moql://127.0.0.1:4443/moq?token=l1d' 'moqt://127.0.0.1:4443/moq' 'http://127.0.0.1:4443/moq'; do
    make_request '"moq-lite-06"' webtransport "$endpoint"
    run_adapter
    expect_refused 'WebTransport requires an https:// endpoint'
done
make_request '"moq-lite-06"' native_quic
edit_request '.endpoint = ""'
run_adapter
expect_refused 'endpoint must be a non-empty single-line string'
make_request '"moq-lite-06"' native_quic
edit_request '.endpoint = "moql://127.0.0.1:4443/moq\nmoql://x:1"'
run_adapter
expect_refused 'endpoint must be a non-empty single-line string'
# A NUL or another control character inside the JSON string is refused before the shell reads it.
make_request '"moq-lite-06"' native_quic
edit_request '.endpoint = "moql://127.0.0.1:4443/a\u0000b"'
run_adapter
expect_refused 'endpoint must be a non-empty single-line string'
for endpoint in $'moql://127.0.0.1:4443/a\tb' $'moql://127.0.0.1:4443/a\x01' $'moql://127.0.0.1:4443/a\x7f'; do
    make_request '"moq-lite-06"' native_quic "$endpoint"
    run_adapter
    expect_refused 'endpoint must be a non-empty single-line string'
done
for endpoint in 'moql://127.0.0.1:4443/a b' $'moql://127.0.0.1:4443/a\xc2\xa0b' 'moql://héte:1/moq'; do
    make_request '"moq-lite-06"' native_quic "$endpoint"
    [[ "$endpoint" == *'é'* ]] && edit_request '.endpoint = "moql://héte:1/moq"'
    run_adapter
    expect_refused 'endpoint contains whitespace, control or non-ASCII characters'
done
for endpoint in 'moql:///moq' 'moql://' 'moql://?token=l1d'; do
    make_request '"moq-lite-06"' native_quic "$endpoint"
    run_adapter
    expect_refused 'endpoint has no authority'
done

# The fixture: exactly namespace ["interop.hang"] and track 0.m4s.
for namespace in '["6d65646961"]' '["696e7465726f70", "68616e67"]' '[]' \
                 '["696e7465726f702e68616e67", "696e7465726f702e68616e67"]' '"696e7465726f702e68616e67"'; do
    make_request '"moq-lite-06"' native_quic "" "$namespace"
    run_adapter
    expect_refused 'namespace_hex must be ["696e7465726f702e68616e67"] (interop.hang)'
done
for track in 766964655f31 '' 312e6d3473 302e6d34732d; do
    make_request '"moq-lite-06"' native_quic "" "$namespace_ok" "$track"
    run_adapter
    expect_refused 'track_name_hex must be 302e6d3473 (0.m4s)'
done

# Timeouts: plain integers only; the scenario timeout within 1..3600000.
for literal in 2500.0 1e3 '"2500"' 0 3600001 -5 null; do
    make_request '"moq-lite-06"' native_quic
    edit_request ".scenario_timeout_ms = $literal"
    # jq may rewrite 2500.0 and 1e3; put the literal back as written.
    sed -i -E "s/\"scenario_timeout_ms\": [^,]*,/\"scenario_timeout_ms\": $literal,/" "$test_dir/request.json"
    grep -qF "\"scenario_timeout_ms\": $literal," "$test_dir/request.json" || fail "could not write $literal"
    run_adapter
    expect_refused 'scenario_timeout_ms must be a whole number of milliseconds from 1 to 3600000'
done
for literal in 0 1.5 '"6000"'; do
    make_request '"moq-lite-06"' native_quic
    edit_request ".process_timeout_ms = $literal"
    run_adapter
    expect_refused 'process_timeout_ms must be a whole number of milliseconds of at least 1'
done

# log_dir: missing, not a directory, not writable.
make_request '"moq-lite-06"' native_quic
edit_request '.log_dir = "'"$test_dir"'/missing"'
run_adapter
expect_refused 'log_dir is not a writable directory'
make_request '"moq-lite-06"' native_quic
edit_request '.log_dir = "'"$test_dir"'/fixture.mp4"'
run_adapter
expect_refused 'log_dir is not a writable directory'
mkdir "$test_dir/read-only"
chmod 0555 "$test_dir/read-only"
if [[ ! -w "$test_dir/read-only" ]]; then
    make_request '"moq-lite-06"' native_quic
    edit_request '.log_dir = "'"$test_dir"'/read-only"'
    run_adapter
    expect_refused 'log_dir is not a writable directory'
fi

# The contract version and the request file.
make_request '"moq-lite-06"' native_quic
run_adapter "$moq_stub" MOQ_INTEROP_DRIVER_CONTRACT_VERSION=2
expect_refused 'unsupported driver contract version'
run_adapter "$moq_stub" MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/missing.json"
expect_refused 'request file is unavailable'

# The publisher binary: missing, not executable, and binaries that are not the current moq CLI.
run_adapter ""
expect_refused 'MOQ_CLI_BIN must name an executable moq CLI'
run_adapter "$test_dir/fixture.mp4"
expect_refused 'MOQ_CLI_BIN must name an executable moq CLI'
run_adapter "$test_dir"
expect_refused 'MOQ_CLI_BIN must name an executable moq CLI'
# The March 2026 moq-cli: no --version (clap usage error, exit 2) and a help without --connect-version.
cat >"$test_dir/old moq-cli" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --help) printf 'Usage: moq-cli [OPTIONS] <COMMAND>\nCommands:\n  serve\n  publish\n'; exit 0 ;;
    *) printf "error: unexpected argument '%s' found\n" "${1:-}" >&2; exit 2 ;;
esac
STUB
# A binary that prints a moq-like version but whose help lacks --connect-version.
cat >"$test_dir/no-connect-version moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.9.0\n'; exit 0 ;;
    --help) printf 'Usage: moq [FLAGS] <SUBCOMMAND>\n      --client-version <V>\n'; exit 0 ;;
esac
exit 0
STUB
# Another tool's version line.
cat >"$test_dir/moq-cli versioned" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq-cli 0.14.1\n'; exit 0 ;;
    --help) printf '      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
exit 0
STUB
chmod +x "$test_dir/old moq-cli" "$test_dir/no-connect-version moq" "$test_dir/moq-cli versioned"
make_request '"moq-lite-06"' native_quic
run_adapter "$test_dir/old moq-cli"
expect_refused "is not the current moq CLI: --version did not print 'moq <version>'"
run_adapter "$test_dir/moq-cli versioned"
expect_refused "is not the current moq CLI: --version did not print 'moq <version>'"
run_adapter "$test_dir/no-connect-version moq"
expect_refused 'is not the current moq CLI: its --help does not mention --connect-version'

# ffmpeg and timeout must be on PATH: a PATH with only bash, env, jq and the stubs lacks one of them.
for missing in ffmpeg timeout; do
    rm -rf -- "$test_dir/minimal"
    mkdir "$test_dir/minimal"
    for tool in bash env jq; do ln -s "$(command -v "$tool")" "$test_dir/minimal/$tool"; done
    ln -s "$bin/moq" "$test_dir/minimal/moq"
    [[ "$missing" == ffmpeg ]] && ln -s "$(command -v timeout)" "$test_dir/minimal/timeout"
    [[ "$missing" == timeout ]] && ln -s "$bin/ffmpeg" "$test_dir/minimal/ffmpeg"
    rm -f -- "$log_dir/publisher.log" "$log_dir/ffmpeg.log" "$record/"*
    set +e
    env -i PATH="$test_dir/minimal" MOQ_CLI_BIN="$bin/moq" STUB_RECORD_DIR="$record" \
        MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" \
        "$test_dir/minimal/bash" "$adapter" >"$test_dir/out" 2>"$test_dir/err"
    status=$?
    set -e
    err=$(cat "$test_dir/err")
    if [[ "$missing" == ffmpeg ]]; then
        expect_refused 'ffmpeg is required on PATH (or MOQ_FFMPEG_BIN)'
    else
        expect_refused 'coreutils timeout is required'
    fi
done

# Exit status: moq's own; a failing ffmpeg with a successful moq is reported on stderr only.
make_request '"moq-lite-06"' native_quic
run_adapter "$moq_stub" STUB_MOQ_EXIT=7
[[ "$status" -eq 7 ]] || fail "moq exiting 7 gave $status"
run_adapter "$moq_stub" STUB_FFMPEG_EXIT=1
[[ "$status" -eq 0 && "$err" == *"moq-lite adapter: ffmpeg exited with status 1"* ]] ||
    fail "ffmpeg exiting 1 under a successful moq gave $status: $err"
run_adapter "$moq_stub" STUB_FFMPEG_EXIT=1 STUB_MOQ_EXIT=3
[[ "$status" -eq 3 && "$err" == *"moq-lite adapter: ffmpeg exited with status 1"* ]] ||
    fail "moq exiting 3 with ffmpeg 1 gave $status: $err"

# moq ending first (the runner closed the session) ends ffmpeg too: it is writing into a closed pipe.
cat >"$test_dir/endless ffmpeg" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$$" >"$STUB_RECORD_DIR/ffmpeg-pid"
# Ends on SIGPIPE, or on the write error when SIGPIPE is ignored (as ffmpeg itself does).
while printf 'fmp4 bytes\n'; do sleep 0.05; done
exit 1
STUB
cat >"$test_dir/quick moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf '      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
head -c 1 >/dev/null
exit 0
STUB
chmod +x "$test_dir/endless ffmpeg" "$test_dir/quick moq"
started=$SECONDS
run_adapter "$test_dir/quick moq" MOQ_FFMPEG_BIN="$test_dir/endless ffmpeg"
(($SECONDS - started <= 3)) || fail "ffmpeg outlived moq by $((SECONDS - started)) s"
[[ "$status" -eq 0 ]] || fail "moq exiting first gave $status: $err"
! kill -0 "$(cat "$record/ffmpeg-pid")" 2>/dev/null || fail 'ffmpeg kept running after moq exited'

# A moq that never exits on its own (the source has ended) is sent SIGTERM by `timeout` at the source
# duration plus 2 s (timeout 1000 ms: a 4 s source, an 6 s deadline) and its own status 0 is kept.
cat >"$test_dir/lingering moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf '      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
cat >/dev/null
sleep 30 &
child=$!
trap 'kill "$child" 2>/dev/null; printf "moq stub: SIGTERM\n"; exit 0' TERM
wait "$child"
exit 3
STUB
chmod +x "$test_dir/lingering moq"
make_request '"moq-lite-06"' native_quic "" "$namespace_ok" "$track_ok" 1000
started=$SECONDS
run_adapter "$test_dir/lingering moq"
elapsed=$((SECONDS - started))
[[ "$status" -eq 0 && "$log" == *"moq stub: SIGTERM"* ]] || fail "lingering moq: status $status, log $log"
((elapsed >= 5 && elapsed <= 9)) || fail "lingering moq ended after ${elapsed}s (expected about 6s)"

# Shutdown by the runner: SIGTERM to the adapter's process group, SIGKILL 100 ms later (recorded as a
# driver failure). group_term.py mirrors that: it starts the adapter in a new session, waits for the
# stubs' "started" lines, sends SIGTERM to the group, polls the adapter's exit every millisecond and
# then waits until no process of the group is left. It prints `adapter_ms status group_ms`.
cat >"$test_dir/group_term.py" <<'PY'
import os, signal, subprocess, sys, time
adapter, out, err = sys.argv[1:4]
limit = float(sys.argv[4])
markers = [(sys.argv[i], sys.argv[i + 1]) for i in range(5, len(sys.argv), 2)]
def group_alive(pgid):
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            with open(f'/proc/{entry}/stat') as f:
                fields = f.read().rsplit(')', 1)[1].split()
        except OSError:
            continue
        if fields[0] != 'Z' and int(fields[2]) == pgid:
            return True
    return False
def started():
    for path, marker in markers:
        try:
            with open(path) as f:
                if marker not in f.read():
                    return False
        except OSError:
            return False
    return True
with open(out, 'wb') as o, open(err, 'wb') as e:
    p = subprocess.Popen([adapter], stdin=subprocess.DEVNULL, stdout=o, stderr=e,
                         start_new_session=True)
pgid = p.pid
deadline = time.monotonic() + 10
while not started():
    if time.monotonic() > deadline or p.poll() is not None:
        os.killpg(pgid, signal.SIGKILL)
        print('-1 not-started -1')
        sys.exit(0)
    time.sleep(0.005)
time.sleep(0.05)
t0 = time.monotonic()
os.killpg(pgid, signal.SIGTERM)
adapter_ms, status = -1, 'running'
while time.monotonic() - t0 < limit:
    code = p.poll()
    if code is not None:
        adapter_ms, status = int((time.monotonic() - t0) * 1000), code
        break
    time.sleep(0.001)
group_ms = -1
while time.monotonic() - t0 < limit:
    if not group_alive(pgid):
        group_ms = int((time.monotonic() - t0) * 1000)
        break
    time.sleep(0.001)
if group_ms < 0:
    os.killpg(pgid, signal.SIGKILL)
if p.poll() is None:
    p.kill()
p.wait()
print(adapter_ms, status, group_ms)
PY

# group_term MOQ_BIN FFMPEG_BIN LIMIT_S: sets adapter_ms, status, group_ms, log, ffmpeg_log.
group_term() {
    rm -f -- "$log_dir/publisher.log" "$log_dir/ffmpeg.log" "$record/"*
    read -r adapter_ms status group_ms < <(env -u MOQ_LITE_TLS_INSECURE -u MOQ_LITE_TLS_ROOT -u NO_COLOR PATH="$bin:$PATH" \
        MOQ_CLI_BIN="$1" MOQ_FFMPEG_BIN="$2" STUB_RECORD_DIR="$record" \
        MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" \
        python3 "$test_dir/group_term.py" "$adapter" "$test_dir/out" "$test_dir/err" "$3" \
        "$log_dir/publisher.log" 'moq stub: started' "$log_dir/ffmpeg.log" 'ffmpeg stub: started') ||
        fail 'group_term.py gave no result'
    log=
    ffmpeg_log=
    [[ -f "$log_dir/publisher.log" ]] && log=$(cat "$log_dir/publisher.log")
    [[ -f "$log_dir/ffmpeg.log" ]] && ffmpeg_log=$(cat "$log_dir/ffmpeg.log")
    return 0
}
# no_orphans: the pids the stubs recorded are gone.
no_orphans() {
    local name pid
    for name in moq ffmpeg; do
        pid=$(cat "$record/$name-pid")
        ! kill -0 "$pid" 2>/dev/null || fail "the $name stub (pid $pid) outlived the group SIGTERM"
    done
}

# Stubs that end at once on SIGTERM, as moq (exit 0 on SIGTERM) and ffmpeg do.
cat >"$test_dir/prompt moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf '      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
printf '%s\n' "$$" >"$STUB_RECORD_DIR/moq-pid"
sleep 30 &
child=$!
trap 'kill "$child" 2>/dev/null; printf "moq stub: SIGTERM\n" >&2; exit 0' TERM
printf 'moq stub: started\n' >&2
wait "$child"
exit 3
STUB
cat >"$test_dir/prompt ffmpeg" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$$" >"$STUB_RECORD_DIR/ffmpeg-pid"
sleep 30 &
child=$!
trap 'kill "$child" 2>/dev/null; printf "ffmpeg stub: SIGTERM\n" >&2; exit 255' TERM
printf 'ffmpeg stub: started\n' >&2
wait "$child"
exit 3
STUB
chmod +x "$test_dir/prompt moq" "$test_dir/prompt ffmpeg"
make_request '"moq-lite-06"' native_quic "" "$namespace_ok" "$track_ok" 10000
# The adapter's exit and the group's end are usually a few ms after the signal; the 100 ms grace is
# checked with up to three attempts so that one scheduling stall on a loaded machine cannot fail the
# test. Everything else is checked on every attempt.
fast=0
for attempt in 1 2 3; do
    group_term "$test_dir/prompt moq" "$test_dir/prompt ffmpeg" 6
    [[ "$status" == 0 ]] || fail "adapter status after the group SIGTERM: $status (attempt $attempt)"
    ((group_ms >= 0 && group_ms <= 1500)) || fail "the process group outlived the SIGTERM by ${group_ms} ms"
    [[ "$log" == *"moq stub: SIGTERM"* && "$ffmpeg_log" == *"ffmpeg stub: SIGTERM"* ]] ||
        fail "the stubs were not sent SIGTERM: $log / $ffmpeg_log"
    no_orphans
    if ((adapter_ms >= 0 && adapter_ms < 100 && group_ms < 100)); then
        fast=1
        break
    fi
    printf 'moq-lite adapter contract: attempt %s: adapter exited after %s ms, group gone after %s ms\n' \
        "$attempt" "$adapter_ms" "$group_ms" >&2
done
((fast)) || fail "ffmpeg and moq did not end within the runner's 100 ms grace (last: adapter ${adapter_ms} ms, group ${group_ms} ms)"
printf 'group SIGTERM: adapter exited after %s ms, ffmpeg and moq gone after %s ms\n' "$adapter_ms" "$group_ms"

# Stubs that ignore SIGTERM are killed by `timeout -k 2` about 2 s after the signal; the adapter still
# exits 0 at once and nothing is left behind.
cat >"$test_dir/deaf moq" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf 'moq 0.14.1\n'; exit 0 ;;
    --help) printf '      --connect-version <CONNECT_VERSION>\n'; exit 0 ;;
esac
trap '' TERM
printf '%s\n' "$$" >"$STUB_RECORD_DIR/moq-pid"
printf 'moq stub: started\n' >&2
exec sleep 30
STUB
cat >"$test_dir/deaf ffmpeg" <<'STUB'
#!/usr/bin/env bash
trap '' TERM
printf '%s\n' "$$" >"$STUB_RECORD_DIR/ffmpeg-pid"
printf 'ffmpeg stub: started\n' >&2
exec sleep 30
STUB
chmod +x "$test_dir/deaf moq" "$test_dir/deaf ffmpeg"
group_term "$test_dir/deaf moq" "$test_dir/deaf ffmpeg" 8
[[ "$status" == 0 && "$adapter_ms" -ge 0 && "$adapter_ms" -lt 1000 ]] ||
    fail "adapter with stubs ignoring SIGTERM: status $status after ${adapter_ms} ms"
((group_ms >= 1500 && group_ms <= 4500)) ||
    fail "stubs ignoring SIGTERM ended ${group_ms} ms after it (expected timeout -k 2: about 2000)"
no_orphans
printf 'group SIGTERM: adapter exited after %s ms, stubs ignoring SIGTERM were killed after %s ms\n' \
    "$adapter_ms" "$group_ms"

printf 'moq-lite adapter contract passed\n'
