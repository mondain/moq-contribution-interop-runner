# moq-lite adapter

`run.sh` lets the runner drive the `moq` CLI of moq-dev/moq (`rs/moq-cli`, binary `moq`;
written against 0.14.1 at commit `b8b0d235`) in driven mode, at moq-lite-06 only, on
native QUIC and WebTransport. It validates the runner's request, runs an ffmpeg test
pattern into `moq ... import fmp4` and supervises both processes; it never decides what a
scenario expects. `adapter.json` is a descriptive manifest; the runner does not read it.

## What it accepts

| Item | Value |
|---|---|
| Draft | the string `"moq-lite-06"` only. Numeric drafts (18, 21, 22, 106, ...) exit 64 with "draft N is not supported (supported drafts: moq-lite-06)"; any other string (`"moq-lite-05"`, `"moq-lite-07-wip"`, ...) with `draft "X" is not supported ...` |
| Transports | `native_quic` (endpoint `moql://...` or `moqt://...`), `webtransport` (endpoint `https://...`) |
| Endpoint | a non-empty printable-ASCII URI whose scheme matches the transport and which has an authority; passed to `--connect` verbatim |
| Namespace | exactly `["696e7465726f702e68616e67"]` (`interop.hang`), published as `--broadcast interop.hang` |
| Track name | exactly `302e6d3473` (`0.m4s`); not passed to the CLI (it names its media tracks `<id>.m4s` itself), it only names the track the runner subscribes to |
| Fixture | ignored: the source is ffmpeg's test pattern |
| TLS CA | not used by default (`--connect-tls-insecure`, whatever `tls_ca` says); with `MOQ_LITE_TLS_ROOT=1`, `--connect-tls-root <tls_ca>` (below) |
| Scenario id | any single-line string; all 19 executable scenarios use the same command line |
| `scenario_timeout_ms`, `process_timeout_ms` | plain integers (`2500.0` and `1e3` are refused); the scenario timeout from 1 to 3600000 |
| Publisher | `MOQ_CLI_BIN`, which must name an executable regular file that is the current `moq` CLI; there is no default |
| Tools | `bash`, `jq`, coreutils `timeout`, `ffmpeg` with libx264 (`MOQ_FFMPEG_BIN` overrides the one on `PATH`) |

The adapter exits 64 with a `moq-lite adapter: ...` message on stderr that names the
problem, before anything starts, when the contract version is not 1, the request file is
unreadable or not one JSON object, a field above is missing or wrong (each field has its
own message), `log_dir` is not a writable directory, `MOQ_LITE_TLS_ROOT=1` is set and
`tls_ca` is empty or not a readable file,
`MOQ_CLI_BIN`, `MOQ_FFMPEG_BIN`, `ffmpeg` or `timeout` is unusable, or `MOQ_CLI_BIN` is
not the current CLI.

### Environment

| Variable | Meaning |
|---|---|
| `MOQ_CLI_BIN` | the `moq` binary (required) |
| `MOQ_FFMPEG_BIN` | the ffmpeg binary (optional; default: `ffmpeg` on `PATH`) |
| `MOQ_LITE_TLS_ROOT` | `1`: `--connect-tls-root <tls_ca>` instead of the default `--connect-tls-insecure` (exit 64 if `tls_ca` is empty or unreadable); `0` or unset: the default; anything else exits 64 |
| `MOQ_LITE_TLS_INSECURE` | accepted for compatibility and a no-op: `0`, `1` or unset (insecure is the default); `1` together with `MOQ_LITE_TLS_ROOT=1` exits 64; anything else exits 64 |

