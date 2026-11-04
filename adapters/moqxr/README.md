# moqxr adapter

`run.sh` lets the runner drive `openmoq-publisher`, the C++ publisher from the sibling
`moqxr` checkout (picoquic native backend, with an opt-in moq5 backend), as a
contribution publisher in driven mode. The adapter supports MoQ drafts 18, 21, 22 and 23 on
native QUIC and WebTransport. It only translates the runner's request into moqxr's
command line; it never decides what a scenario expects. `adapter.json` is a descriptive
manifest; the runner does not read it.

## What it accepts

| Item | Value |
|---|---|
| Drafts | 18, 21, 22, 23 (anything else exits 64: "supported drafts: 18, 21, 22, 23") |
| Transports | `native_quic` (endpoint must be `moqt://...`), `webtransport` (endpoint must be `https://...`) |
| Namespace | exactly `["6d65646961"]` (`media`) |
| Track name | exactly `766964655f31` (`vide_1`) |
| Fixture | required: a readable MP4 (`--driver-fixture`), passed to moqxr as `--input` |
| TLS CA | required: a readable trust file (`tls_ca`) |
| Publisher | `MOQXR_BIN`, which must name an executable; there is no default |
| Tools | `bash`, `jq` |

The adapter exits 64 with a `moqxr adapter: ...` message on stderr, before moqxr starts,
when the contract version is not 1, the request file is missing, `MOQXR_BIN` is unset or
not executable, the request is malformed or names another draft, transport, namespace or
track, the fixture or CA is unreadable, or the endpoint scheme does not match the
transport. The runner then records the run as `error` with the logs retained.

## Build moqxr and run it

Build `openmoq-publisher` as moqxr's own `README.md` describes (default backend):

```sh
# in the moqxr checkout
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
export MOQXR_BIN=/path/to/moqxr/build/openmoq-publisher
```

The fixture used by the sweeps is moqxr's `tests/fixtures/locmaf-publisher.mp4`.
The driven smoke script runs one scenario (`subscribe-to-publisher-track` at draft 18,
`d21-publisher-request-stream-placement` at 21, `d22-publisher-request-stream-placement`
at 22) and prints the verdict, the publisher exit and the pass and fail counts:

```sh
bash tests/e2e/driven-moqxr.sh 22 native_quic build/moq-interop-runner "$MOQXR_BIN" \
    /path/to/moqxr/tests/fixtures/locmaf-publisher.mp4
bash tests/e2e/driven-moqxr.sh --dry-run 22 webtransport build/moq-interop-runner "$MOQXR_BIN" fixture.mp4
```

`--dry-run` prints the runner invocation and the run request without starting anything.
The script starts the runner with these driver arguments (ports default to 19301/19302;
`MOQ_INTEROP_TEST_HTTP_PORT` and `MOQ_INTEROP_TEST_UDP_PORT` override them):

```sh
MOQXR_BIN=... build/moq-interop-runner --bind 127.0.0.1 --port 19301 \
  --database "$T/runs.sqlite3" --docs docs --requirements requirements \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 19302 --publisher-port-end 19302 \
  --tls-cert "$T/cert.pem" --tls-key "$T/key.pem" \
  --driver-executable "$PWD/adapters/moqxr/run.sh" \
  --driver-fixture /path/to/locmaf-publisher.mp4 --driver-log-root "$T/logs"
```

and posts `{"draft": 22, "transport": "native-quic", "mode": "driven", "scenarios":
[...], "timeout_ms": 12000, "track": {"namespace_hex": ["6d65646961"], "name_hex":
"766964655f31"}}` to `/api/v1/runs`. moqxr has no FETCH, so for a sweep add
`--publisher-no-fetch` to the runner (as the sweeps in `docs/interop-notes.md` do).

`tests/e2e/moqxr-matrix.sh [--dry-run] [--pair DRAFT TRANSPORT] RUNNER_BIN MOQXR_BIN
MP4_FIXTURE` runs the adapter contract test, then the driven script for drafts 18, 21
22 and 23 on both transports, each pair on fixed ports (19201 to 19216). It is a smoke test
of the harness, not a conformance sweep.

