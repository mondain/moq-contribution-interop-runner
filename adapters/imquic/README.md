# imquic adapter

`run.sh` lets the runner drive imquic's example publisher (`examples/moq-pub.c` from the
imquic C library, built as `examples/imquic-moq-pub`) in driven mode, at MoQ draft 22
only, on native QUIC and WebTransport. It translates the runner's request into moq-pub's
command line and supervises the process; it never decides what a scenario expects.
`adapter.json` is a descriptive manifest; the runner does not read it.

## What it accepts

| Item | Value |
|---|---|
| Drafts | 22 only (18, 21 and others exit 64: "draft N is not supported (supported drafts: 22)") |
| Transports | `native_quic` (endpoint `moqt://HOST:PORT[/path]`), `webtransport` (endpoint `https://HOST:PORT[/path][?query]`) |
| Namespace | exactly `["6d65646961"]` (`media`), passed as `-n media` |
| Track name | exactly `766964655f31` (`vide_1`), passed as `-N vide_1` |
| Fixture | ignored: moq-pub publishes a clock (one Object per second, one Group per minute) |
| TLS CA | ignored: moq-pub never verifies the certificate |
| Publisher | `IMQUIC_PUB_BIN`, which must name an executable regular file; there is no default |
| Tools | `bash`, `jq`, coreutils `timeout` |

The adapter exits 64 with an `imquic adapter: ...` message on stderr, before moq-pub
starts, when the contract version is not 1, `IMQUIC_PUB_BIN` is unusable, the request is
malformed or names another draft, transport, namespace or track, `scenario_timeout_ms`
is not written as a plain integer (`2500.0` and `1e3` are refused), `log_dir` is not a
writable directory, or the endpoint cannot be expressed: wrong scheme for the transport,
a query on native QUIC, an empty host, a missing or out-of-range port, user information,
a fragment, an IPv6 zone, whitespace or control characters.

## Build imquic and run it

Build imquic with its MoQ examples as imquic's own `README.md` describes (picoquic
installed in the repository root first), then point `IMQUIC_PUB_BIN` at the binary:

```sh
# in the imquic checkout
sh autogen.sh
./configure --enable-moq-examples
make
export IMQUIC_PUB_BIN=/path/to/imquic/examples/imquic-moq-pub   # or the installed imquic-moq-pub
```

The driven smoke script runs `d22-publisher-request-stream-placement` once and prints the
verdict, the publisher exit and the pass and fail counts. It needs an MP4 path for the
runner's `--driver-fixture`, which the adapter does not read:

```sh
bash tests/e2e/driven-imquic.sh 22 native_quic build/moq-interop-runner "$IMQUIC_PUB_BIN" fixture.mp4
bash tests/e2e/driven-imquic.sh --dry-run 22 webtransport build/moq-interop-runner "$IMQUIC_PUB_BIN" fixture.mp4
```

`--dry-run` prints the runner invocation and the run request without starting anything.
The script starts the runner with these driver arguments (ports default to 19221/19222;
`MOQ_INTEROP_TEST_HTTP_PORT` and `MOQ_INTEROP_TEST_UDP_PORT` override them):

```sh
IMQUIC_PUB_BIN=... build/moq-interop-runner --bind 127.0.0.1 --port 19221 \
  --database "$T/runs.sqlite3" --docs docs --requirements requirements \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 19222 --publisher-port-end 19222 \
  --tls-cert "$T/cert.pem" --tls-key "$T/key.pem" \
  --driver-executable "$PWD/adapters/imquic/run.sh" \
  --driver-fixture fixture.mp4 --driver-log-root "$T/logs"
```

and posts `{"draft": 22, "transport": "native-quic", "mode": "driven", "scenarios":
[...], "timeout_ms": 12000, "track": {"namespace_hex": ["6d65646961"], "name_hex":
"766964655f31"}}` to `/api/v1/runs`. moq-pub registers no FETCH handler, so for a sweep
add `--publisher-no-fetch` to the runner (as the sweep in `docs/interop-notes.md` does).

`tests/e2e/imquic-matrix.sh [--dry-run] [--pair 22 TRANSPORT] RUNNER_BIN IMQUIC_PUB_BIN
MP4_FIXTURE` runs the adapter contract test, then the driven script on both transports
(native QUIC on 19221/19222, WebTransport on 19223/19224). It is a smoke test of the
harness, not a conformance sweep.

## How the request maps to moq-pub options

| Request field | moq-pub option |
|---|---|
| `draft` | `-M 22` |
| `transport` | `-q` (raw QUIC) or `-w` (WebTransport) |
| `endpoint` | `-r HOST -R PORT` (IPv6 brackets removed, port normalized); WebTransport adds `-H PATH`, the path and query unchanged (`/` if empty); the native QUIC path is dropped |
| `namespace_hex`, `track_name_hex` | `-n media -N vide_1` (decoded; printable ASCII only) |
| `scenario_id` | `-X` or not, and `-D datagram` for `d22-object-datagram-flags` (below) |
| `scenario_timeout_ms` | `timeout ... <seconds rounded up + 3>` around moq-pub (it has no timeout option) |
| `log_dir` | moq-pub's output (logging `-d 4`) goes to `<log_dir>/publisher.log` |
| `fixture`, `tls_ca` | not used |