The CLI reads every flag from a `MOQ_*` environment variable as well (`MOQ_CONNECT`,
`MOQ_HOP`, `MOQ_CONNECT_TLS_ROOT`, ...). The adapter unsets every `MOQ_*` variable
(including the three above and `MOQ_INTEROP_*`, after reading them) before it runs
anything, so only its command line configures the publisher. It sets `NO_COLOR=1` (a
non-empty value: an empty `NO_COLOR` does not turn the colours off), so `publisher.log` has
no ANSI colour codes. `RUST_LOG` is passed through on purpose, for debugging: if set, it
overrides `--log-level debug` and so changes how much `publisher.log` holds (a filter
such as `RUST_LOG=trace` makes it much larger, `RUST_LOG=warn` drops the debug lines a
triage relies on).

## The binary: the current moq CLI, built offline

The prebuilt `target/debug/moq-cli` in a moq-dev checkout from March 2026 is an older,
different tool (`moq-cli serve|publish`, no `--version`, no `--connect-version`). The
adapter refuses it: `"$MOQ_CLI_BIN" --version` must print a line starting `moq ` and
`"$MOQ_CLI_BIN" --help` must mention `--connect-version`, else it exits 64 with "is not
the current moq CLI". The version line is written to the adapter's stderr (the runner's
`stderr.bin`) on every accepted run.

Build the CLI from a scratch copy, never inside the peer checkout (which pins a rustup
toolchain and must stay untouched):

```sh
# a copy of the moq-dev/moq checkout without target/, .git/ and rust-toolchain.toml
rsync -a --exclude target --exclude .git --exclude rust-toolchain.toml /path/to/moq/ "$SCRATCH/moq/"
cd "$SCRATCH/moq"
CARGO_TARGET_DIR="$SCRATCH/target" cargo +1.98.1 build -p moq-cli --release --offline --locked
export MOQ_CLI_BIN="$SCRATCH/target/release/moq"
"$MOQ_CLI_BIN" --version    # moq 0.14.1
```

`--offline` needs the dependencies in the local cargo cache already (a previous build of
the same lock file); the build took about 3 minutes.

## The command line

For a request with endpoint `E` and scenario timeout `T` ms (`S` = `T` rounded up to whole
seconds, plus 3):

```text
timeout --foreground --preserve-status -k 2 -s TERM <S+2> ffmpeg -hide_banner -v error -re \
    -f lavfi -i testsrc2=size=640x360:rate=30 -t <S> -c:v libx264 -preset veryfast \
    -tune zerolatency -profile:v baseline -pix_fmt yuv420p -b:v 200k -maxrate 200k \
    -bufsize 400k -g 30 -keyint_min 30 -sc_threshold 0 -f mp4 \
    -movflags cmaf+separate_moof+delay_moov+skip_trailer -frag_duration 1000 - \
  | timeout --foreground --preserve-status -k 2 -s TERM <S+2> "$MOQ_CLI_BIN" --log-level debug \
    --connect-version moq-lite-06 --connect-once --connect-timeout 10s \
    --connect-tls-insecure --connect "E" --broadcast interop.hang import fmp4
```

with `--connect-tls-root <tls_ca>` in place of `--connect-tls-insecure` only when
`MOQ_LITE_TLS_ROOT=1` is set. `-frag_duration` is in microseconds, so `1000` (1 ms) makes
ffmpeg write about one fragment per frame; this is the recipe verified with the CLI, and the
live smoke (L1e Task 4) may change it to `1000000` (1 s, one fragment per GOP).
ffmpeg's standard input is `/dev/null`, its standard error goes to `<log_dir>/ffmpeg.log`;
moq's standard output and standard error go to `<log_dir>/publisher.log`. Every argument
is a separate array element: nothing from the request is evaluated or word-split, and the
broadcast name comes only from the pinned constant.

| Option | Why |
|---|---|
| `--connect-version moq-lite-06` | offers moq-lite-06 only: ALPN `moq-lite-06` on raw QUIC, WebTransport protocol `moq-lite-06` on `https` |
| `--connect-once` | one dial, no redial: one session per context, deterministic |
| `--connect-timeout 10s` | a dial that never completes ends the CLI (exit 1) |
| `--log-level debug` | enough detail in `publisher.log` to triage a run |
| `--broadcast interop.hang` | the fixture's broadcast (the runner's broadcast path is the namespace fields joined with `/`) |
| `import fmp4` | fragmented MP4 from standard input; a new group at each video sync sample |

