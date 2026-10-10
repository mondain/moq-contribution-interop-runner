#!/usr/bin/env bash
# Driver adapter for the moq CLI of moq-dev/moq (rs/moq-cli, binary `moq`, 0.14.1 at b8b0d235) at
# moq-lite-06. It validates the runner's request and runs, as a small supervisor (see the end of
# this file), the pipeline
#
#   ffmpeg <test pattern -> fragmented MP4 on stdout> | moq <dial options> --broadcast NAME import fmp4
#
# with ffmpeg's messages in <log_dir>/ffmpeg.log and moq's output in <log_dir>/publisher.log. It
# describes the CLI's command line only; it never decides what the runner expects. Every one of the
# 19 executable moq-lite-06 scenarios gets the same command line: the scenarios differ only in what
# the runner does (tests/golden/moq-lite-cmdlines.txt).
#
# moq's command line (dial flags before the verb):
#   --log-level debug              logging, to standard error (RUST_LOG, if set, overrides it)
#   --connect-version moq-lite-06  offer moq-lite-06 only (ALPN moq-lite-06 on raw QUIC; the
#                                  WebTransport protocol moq-lite-06 on https)
#   --connect-once                 dial once, never redial: one session per context
#   --connect-timeout 10s          give up dialing after 10 s (exit 1)
#   --connect-tls-insecure         do not verify the runner's certificate (the default); with
#     or --connect-tls-root FILE   MOQ_LITE_TLS_ROOT=1 the request's tls_ca is trusted instead
#                                  (see the TLS block below)
#   --connect URL                  the request's endpoint, VERBATIM: moql:// or moqt:// is raw QUIC
#                                  (the path and query go into the SETUP Path parameter), https:// is
#                                  WebTransport (the path and query are the CONNECT :path)
#   --broadcast NAME               the broadcast it announces (the pinned fixture below)
#   import fmp4                    read fragmented MP4 from standard input; a new group at each
#                                  video sync sample, tracks catalog.json, catalog and <id>.m4s
# It has no duration flag. It exits 0 on standard input EOF (the source ended), SIGINT or SIGTERM,
# and when the session closes; 1 when the dial fails; 2 on a usage error.
# Every flag can also come from a MOQ_* environment variable (MOQ_CONNECT, MOQ_HOP, ...): the
# adapter removes all MOQ_* variables from the pipeline's environment, so only this command line
# configures it.
#
# The moq-cli of March 2026 (`moq-cli serve|publish`) is a different, older tool: it has no
# --version and no --connect-version, and is refused (exit 64).
set -euo pipefail

# The track fixture: the only broadcast and track this adapter publishes. PROVISIONAL until the live
# smoke (L1e Task 4) pins them; change these three lines, adapters/moq-lite/adapter.json and the
# goldens together. The broadcast is the namespace field (one field; a broadcast path joins the
# fields with "/"); the track name is not passed to the CLI (the CLI names its media tracks
# <id>.m4s itself), it only names the track the runner subscribes to.
readonly fixture_namespace_hex='["696e7465726f702e68616e67"]'
readonly fixture_track_name_hex='302e6d3473'
readonly fixture_broadcast='interop.hang'

fail() {
    printf 'moq-lite adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -f "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
moq_bin=${MOQ_CLI_BIN:-}
[[ -n "$moq_bin" && -f "$moq_bin" && -x "$moq_bin" ]] ||
    fail 'MOQ_CLI_BIN must name an executable moq CLI (moq-dev/moq rs/moq-cli, binary moq)'
command -v timeout >/dev/null || fail 'coreutils timeout is required'
if [[ -n "${MOQ_FFMPEG_BIN:-}" ]]; then
    ffmpeg_bin=$MOQ_FFMPEG_BIN
    [[ -f "$ffmpeg_bin" && -x "$ffmpeg_bin" ]] || fail 'MOQ_FFMPEG_BIN must name an executable ffmpeg'
else
    ffmpeg_bin=$(command -v ffmpeg) || fail 'ffmpeg is required on PATH (or MOQ_FFMPEG_BIN)'
fi
case "${MOQ_LITE_TLS_ROOT:-}" in
    '' | 0) use_tls_root=0 ;;
    1) use_tls_root=1 ;;
    *) fail 'MOQ_LITE_TLS_ROOT must be 0 or 1' ;;