## How the request maps to moqxr options

| Request field | moqxr option |
|---|---|
| `draft` | `--draft 18`, `--draft 21`, `--draft 22` or `--draft 23` |
| `transport` | `--transport raw` (native QUIC) or `--transport webtransport` |
| `endpoint` | `--endpoint`, unchanged |
| `fixture` | `--input` |
| `tls_ca` | `--ca` |
| namespace (fixed) | `--namespace media` |
| `scenario_timeout_ms` | `--timeout`, rounded up to whole seconds: T; older paced modes use T+3, draft 23 uses 2T+3 |
| `scenario_id` | selects `--forward`, `--paced` and `--publish-catalog` (below) |
| `log_dir` | not passed; the runner keeps the adapter's (and moqxr's) output in `stdout.bin` and `stderr.bin` there |

Per-scenario modes: draft 18 always runs `--forward 0`. At drafts 21 and 22 the default
is `--forward 1` (moqxr sends its own PUBLISH); a short list of scenarios in which the
runner subscribes gets `--forward 0`, and the raw-probe list gets `--forward 0 --paced`
with the timeout plus 3 seconds, so the runner, not moqxr, ends the context. Three
unprefixed (draft 18) publish ids get `--publish-catalog`. A draft 22 id `d22-X` is normalized to its draft 21
implementation id `d21-X` and gets exactly that scenario's options with `--draft 22`,
except the 14 ids of `d22_paced_overrides` (paced at draft 22 only; draft 21 command
lines are frozen) and the draft 22 own scenarios and two unscored probes, which have an
explicit entry (all paced except `d22-publisher-location-filter-parameter`, which keeps
`--forward 1`). The lists and their reasons are in `run.sh`; the tables are in
[the harness guide](../../docs/publisher-harness-guide.md#32-the-request-file).

## Lifecycle

The adapter `exec`s moqxr, so moqxr receives the runner's signals directly. When a
context ends the runner sends SIGTERM to the adapter's process group and SIGKILL 100 ms
later; a SIGKILL is recorded as a process failure. moqxr's own `--timeout` is the
scenario timeout rounded up (plus 3 seconds for older paced scenarios). Draft 23
uses twice the rounded timeout plus 3 seconds, covering separate connection and
delivery deadlines; the runner enforces `process_timeout_ms` and stops the child.

## Tests and goldens

| ctest | What it pins |
|---|---|
| `moqxr-adapter-contract` | validation and refusals (exit 64), transport and endpoint mapping, `--forward` defaults per draft, draft 22 normalization, paced overrides, `--publish-catalog` |
| `moqxr-adapter-cmdlines-d18`, `-d21`, `-d22`, `-d23` | the full argument list for every executable id on both transports, in `tests/golden/moqxr-cmdlines-d18.txt`, `-d21.txt`, `-d22.txt`, `-d23.txt`; at draft 22 also that every shared id equals its `d21-` twin except the paced overrides |
| `moqxr-matrix-plan` | the matrix and driven scripts' `--dry-run` plan (`tests/golden/moqxr-matrix-plan.txt`) and that a stub run does what the plan says |

All of them use stubs and need no network. Goldens are never rewritten by default; to
regenerate after a deliberate change run
`bash tests/e2e/moqxr-adapter-cmdlines.sh 22 --update` (or set `MOQ_UPDATE_GOLDEN=1`)
and review the diff. To give a scenario id different options: add it to the matching
list in `run.sh` (for a draft 22-only change, `d22_paced_overrides`, and the copy of
that list in `tests/e2e/moqxr-adapter-cmdlines.sh`), regenerate the golden for the
affected draft, and run `ctest --test-dir build -R "moqxr|imquic"`. The imquic
command-line test derives its modes from the moqxr draft 22 golden, and the imquic
adapter keeps a copy of `d22_paced_overrides` (`d22_moqxr_paced_overrides`) that its
test compares with this one, so a change here usually needs the imquic copy and the
imquic golden updated too.

## Known limits and results

- moqxr supports drafts 21, 22 and 23 on its native backend only; the moq5 backend stays on
  drafts 16 and 18 (moqxr `README.md`). The adapter does not select a backend.
- The adapter refuses what it cannot express (other names, drafts or schemes) instead
  of guessing, and passes no option that describes an expectation.
- Some draft 21 rows fail only because their probes run `--forward 1` at draft 21; see
  the punch list's status notes before treating them as moqxr defects.

The draft 22 sweeps at moqxr `4b615f4` (re-swept after the F1 runner fixes) and at
`1883b9f` both scored 69 pass, 6 fail and 72 not_run rows on native QUIC (2026-10-06);
each is one peer at one revision on one date, not a conformance claim. The method, triage and full results are in
[interop-notes.md](../../docs/interop-notes.md#draft-22-sweep-against-moqxr-4b615f4), and
the open findings in [moqxr-punch-list.md](../../docs/moqxr-punch-list.md).

## Draft 23 testing and debugging

Use a native-backend moqxr build whose `--help` lists `--draft ...|23`; rebuild
local `main` if the binary is older than its source. The adapter accepts exactly
`d23-publisher-smoke`, `d23-direct-subscribe-smoke`, `d23-publish-smoke` and the `d23-probe-*`
ids for draft 23. Most run `--forward 0 --paced` so the runner can subscribe and observe subgroup objects.
Four run `--forward 1` instead, because they read the publisher's own PUBLISH: `d23-publish-smoke` and the probes
`d23-probe-unknown-error-detail`, `d23-probe-unknown-request-error-code` and `d23-probe-second-publish-response`.
At `--forward 1` moqxr offers `catalog`, `soun_2` and `vide_1` with PUBLISH, one after another, and blocks on each
answer. The runner accepts every PUBLISH in the fixture namespace and does not SUBSCRIBE; a refused PUBLISH ends
moqxr's session. The unknown-detail and unknown-code probes refuse the first track (`catalog`) with their own bytes
as the probe, so moqxr's close with code 0 that follows is not a verdict.
The fixture remains namespace `media`, track `vide_1` (`766964655f31`); use that
track in API requests rather than the generic publisher guide's `vide` example.

```sh
export MOQXR_BIN=/path/to/moqxr/build/openmoq-publisher
MOQ_INTEROP_TEST_KEEP=1 bash tests/e2e/driven-moqxr.sh 23 native_quic \
    build/moq-interop-runner "$MOQXR_BIN" /path/to/locmaf-publisher.mp4
MOQ_INTEROP_TEST_KEEP=1 bash tests/e2e/driven-moqxr.sh 23 webtransport \
    build/moq-interop-runner "$MOQXR_BIN" /path/to/locmaf-publisher.mp4
```

Set `MOQ_INTEROP_TEST_SCENARIO=d23-direct-subscribe-smoke` to test direct
subscription after SETUP. `--dry-run` prints the exact runner invocation and
request. `tests/e2e/moqxr-matrix.sh --pair 23 native_quic ...` and `--pair 23
webtransport ...` select the draft-23 matrix pairs on ports 19213/19214 and
19215/19216. The default matrix includes both pairs.

With `MOQ_INTEROP_TEST_KEEP=1`, the helper prints `kept /tmp/moq-interop-driven.*`
and preserves the database, runner log and
`logs/<run-id>/<scenario-id>/{request.json,stdout.bin,stderr.bin}`. Inspect these
when diagnosing negotiation or delivery; the script exits nonzero if the smoke
evidence is missing or its status is not PASS. A successful draft-23 run prints
`smoke=PASS` and `verdict=incomplete`: unreviewed requirements remain NOT_RUN.

Validated on 2026-11-03 with moqxr main `fae20de` (0.4.12): both scenarios passed
on native QUIC and WebTransport, four runs total. Each stored SETUP, namespace
announcement, subscription establishment, two distinct objects and 1357 payload
bytes. This is delivery smoke evidence; the runner treats payloads as opaque.