**The source.** A 640x360, 30 fps test pattern, H.264 baseline at about 200 kbit/s (200 to
380 measured), a 1 s GOP without scene-cut keyframes (so the CLI starts a group every
second), video only (the CLI rejects per-frame audio and video interleaving), fragmented MP4
in real time (`-re`). The CLI has no duration flag: ffmpeg's `-t S` ends the source, which
gives moq its standard input EOF and a graceful exit 0. The runner waits up to the timeout
for the connection and fits each probe inside the timeout, so the source outlasts a context
whose publisher connects within 3 s; normally the runner stops the pipeline first. Use a run
`timeout_ms` that fits the longest probe (`l06-errors-unknown-reset-code`, about 9 s): a
source that ends inside a probe makes the CLI close the session with NO_ERROR.

**The endpoint.** The runner builds `moql://HOST:PORT/moq?token=l1d` (native QUIC) and
`https://HOST:PORT/moq?token=l1d` (WebTransport): the path `/moq` and query `token=l1d` are
fixed constants in `src/app/lite_run.cpp`, judged by rows 120 and 124 (native QUIC, where
the CLI puts them in its SETUP Path) and 125 (WebTransport, where they are the CONNECT
`:path` and the SETUP must carry no Path). The WebTransport listener accepts exactly that
`:path`, so the adapter passes the endpoint unchanged: it only checks that the scheme fits
the transport (`moqt://` is accepted for native QUIC too) and that it is printable ASCII
with an authority. An `https://` dial also races a WebSocket fallback over TCP on the same
port, which is harmless when nothing listens there.

**TLS.** The default is `--connect-tls-insecure`, whatever the request's `tls_ca` says.
The runner always fills `tls_ca` (its `--driver-ca`, else its `--tls-cert`), and its usual
certificate is a self-signed one made with `openssl req -x509`, which carries CA:TRUE. The
CLI's verifier (rustls-webpki) refuses a CA certificate presented as the server's own:
`--connect-tls-root` with that file fails every handshake with `invalid peer certificate:
CaUsedAsEndEntity` (checked with moq 0.14.1). Trusting `tls_ca` is therefore an explicit
opt-in: `MOQ_LITE_TLS_ROOT=1` passes `--connect-tls-root <tls_ca>` (which replaces the
system roots), for a listener whose server certificate is issued by the CA in that file;
the adapter then exits 64 when `tls_ca` is empty or unreadable. `MOQ_LITE_TLS_INSECURE`,
the opt-out of an earlier version, is still accepted (`0` or `1`) and changes nothing.

## Exit codes

| Status | Meaning |
|---|---|
| 64 | the adapter refused the request or its environment (nothing was started) |
| 0 | moq ended normally: source EOF, or SIGTERM (also from the `timeout` deadline) |
| 1 | moq's dial failed (for example `--connect-timeout`) |
| 2 | moq rejected its command line |
| other | moq's own status as `timeout --preserve-status` reports it, also when the session closed (137 after `-k 2`) |

