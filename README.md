# moq-contribution-interop-runner

Publisher-focused MoQT interoperability runner. The checked-in draft text in
`docs/` is the protocol authority. The requirement inventories cover drafts 18
and 21. The draft-18 scenarios subscribe to a configured track,
optionally subscribe again to verify rejection with `DUPLICATE_SUBSCRIPTION`,
or issue a standalone FETCH and check for exactly one response. Draft-21
profiles accept a publisher's PUBLISH for a
configured track and send an empty REQUEST_OK. These observed-mode scenarios
can run over native QUIC or WebTransport. The remaining publisher requirements
are cataloged but not executable yet.
Requests for unsupported scenarios return HTTP 422; they are never silently
scored as conformant.

Build on Linux with `cmake -S . -B build`, then
`cmake --build build -j4`. The production runner uses pinned picoquic and
picotls; it does not link quiche, BoringSSL, or Rust. To include the independent
quiche test peer and both native process-level publisher tests, configure with
`-DMOQ_INTEROP_BUILD_QUICHE_TEST_PEER=ON`, then run
`ctest --test-dir build --output-on-failure`. The option is off by default and
off in the production Docker image. The pinned picoquic revision accepts Retry
tokens for 120 seconds; the native listener rejects other configured lifetimes
rather than silently using a different value.
The optional suite also retains the legacy quiche integration tests in a
separate executable and compares draft-18/21 native evidence classes,
requirement outcomes, and scores across both backends (`native-parity`).
Runtime options are listed by `build/moq-interop-runner --help`.

To run the executable scenario, provide a PEM certificate and private key:

```sh
build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --publisher-bind 127.0.0.1 --publisher-port-start 4443 \
  --publisher-port-end 4452 --tls-cert cert.pem --tls-key key.pem
```

Without both TLS files, the HTTP inventory and stored-result endpoints remain
available, but POST `/api/v1/runs` returns HTTP 503 for executable scenarios.
The certificate must be trusted by the publisher being tested. The service
binds one UDP port per active run, up to the configured port range. It does not
forward to subscribers. A WebTransport client that sends `Origin` must use an
origin explicitly listed with repeatable `--publisher-origin https://host`
options. Non-browser clients may omit `Origin`; use
`--require-publisher-origin` to require it. The client must negotiate HTTP/3,
QUIC/H3 DATAGRAM, RESET_STREAM_AT, and the current WebTransport settings and
extended CONNECT profile; legacy WebTransport settings or protocol tokens are
rejected before MOQT bytes are scored.

The HTTP run configuration accepts an optional opaque-byte track fixture:

```json
{
  "draft": 18,
  "transport": "native-quic",
  "mode": "observed",
  "scenarios": ["subscribe-to-publisher-track"],
  "timeout_ms": 1000,
  "track": {"namespace_hex": ["6e"], "name_hex": "78"}
}
```

For the draft-18 duplicate-subscription check, set `scenarios` to
`["subscribe-again-to-established-publisher-track"]`. The runner waits for
`SUBSCRIBE_OK` to the first request, sends a second SUBSCRIBE for the identical
track, and scores `D18-5-1-MUST-004` from the response code. If the first
subscription is not established, that conditional requirement remains
`NOT_RUN` rather than becoming a publisher failure.

For the draft-18 FETCH response check, use
`["fetch-publisher-track-range"]`. The runner sends a standalone FETCH for
the configured track from Location `{0, 0}` to `{0, 1}` and scores
`D18-5-2-MUST-001` when exactly one `FETCH_OK` or `REQUEST_ERROR` is
observed. The track fixture names the request target; it does not assert that
the publisher has already published an Object. This profile does not score
the response code, range validity, or FETCH object delivery.

For draft-18 discovery response checks, use
`["subscribe-namespace-at-publisher"]` or
`["subscribe-tracks-at-publisher"]`. The runner sends the corresponding
request with the configured track namespace as its prefix and scores
`D18-6-1-MUST-001` or `D18-6-1-MUST-003` when exactly one `REQUEST_OK` or
`REQUEST_ERROR` is observed. The short post-response observation window
detects duplicate replies; these profiles do not yet score the separate
first-response ordering requirements or subsequent namespace/track updates.