esac
# MOQ_LITE_TLS_INSECURE: the former opt-out, now the default; accepted as a no-op alias.
case "${MOQ_LITE_TLS_INSECURE:-}" in
    '' | 0) ;;
    1) ((!use_tls_root)) || fail 'MOQ_LITE_TLS_ROOT=1 and MOQ_LITE_TLS_INSECURE=1 contradict each other' ;;
    *) fail 'MOQ_LITE_TLS_INSECURE must be 0 or 1' ;;
esac
# Everything this adapter reads from the environment is read; the CLI's own MOQ_* variables (and the
# ones above) must not configure the publisher.
for name in $(compgen -e); do
    if [[ "$name" == MOQ_* ]]; then
        unset "$name"
    fi
done
# Plain logs: the CLI's tracing output has no ANSI colour codes with NO_COLOR set to a non-empty
# value (an empty NO_COLOR does not disable them).
export NO_COLOR=1

# The request. Exactly one JSON object; every field is checked on its own, so a refusal names it.
jq -en '[inputs] | length == 1 and (.[0] | type == "object")' "$request_file" >/dev/null 2>&1 ||
    fail 'request is not a JSON object'
case "$(jq -r '.draft | type' "$request_file")" in
    number) fail "draft $(jq -r '.draft' "$request_file") is not supported (supported drafts: moq-lite-06)" ;;
    string)
        jq -e '.draft == "moq-lite-06"' "$request_file" >/dev/null ||
            fail "draft $(jq -c '.draft' "$request_file") is not supported (supported drafts: moq-lite-06)" ;;
    *) fail 'draft is missing or not a string or number' ;;
esac
# require FILTER MESSAGE: refuses unless FILTER is true for the request.
require() {
    jq -e --argjson namespace "$fixture_namespace_hex" --arg track "$fixture_track_name_hex" \
        "$1" "$request_file" >/dev/null 2>&1 || fail "$2"
}
# A string jq hands to the shell: no control characters (newline, NUL, which $(...) would drop).
plain='type == "string" and (test("[[:cntrl:]]") | not)'
require '.schema_version == 1' 'schema_version must be 1'
require '.transport == "native_quic" or .transport == "webtransport"' \
    'transport must be native_quic or webtransport'
require ".run_id | $plain and length > 0" 'run_id must be a non-empty single-line string'
require ".scenario_id | $plain and length > 0" 'scenario_id must be a non-empty single-line string'
require ".endpoint | $plain and length > 0" 'endpoint must be a non-empty single-line string'
require ".fixture | $plain" 'fixture must be a single-line string'
require ".tls_ca | $plain" 'tls_ca must be a single-line string'
require ".log_dir | $plain and length > 0" 'log_dir must be a non-empty single-line string'
# jq passes number literals through unchanged (2500.0, 1E+3), which the shell arithmetic below cannot
# use: the timeouts must be written as plain integers.
require '.scenario_timeout_ms | type == "number" and (tostring | test("^[0-9]+$")) and . >= 1 and . <= 3600000' \
    'scenario_timeout_ms must be a whole number of milliseconds from 1 to 3600000'
require '.process_timeout_ms | type == "number" and (tostring | test("^[0-9]+$")) and . >= 1' \
    'process_timeout_ms must be a whole number of milliseconds of at least 1'
require '.namespace_hex == $namespace' \
    "namespace_hex must be $fixture_namespace_hex ($fixture_broadcast): the only broadcast this adapter publishes"
require '.track_name_hex == $track' \
    "track_name_hex must be $fixture_track_name_hex (0.m4s): the media track the moq CLI names for the first video track"

transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
log_dir=$(jq -r '.log_dir' "$request_file")
tls_ca=$(jq -r '.tls_ca' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")
# The fixture (an MP4 path) is not used: the source is ffmpeg's test pattern. The scenario id is not
# used either: the command line is the same for every scenario.

# The endpoint goes to --connect unchanged (the runner builds moql://HOST:PORT/moq?token=l1d or
# https://HOST:PORT/moq?token=l1d, whose path and query rows 120, 124 and 125 judge). Only its
# scheme is matched with the transport; it is never rewritten.
if [[ "$transport" == webtransport ]]; then
    [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an https:// endpoint'
else
    [[ "$endpoint" == moql://* || "$endpoint" == moqt://* ]] ||
        fail 'native QUIC requires a moql:// or moqt:// endpoint'
fi
# A URI is printable ASCII (code points 0x21-0x7e, checked by jq, independent of the locale).
require '.endpoint | test("^[!-~]+$")' 'endpoint contains whitespace, control or non-ASCII characters'
authority=${endpoint#*://}
authority=${authority%%[/?#]*}
[[ -n "$authority" ]] || fail 'endpoint has no authority (host and port)'
[[ -d "$log_dir" && -w "$log_dir" ]] || fail 'log_dir is not a writable directory'

# TLS. The runner's listener presents its --tls-cert; the request's tls_ca is --driver-ca, else
# that certificate, so the runner always fills it in. The default is --connect-tls-insecure,
# whatever tls_ca says: the runner's usual certificate is self-signed with CA:TRUE (openssl req
# -x509), and the CLI's verifier (rustls-webpki) refuses a CA certificate as the server's own
# ("invalid peer certificate: CaUsedAsEndEntity"), so --connect-tls-root with it fails every
# handshake. MOQ_LITE_TLS_ROOT=1 opts in to --connect-tls-root <tls_ca> (it replaces the system
# roots) for a listener whose certificate chains to a real CA file; tls_ca must then be a readable
# file.
if ((use_tls_root)); then
    [[ -n "$tls_ca" ]] || fail 'MOQ_LITE_TLS_ROOT=1 needs a non-empty tls_ca'
    [[ -f "$tls_ca" && -r "$tls_ca" ]] || fail 'tls_ca is not a readable file'
    tls_args=(--connect-tls-root "$tls_ca")
else
    tls_args=(--connect-tls-insecure)
fi

# The binary must be the current moq CLI. Checked last, so a refused request runs nothing.
version=$("$moq_bin" --version </dev/null 2>/dev/null) || version=
version_line=
while IFS= read -r line; do
    if [[ "$line" =~ ^moq\ [^[:space:]]+ ]]; then
        version_line=$line
        break
    fi
done <<<"$version"
[[ -n "$version_line" ]] ||
    fail "MOQ_CLI_BIN ($moq_bin) is not the current moq CLI: --version did not print 'moq <version>' (the old moq-cli is not supported; see adapters/moq-lite/README.md)"
help_text=$("$moq_bin" --help </dev/null 2>&1) || help_text=
[[ "$help_text" == *--connect-version* ]] ||
    fail "MOQ_CLI_BIN ($moq_bin) is not the current moq CLI: its --help does not mention --connect-version"
printf 'moq-lite adapter: publisher %s (%s)\n' "$version_line" "$moq_bin" >&2

# The source: a 640x360 30 fps test pattern, H.264 baseline at about 200 kbit/s (200-380 measured),
# a 1 s GOP (-g 30, no scene-cut keyframes) so the CLI starts a group every second, video only (the
# CLI rejects per-frame audio/video interleaving), as fragmented MP4 on standard output, in real time
# (-re). The CLI has no duration flag: ffmpeg ends after the scenario timeout rounded up to whole
# seconds plus 3 s, which gives moq its standard input EOF and a graceful exit 0. The runner waits up
# to the timeout for the connection and runs each probe inside the timeout, so the source outlasts a
# context whose publisher connects within 3 s; the runner normally stops the pipeline first.
timeout_seconds=$(((timeout_ms + 999) / 1000))
((timeout_seconds > 0)) || fail 'invalid scenario timeout'
source_seconds=$((timeout_seconds + 3))
ffmpeg_args=(
    -hide_banner -v error -re -f lavfi -i testsrc2=size=640x360:rate=30 -t "$source_seconds"
    -c:v libx264 -preset veryfast -tune zerolatency -profile:v baseline -pix_fmt yuv420p
    -b:v 200k -maxrate 200k -bufsize 400k -g 30 -keyint_min 30 -sc_threshold 0
    -f mp4 -movflags cmaf+separate_moof+delay_moov+skip_trailer -frag_duration 1000 -
)
moq_args=(
    --log-level debug --connect-version moq-lite-06 --connect-once --connect-timeout 10s
    "${tls_args[@]}" --connect "$endpoint" --broadcast "$fixture_broadcast" import fmp4
)
# Each side runs under `timeout`, 2 s after the source's end: a moq that does not exit after its
# EOF, or an ffmpeg that hangs, is sent SIGTERM then, and SIGKILL 2 s after any signal (-k 2, also
# after the runner's SIGTERM, which `timeout` relays). --foreground keeps `timeout` in the runner's
# process group, so the group signal reaches ffmpeg and moq directly; --preserve-status reports the
# child's own status (moq exits 0 on SIGTERM) instead of 124.
supervise=(timeout --foreground --preserve-status -k 2 -s TERM "$((source_seconds + 2))")

# The pipeline. ffmpeg's standard input is /dev/null (it would read commands from it; the adapter's
# own standard input must not be read). The exit status is moq's: the runner judges the wire, and
# ffmpeg exits nonzero whenever moq ended first (the runner closed the session; ffmpeg's next write
# fails), so ffmpeg's status cannot tell a failed run. A nonzero ffmpeg status is reported on the
# adapter's standard error (the runner's stderr.bin), its messages are in ffmpeg.log.
publish() {
    set +e +o pipefail
    "${supervise[@]}" "$ffmpeg_bin" "${ffmpeg_args[@]}" </dev/null 2>"$log_dir/ffmpeg.log" |
        "${supervise[@]}" "$moq_bin" "${moq_args[@]}" >"$log_dir/publisher.log" 2>&1
    local statuses=("${PIPESTATUS[@]}")
    if ((statuses[0] != 0)); then
        printf 'moq-lite adapter: ffmpeg exited with status %s (see ffmpeg.log)\n' "${statuses[0]}" >&2
    fi
    exit "${statuses[1]}"
}

# Supervisor. The runner stops the adapter with SIGTERM to its process group and SIGKILLs the group
# 100 ms later, recording the SIGKILL as a driver failure (run error). So this script does not exec
# the pipeline: it runs it in the background, in the same process group, and
#   - on SIGTERM, SIGINT or SIGHUP exits 0 at once: the group signal has already reached ffmpeg, moq
#     and both `timeout`s (and the background subshell, whose trap is reset to the default, so it
#     ends too); ffmpeg and moq end on it (moq exits 0), nothing is forwarded from here;
#   - otherwise waits and exits with moq's status (above).
# Trade-off: after a stop, ffmpeg and moq may briefly outlive the adapter without the runner's
# SIGKILL backstop; they stay bounded by `timeout -k 2` (SIGKILL 2 s after the signal at the latest).
# Interactive caveat: a background child of a shell without job control starts with SIGINT and
# SIGQUIT ignored, so Ctrl-C ends only this adapter and the pipeline runs on to its deadline. The
# runner stops drivers with SIGTERM, which is not affected.
trap 'exit 0' TERM INT HUP
publish &
pipeline_pid=$!
pipeline_status=0
wait "$pipeline_pid" || pipeline_status=$?
exit "$pipeline_status"