The adapter's status is moq's, not ffmpeg's: ffmpeg exits nonzero whenever moq ended
first (the runner closed the session and ffmpeg's next write fails), so its status cannot
tell a failed run. A nonzero ffmpeg status is reported on the adapter's stderr
(`moq-lite adapter: ffmpeg exited with status N`) and its messages are in `ffmpeg.log`.

## Supervision

The runner stops a driver with SIGTERM to its process group and SIGKILLs the group 100 ms
later, recording that as a driver failure. The adapter does not `exec` the pipeline: it
runs it in the background in the same process group (`timeout --foreground` keeps both
`timeout`s there too), and on SIGTERM, SIGINT or SIGHUP exits 0 at once. The group's
SIGTERM has already reached ffmpeg, moq and both `timeout`s; moq exits 0 on it, ffmpeg
stops, and `timeout -k 2` SIGKILLs either of them 2 s after the signal if it has not
ended, so nothing outlives the stop by more than 2 s and no process is orphaned. Without
a signal the adapter waits for the pipeline and exits with moq's status. Run by hand,
Ctrl-C ends only the adapter (a background child of a non-interactive shell ignores
SIGINT) and the pipeline runs on to its deadline.

## The fixture

The adapter publishes exactly one broadcast, `interop.hang`, and accepts only the track
fixture `namespace_hex: ["696e7465726f702e68616e67"]`, `track_name_hex: "302e6d3473"`
(`0.m4s`, the name the CLI gives the first media track, from the moov trak order). These
are the expected shape from the survey of the CLI and are PROVISIONAL: the live smoke of
L1e Task 4 pins them from the CLI's real announcements. They are defined once, at the top
of `run.sh` (`fixture_namespace_hex`, `fixture_track_name_hex`, `fixture_broadcast`), and in
`adapter.json`; change both and regenerate the golden. Post the matching fixture with the
run:

```json
{"draft": "moq-lite-06", "transport": "native-quic", "mode": "driven", "timeout_ms": 12000,
 "scenarios": ["l06-subscribe-latest"],
 "track": {"namespace_hex": ["696e7465726f702e68616e67"], "name_hex": "302e6d3473"}}
```

## Per-scenario options: none

Unlike `adapters/moqxr` (`--forward`, `--paced` per scenario) and `adapters/imquic` (`-X`,
`-D datagram`), this adapter has no per-scenario table: the CLI has no options that change
what it emits per scenario. All 19 executable scenarios (`tests/golden/executable-ids-d106.txt`)
get the same command line on a transport; they differ only in what the runner does
(which SETUP it sends, what it subscribes to, which streams it resets or closes).

## Limits

- No duration flag: the source length is derived from the scenario timeout (above).
- The CLI announces its broadcast at startup and the adapter has no option that makes it
  end and restart the broadcast within a session, so row 152 (`l06-announce-lifecycle`)
  is expected `not_run`; that and the other `not_run` rows are decided by the runner, not
  here.
- Only the pinned fixture: one broadcast, the first video track.
- `--connect-once`: a session the runner closes is not redialed; each context starts a
  new process.

## Tests and goldens

| ctest | What it pins |
|---|---|
| `moq-lite-adapter-contract` | validation and every refusal (exit 64 with a message naming the problem), the endpoint passed verbatim (also with shell metacharacters), TLS handling (insecure by default, also with a `tls_ca`; the `MOQ_LITE_TLS_ROOT=1` opt-in and its refusals), `MOQ_FFMPEG_BIN`, the cleared `MOQ_*` environment and `NO_COLOR=1`, the separate logs, exit statuses, the `timeout` deadline, and the supervisor's shutdown (adapter exit and the end of ffmpeg and moq within the 100 ms grace, no orphan; the `-k 2` bound for processes that ignore SIGTERM) |
| `moq-lite-adapter-cmdlines` | the full ffmpeg and moq command lines for every executable moq-lite-06 id on both transports, in `tests/golden/moq-lite-cmdlines.txt`; that the id list equals `kLiteExecutableScenarios` and that every id has the same command line |

Both use stub binaries and need no network (the contract test also needs python3). Run them
with `ctest --test-dir build -R moq-lite-adapter` or directly:

```sh
bash tests/e2e/moq-lite-adapter-contract.sh
bash tests/e2e/moq-lite-adapter-cmdlines.sh            # compare with the golden
bash tests/e2e/moq-lite-adapter-cmdlines.sh --update   # or MOQ_UPDATE_GOLDEN=1: rewrite it, then review the diff
```