For draft 21, set `draft` to `21` and select exactly one of
`d21-publisher-request-stream-placement`, `d21-setup-unknown-options`, or
`d21-setup-duplicate-unknown-options`, `d21-server-sends-authority`, or
`d21-server-sends-path` in `scenarios`; the native endpoint advertises ALPN
`moqt-21`, while WebTransport uses ALPN `h3` and selects `moqt-21` through
`WT-Available-Protocols` / `WT-Protocol`.
The two unknown-option SETUP profiles send the draft-21 reserved GREASE option
type `0x9D` once or twice, then require a valid PUBLISH and REQUEST_OK before
scoring receiver requirements `D21-9-1-MUST-287`, `-288`, and (for duplicates)
`-290` as passes. A close or timeout without that exchange stays `NOT_RUN`,
not a publisher failure. The draft-21 runner waits for both SETUP messages, including when
PUBLISH arrives before SETUP completes, and records the observed PUBLISH and
REQUEST_OK. This is a publisher-announcement test, not a subscriber-serving
relay or a full draft-21 conformance test. Unexercised catalog lines remain
`NOT_RUN`, so a valid run against the full catalog remains incomplete. An
invalid first message on a publisher-opened request stream is recorded and
fails `D21-6-3-MUST-NOT-141`; a different permitted but unsupported opener is
not reported as a publisher failure.

The AUTHORITY and PATH profiles deliberately send an otherwise well-formed
server SETUP with one role-forbidden option. They score
`D21-9-1-1-MUST-293` or `D21-9-1-2-MUST-300` only after the local SETUP and
the publisher client's application close are observed. The expected close codes
are `INVALID_AUTHORITY` (`0x19`) and `INVALID_PATH` (`0x8`); a different
application close code fails the selected requirement, while an unobserved or
transport-level close remains `NOT_RUN`. A completed publication after the
forbidden option, without the required close, also fails the requirement.

`namespace_hex` is an ordered array of 0–32 nonempty hex-encoded namespace
fields; `name_hex` is the possibly empty hex-encoded Track Name. The decoded
full name is limited to 4,096 bytes. Hex encoding preserves arbitrary bytes,
including NUL, without imposing a text canonicalization on publishers.

Create a run with `curl -sS -X POST http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' -d @run.json`, where `run.json` contains
the JSON above. HTTP 201 returns the run ID and `publisher_endpoint` with
`address`, `port`, and ALPN. For WebTransport, change `transport` to
`"webtransport"` in the run request; the endpoint also returns an HTTPS `url`,
`path`, and exact `moqt-18` or `moqt-21` `protocol`. Give that URL to a
WebTransport publisher. Native publishers connect to the returned UDP address
and port with the draft ALPN. Then retrieve `GET /api/v1/runs/{id}` or
`GET /api/v1/runs/{id}/events`. `GET /results` is the HTML summary, and
`GET /results/{id}` is the server-rendered requirement-by-requirement report.
`GET /results/{id}.json` exports every catalog row and its evidence;
`GET /results/{id}.tap` exports scenario-level TAP 14 diagnostics. The HTML
report supports `strength`, `outcome`, `section`, and `scenario` filters.
`GET /api/v1/requirements?draft=18` or `draft=21` lists catalog entries.
`GET /healthz` distinguishes the two inventoried drafts from the narrow
executable profiles and whether their listeners are configured.
`POST /api/v1/runs/{id}/stop` ends an active run and finalizes it as incomplete;
it does not count the interrupted interaction as a publisher failure.
Scoring distinguishes required MUST/MUST NOT, weighted recommendations, and
coverage; unexecuted requirements remain visible rather than counting as
passes. MUST/MUST NOT carry weight 10, SHOULD/SHOULD NOT weight 3, and MAY
weight 1. A failed required row yields a `fail` verdict; otherwise any
unexecuted applicable, testable row yields `incomplete`. The required score,
weighted score, and coverage have separate numerators and denominators in
the result exports. An incomplete or harness-failed run is not a publisher
failure.