Per-scenario modes: publish-first (`-X`: PUBLISH right after SETUP, every SUBSCRIBE
refused with DUPLICATE_SUBSCRIPTION) or announce-and-wait (no `-X`: PUBLISH_NAMESPACE,
then one SUBSCRIBE is served). A `d22-X` id is normalized to `d21-X` and the mode is
derived from the moqxr adapter: moqxr `--forward 1` gives `-X`, `--forward 0` (paced or
not) gives none, and an id no list names falls back to `-X`. The exceptions are the
d22-keyed lists in `run.sh`: `d22_moqxr_paced_overrides` (a copy of moqxr's draft 22
paced overrides, no `-X`), `d22_announce_overrides` (moqxr `--forward 1`, here no `-X`),
`d22_publish_overrides` (moqxr `--forward 0`, here `-X`), and the explicit entries for
the own draft 22 scenarios and probes (`own22_publish_first`, `own22_announce`). The
reasons are in `run.sh`; the table is in
[the harness guide](../../docs/publisher-harness-guide.md#the-imquic-adapter-draft-22).

## Lifecycle

The runner stops a driver with SIGTERM to its process group and SIGKILLs the group
100 ms later, recording that as a driver failure; moq-pub needs about 40 to 160 ms after
SIGTERM to send PUBLISH_DONE and PUBLISH_NAMESPACE_DONE and close. So the adapter does
not `exec` moq-pub. It starts `timeout --foreground --preserve-status -k 2 -s TERM
<timeout+3>` and moq-pub in the background, in the same process group, and on SIGTERM,
SIGINT or SIGHUP exits 0 at once, while moq-pub (already signalled by the group SIGTERM)
finishes on its own. Without a signal it waits and exits with moq-pub's own status. The
trade-off: after a stop moq-pub briefly outlives the adapter without the runner's
SIGKILL backstop; `timeout -k 2` still kills it at most 2 s after the signal. moq-pub
can receive two SIGTERMs (the group's and the one `timeout` relays); when its stop
counter was already bumped it then exits(1) without cleanup, in about 5 percent of runs,
which changed no verdict. Run by hand, Ctrl-C ends only the adapter and moq-pub runs on
to its `timeout` deadline.

## Tests and goldens

| ctest | What it pins |
|---|---|
| `imquic-adapter-contract` | validation and refusals (exit 64), endpoint translation, `-X` and `-D datagram` choices, the `timeout` wrapper, and the supervisor's shutdown (adapter exit within the 100 ms grace, publisher cleanup, the `-k 2` bound) |
| `imquic-adapter-cmdlines-d22` | the full `timeout` and moq-pub command line for every executable draft 22 id and the probes on both transports, in `tests/golden/imquic-cmdlines-d22.txt`; twin parity (`d22-X` equals `d21-X` except the override lists), the moqxr derivation against `tests/golden/moqxr-cmdlines-d22.txt`, and that the own lists name exactly the own scenarios and probes |
| `imquic-matrix-plan` | the matrix and driven scripts' `--dry-run` plan (`tests/golden/imquic-matrix-plan.txt`) and that a stub run does what the plan says |

All of them use stubs and need no network. Goldens are never rewritten by default; to
regenerate after a deliberate change run `bash tests/e2e/imquic-adapter-cmdlines.sh
--update` (or set `MOQ_UPDATE_GOLDEN=1`) and review the diff. To change a scenario's
mode: add its `d22-` id to the right list in `run.sh` and to the copy of that list in
`tests/e2e/imquic-adapter-cmdlines.sh`, regenerate the golden, and check that the
twin-parity and derivation checks still pass.

## Known limits and results

- moq-pub is a demo publisher: one clock track, one subscriber at a time, no FETCH,
  SUBSCRIBE_NAMESPACE or SUBSCRIBE_TRACKS handler of its own.
- Names must be printable ASCII without spaces; the adapter accepts only `media` /
  `vide_1`.
- TLS verification is always off, so `tls_ca` is ignored.
- Its raw QUIC client sends no PATH or AUTHORITY SETUP option, so a `moqt://` path is
  dropped and a `moqt://` query is refused (exit 64).
- It has no timeout option, so the adapter wraps it in `timeout`.
- The adapter passes no emission option other than `-D datagram` (no `-P`, `-f`, `-F`
  or `-x`), so rows that need padding, gaps or Object properties stay unscored.

The draft 22 sweep at imquic `6836173` (2026-10-06) scored 45 pass, 20 fail and 81
not_run rows on native QUIC; on WebTransport every row stayed not_run because moq-pub
never sends its extended CONNECT (imquic I-01). This is one peer at one revision on one
date, not a conformance claim. The method, triage and full results are in
[interop-notes.md](../../docs/interop-notes.md#draft-22-sweep-against-imquic-6836173), and
the findings in [imquic-punch-list.md](../../docs/imquic-punch-list.md).