Run `build/moq-interop-audit --draft 18` or `--draft 21` to inspect the
static completeness gate. Add `--format json` for sorted per-requirement findings,
draft digest and source revision, executable coverage counts, and residual
`not_testable`/`not_applicable` rows with reasons and draft citations. Exit
status 0 means the source and required evaluator/scenario/evidence registry
checks pass; status 1 means the draft is not yet executable-complete, which
is expected for the current narrow profiles. As of this checkpoint, only
5/175 draft-18 and 7/175 draft-21 applicable, testable MUST/MUST NOT rows
have registered executable bindings. A registered binding is a static gate,
not proof that a publisher passed it; run results still require live evidence.

To launch a publisher automatically, configure a trusted executable adapter
at runner startup and set the run request's `mode` to `"driven"`. The runner
starts its native listener first, passes the exact endpoint and track fixture
through the versioned JSON contract, and saves the adapter's stdout/stderr,
exit status, and SHA-256 log hashes under a per-run directory. An adapter
cannot set expected behavior or requirement scores. A publisher that exits
before connecting yields a run-level `ERROR` with logs retained; once
connected, scores derive from MoQT observations, not process exit status.
The adapter executable and arguments are set by the operator, never by an
unauthenticated HTTP caller.

For `moqxr`, the bundled adapter requires `bash` and `jq` and expects the
`media` namespace and `vide_1` track. This example uses the sibling build and
fixture; substitute actual publisher paths if they differ:

```sh
MOQXR_BIN="$(realpath ../moqxr/build/openmoq-publisher)" \
  build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --publisher-bind 127.0.0.1 --publisher-port-start 4443 \
  --publisher-port-end 4452 --tls-cert cert.pem --tls-key key.pem \
  --driver-executable "$PWD/adapters/moqxr/run.sh" \
  --driver-fixture "$(realpath ../moqxr/tests/fixtures/locmaf-publisher.mp4)" \
  --driver-log-root "$PWD/driver-logs"
```

Submit the same run JSON shown above with `"mode":"driven"`,
`"namespace_hex":["6d65646961"]`, and `"name_hex":"766964655f31"`.
Use draft 18 or 21 and `native-quic` or `webtransport`; the adapter maps
these to `moqxr` CLI options. For another publisher, implement the JSON
contract in `adapters/contract.schema.json` and configure its executable
with `--driver-executable`, optional repeated `--driver-arg`,
`--driver-fixture`, optional `--driver-ca`, and `--driver-log-root`. The
executable path must be absolute; arguments are never interpreted as a shell
command. Observed mode remains available without an adapter.

For Docker Compose, put `cert.pem` and `key.pem` in a directory readable by
container UID 10001, set `MOQ_INTEROP_TLS_DIR` to that directory, and set
`MOQ_INTEROP_PUBLISHER_HOST` to the externally reachable address. Use
`MOQ_INTEROP_UDP_BIND`, `MOQ_INTEROP_UDP_START`, and `MOQ_INTEROP_UDP_END` to
match the published UDP range. `scripts/container-build.sh build` creates the
image from a clean committed tree; `docker compose up` starts the HTTP and UDP
listeners. The HTTP endpoint is bound to localhost by default. Keep it on a
trusted network because the API has no authentication yet.
For container-driven runs, put a compatible publisher, media fixture, and any
custom adapter in `MOQ_INTEROP_PUBLISHER_DIR` (mounted read-only at
`/opt/publisher`). Set `MOQ_INTEROP_DRIVER_EXECUTABLE` to the in-container
adapter path, such as `/usr/local/lib/moq-interop/moqxr/run.sh`, and set
`MOQ_INTEROP_DRIVER_FIXTURE` to its in-container fixture path. For the bundled
`moqxr` adapter, set `MOQXR_BIN` to the publisher executable path under
`/opt/publisher`; the default is `/opt/publisher/openmoq-publisher`.
Container logs are retained in the `validator-results` volume under
`driver-logs/<run-id>`. If the adapter variable is unset, Compose runs
observed mode only. The published host and UDP range must be reachable from
the publisher process; `127.0.0.1` is valid only within the same network
namespace.
On graceful SIGTERM, active runs stop and finalize as incomplete. If the
process or container is killed before it can finalize, the next startup
marks each interrupted active run `ERROR` and records a `runner_recovery`
event; already-finalized results are left unchanged. Keep the SQLite database
and driver logs on persistent storage if results must survive container
replacement.

An opt-in black-box check against an external publisher is available:

```sh
bash tests/e2e/moqxr-matrix.sh \
  "$PWD/build/moq-interop-runner" \
  "/path/to/openmoq-publisher" \
  "/path/to/moqxr/tests/fixtures/locmaf-publisher.mp4"
```

This starts four independent **driven** HTTP runs: drafts 18 and 21 over
native QUIC and WebTransport. It checks runner health, process diagnostics,
retained contract input, and the full requirement export; it reports pass/fail
row counts without requiring the publisher to pass. Use the scripts below for
manual observed-mode diagnostics.

```sh
bash tests/e2e/draft18-native-moqxr.sh \
  "$PWD/build/moq-interop-runner" \
  "/path/to/openmoq-publisher" \
  "/path/to/moqxr/tests/fixtures/locmaf-publisher.mp4"
```

Pass `21` as a fourth argument to exercise the draft-21 PUBLISH-announcement
profile with `moqxr --preannounce-tracks`; omitting it selects draft 18. The
script checks the returned ALPN and prints publisher and runner logs on failure.
In a 2026-09-29 test with `moqxr` build `g478d6c0.dirty`, its picoquic client
did not negotiate QUIC DATAGRAM for either draft, so both attempts ended at the
transport gate before publisher behavior could be scored. This is an interop
observation, not a validator pass or a reason to bypass that draft requirement.

The optional test peer exercises the actual HTTP-created run and production
native listener for both drafts without depending on a particular publisher:
`ctest --test-dir build -R 'draft(18|21)-native' --output-on-failure`.
The WebTransport run API test creates both draft endpoints, performs an HTTP/3
extended CONNECT with a scripted publisher, sends MOQT SETUP, and checks stored
`peer_setup_received` evidence. For draft 18 it also answers the runner's
SUBSCRIBE and checks the scored single-response requirement:
`ctest --test-dir build -R webtransport-run-api --output-on-failure`.
The sibling `moq-rs/moq-pub` is a useful publisher reference but its checked-in
`moq-transport` currently lists draft versions only through 14 and ALPN
`moq-00`, so it is not a draft-18/21 acceptance fixture. The diagnostic
`tests/e2e/draft18-webtransport-smoke.sh` and
`tests/e2e/draft21-webtransport-smoke.sh` take the runner binary, moqxr
publisher binary, and MP4 fixture as arguments. They require a successful
publisher exit, observed SETUP, and at least one passing requirement. They
currently complete with moqxr in both drafts; the overall run verdict remains
`incomplete` because these smoke scenarios cover only a small part of each
draft's requirement inventory.

```sh
bash tests/e2e/draft18-webtransport-smoke.sh \
  "$PWD/build/moq-interop-runner" \
  "/path/to/openmoq-publisher" \
  "/path/to/moqxr/tests/fixtures/locmaf-publisher.mp4"
bash tests/e2e/draft21-webtransport-smoke.sh \
  "$PWD/build/moq-interop-runner" \
  "/path/to/openmoq-publisher" \
  "/path/to/moqxr/tests/fixtures/locmaf-publisher.mp4"
```

The runner acknowledges parameter-free PUBLISH_NAMESPACE requests needed for
these contribution flows and rejects the forbidden `.` namespace. It does not
silently authorize token-bearing announcements; those are not executable in
the current observed profiles.

Each script starts a loopback runner with temporary TLS material, asks the
publisher to connect, prints the run verdict and any scored requirements, and
removes its temporary files. It requires `openssl`, `curl`, and `jq`; it is not
part of the default CTest suite because `moqxr` and media input are external.
This fixture publishes the `media` namespace and `vide_1` track expected by
the script.
If the publisher omits QUIC DATAGRAM negotiation, the runner rejects the
session as required by draft 18 section 3.1, records the close, and leaves
publisher behavior unscored rather than marking a pass.
