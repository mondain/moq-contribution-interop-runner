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
`cmake --build build -j4` and `ctest --test-dir build --output-on-failure`.
The runner and local native test peers use pinned picoquic and picotls.
Native draft-18/21 process tests and evaluator integration tests are included
in the default test build. The pinned picoquic revision accepts Retry tokens
for 120 seconds; the native listener rejects other configured lifetimes.
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

Receiver-error probes are listed by `/healthz` alongside the publication
scenarios. In observed mode these probes accept a run request without `track`;
for example, use `receive-forward-outside-zero-one` for draft 18 or
`d21-forward-value-two` for draft 21. Each context sends one isolated malformed
stimulus and scores the publisher's application close. Exact close codes are
checked where the draft prescribes them. Transport closes, local closes,
partial writes, missing SETUP, unsupported datagrams, and timeouts remain
`NOT_RUN`. Datagram probes require observed negotiated payload capacity and
complete atomic transport acceptance. The stored evidence includes received
stream bytes, submitted bytes and accepted lengths, delivery ordering, and the
peer close code and error space. Driven mode still requires the publisher's
track fixture. Request-error probes such as `request-track-in-single-period-namespace`
(draft 18) and `d21-request-single-period-namespace` (draft 21) instead wait for
an actual response on the matching request stream and verify its error code.
Optional filter and token-cache probes require an observed peer SETUP advertising
sufficient support for their stimulus; unmet prerequisites remain `NOT_RUN`.

Raw probes can select up to 100 distinct scenario IDs in one run. The runner
executes them in the supplied order, using a fresh session for each context and
keeping the same publisher endpoint. `timeout_ms` applies to each context.
For example, a draft-21 GOAWAY run selects
`["d21-duplicate-request-goaway", "d21-goaway-on-distinct-request-streams"]`.
Requirements needing multiple contexts are scored after collection finishes;
an absent or incomplete context cannot supply a pass.

In observed mode, read `/api/v1/runs/{id}/events` and reconnect to the same
endpoint when `context_ready` names the next scenario. Open a fresh QUIC or
WebTransport session each time. Coordinate the publisher connections for that
run; observed mode does not authenticate the publisher's identity. Driven mode
starts the configured publisher process for each context and retains separate
request and process logs. Cancellation and process or listener failures preserve
collected evidence and prevent a complete successful run.

The original typed publication scenarios still accept one scenario per run;
combining them with raw probes returns HTTP 422.

For the draft-18 duplicate-subscription check, set `scenarios` to
`["subscribe-again-to-established-publisher-track"]`. The runner waits for
`SUBSCRIBE_OK` to the first request, sends a second SUBSCRIBE for the identical
track, and scores `D18-5-1-MUST-004` from the response code. If the first
subscription is not established, that conditional requirement remains
`NOT_RUN` rather than becoming a publisher failure.
The same `subscribe-to-publisher-track` run inspects the publisher's SETUP
option types for `D18-10-3-MUST-NOT-001`. It fails repeated known
non-repeatable types, permits repeated AUTHORIZATION TOKEN options, and
leaves repeated unknown extension types unscored. Over WebTransport it also
scores the AUTHORITY and PATH prohibitions and closes with INVALID_AUTHORITY
or INVALID_PATH if either forbidden option is received. Passing these SETUP
rows requires a completed request/response exchange.

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
`d21-publisher-request-stream-placement`, `d21-setup-unknown-options`,
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
The same publisher-announcement run inspects the publisher's SETUP option
types for `D21-9-1-MUST-NOT-289`. It fails repeated known non-repeatable
types, allows repeated AUTHORIZATION TOKEN options, and leaves repeated
unknown extension types unscored because their sender multiplicity rule is
not known to this runner. A pass requires a completed PUBLISH exchange.

The AUTHORITY and PATH profiles deliberately send an otherwise well-formed
server SETUP with one role-forbidden option. They score
`D21-9-1-1-MUST-293` or `D21-9-1-2-MUST-300` only after the local SETUP and
the publisher client's application close are observed. The expected close codes
are `INVALID_AUTHORITY` (`0x19`) and `INVALID_PATH` (`0x8`); a different
application close code fails the selected requirement, while an unobserved or
transport-level close remains `NOT_RUN`. A completed publication after the
forbidden option, without the required close, also fails the requirement.
Over WebTransport, the same probes additionally score the transport-specific
`D21-9-1-1-MUST-294` or `D21-9-1-2-MUST-301`. Native-QUIC runs leave those
WebTransport-only rows `NOT_RUN`.

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
`GET /results/completeness.json` downloads the validator's live completeness
inventory; the `/results` page summarizes the same data by draft and transport.
Each draft entry identifies its source digest and validator revision, catalog
row count, registered required/optional evaluator coverage, static findings,
and classified `not_testable`/`not_applicable`/informative rows with reasons
and draft section/line citations. Each transport entry reports run count,
scored outcome rows, distinct requirements with an evidence-backed pass/fail
observation, execution-audit consistency, and cited `not_run` rows. A pass/fail
observation is not itself proof of conformance: inspect the execution findings
and the individual run's evidence before making a conformance claim. An empty
transport has zero observed coverage and all applicable, testable rows remain
`not_run`. This HTTP inventory reflects the currently stored runs; the release
audit artifact below additionally records exact verification commands, draft
digests, publisher binary/fixture hashes, and stage results.
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
76/175 draft-18 and 76/175 draft-21 applicable, testable MUST/MUST NOT rows
have executable bindings for every named scenario and evaluator. Partially
registered families remain incomplete. A registered binding is a static gate,
not proof that a publisher passed it; run results still require live evidence.
Raw runs collect independent sessions before evaluating the full catalog once.
Every named context remains required; outcomes from separate runs are not merged.

FETCH group-order profiles decode complete Objects on each associated FETCH stream.
Draft-18 executes both explicit orders in one scenario; draft-21 uses ascending,
descending, and default ascending scenarios. A pass needs a typed FETCH_OK,
at least two distinct groups, and a complete stream. Missing named contexts
remain unscored. Draft-18 uses its exclusive end bound and whole-end-group
special case; draft-21 uses its inclusive end bound.

Draft-21 notification probes establish a namespace or FETCH request with a
valid typed response, then send PUBLISH_STATE_NOTIFY on that request stream.
They check for an application PROTOCOL_VIOLATION close. FETCH probes require a
configured track fixture; saved evidence retains the opening, notification,
response bytes, acceptance markers, and peer close code.
Subscriber-direction notification probes cover both SUBSCRIBE and PUBLISH
established subscriptions with configured track fixtures. Both contexts are
required for the complete requirement result.

Request GOAWAY probes wait for typed establishment before sending two GOAWAY
messages on one request stream. The draft-21 control sends one on each of two
independent request streams and requires a typed response on a fresh request;
silence alone does not prove success.

Discovery overlap profiles for both drafts establish active typed discovery
subscriptions before testing exact, ancestor, and descendant common prefixes.
Prefix updates establish A and a disjoint B whose first namespace field differs,
then update B to A on B's actual request stream using a fresh Request ID.
Draft-21 also executes both request types together to prove their independent
overlap spaces; each requirement scores its own type. These profiles require a
track fixture, use only its namespace, and allow at most 31 nonempty fields and
4,094 namespace bytes so the descendant and disjoint controls remain valid.
An empty configured namespace selects the canonical `(a)` fixture. A complete
REQUEST_ERROR with PREFIX_OVERLAP (`0x30`) passes; a typed wrong error or OK fails,
and missing establishment or incomplete/late response evidence stays NOT_RUN.

FETCH first-object profiles for both drafts require a configured track containing
Group 7, Object 9. They request exactly that point (draft-18 uses exclusive end
7/10; draft-21 uses inclusive end 7/9). A typed FETCH_OK and one associated
FETCH stream prove the first ordinary Object. Missing Group or Object ID flags
fail only the corresponding requirement. Complete typed data at 7/9 passes;
empty responses, range markers, other locations, and incomplete evidence remain
NOT_RUN. Object bytes may arrive before FETCH_OK.

Draft-21 FETCH response-count profiles cover accepted and rejected requests.
They require a valid typed reply on the actual request stream and collect
through peer FIN before passing a singleton. Two replies fail immediately;
missing FIN or reset-only closure remains NOT_RUN. Each named context is
required for the full catalogue row to pass, so a run exercising only one
context remains incomplete.

Draft-21 SUBSCRIBE, SUBSCRIBE_NAMESPACE, and SUBSCRIBE_TRACKS response-count
profiles likewise require accepted and rejected contexts. Discovery additionally
requires REQUEST_OK or REQUEST_ERROR to be the first message on the response
stream. Later namespace notifications do not count as additional replies.
Redirect errors for discovery require an empty Track Name. These profiles use
the configured track namespace as the discovery prefix; discovery permits a
valid prefix even when the Track Name is unsuitable for SUBSCRIBE.

Draft-21 Range Filter limit probes prepare their request from the publisher's
actual MAX_FILTER_RANGES advertisement. They send exactly one Range over the
limit across distinct filter keys, or one Range when the limit defaults to
zero. Capacities too large for a bounded request remain NOT_RUN. Duplicate-key
update probes wait for a valid SUBSCRIBE_OK before updating the same stream.
Saved evidence records the prefix used to prepare each payload, its actual
bytes, and its acceptance marker.

Established update probes record when each write was accepted and wait for
the complete peer update on that request's actual stream. Missing, early,
truncated, or unrelated updates cannot establish a passing result.

FETCH cleanup probes require a configured track fixture and an actual open
FETCH data stream. Cancellation sends FIN on the request's sending direction
before STOP_SENDING on its receiving direction. Request and data resets are
scored independently. A failed update requires a valid REQUEST_ERROR and a
reset of its associated data stream. FIN-only results remain NOT_RUN because
transport timing cannot establish a missing reset.
Saved evidence includes actual stream IDs, reset/stop error codes, and accepted
operation markers.

SUBSCRIBE cancellation probes wait for a valid SUBSCRIBE_OK and at least two
open associated subgroup streams before sending STOP_SENDING. Passing requires
actual resets of the request stream and all observed associated open streams,
including reordered late subgroup headers. Ambiguous FIN or alias ownership
results remain NOT_RUN.

Draft-21 server update probes likewise wait for a fully decoded successful
response before reusing the request stream. The duplicate update ID probe
waits for the first update's acknowledgment before repeating its ID, so
outstanding update credits cannot explain the required close.
Draft-21 failed-update probes require an actual complete `REQUEST_ERROR`
before checking subscription `PUBLISH_DONE` with `UPDATE_FAILED`, or discovery
request closure. They verify accepted local FIN and peer FIN or RESET on the
same request stream; `STOP_SENDING` and a RESET after session closure do not
prove the peer's sending direction closed. Cleanup is conditional on rejection
and does not assume an authorization-alias error code.

Unknown authorization alias probes preserve the draft's missing REQUEST_ERROR
assignment. Without a mapping, a structurally valid rejection remains `NOT_RUN`.
To test a deployed mapping explicitly, start the runner with
`--unknown-auth-token-alias-compat-code 0x17` (decimal values also work).
Configured runs record the chosen code in evidence and expose
`scoring_profile: "compatibility"` in JSON and a compatibility label in the
HTML report and TAP diagnostics. These outcomes validate the configured mapping; they do not
establish a standards assignment. A different response code fails the probe.

Draft-21 contribution profiles (`src/scenarios/draft21_contribution_*.cpp`)
each need a configured track fixture. Every context proves the publisher kept
serving requests with a fresh request for that track, and only a complete,
well-framed observation can pass or fail a row; a close, timeout or missing
data leaves it `NOT_RUN`.

- SETUP token registration: an oversized `REGISTER` is sent against the
  publisher's announced `MAX_AUTH_TOKEN_CACHE_SIZE`, or its default of zero.
  Only an application close with `AUTH_TOKEN_CACHE_OVERFLOW` (0x13) fails
  `D21-9-1-4-MUST-NOT-307`.
- GREASE: reserved SETUP options (odd, even and repeated) and an unknown
  `REQUEST_ERROR` code sent to the publisher's own PUBLISH must leave the
  session usable. A close is never scored against the publisher.
- REQUEST_UPDATE accounting: single, pipelined successful and pipelined
  failing updates are counted on the request stream and fenced by a later
  TRACK_STATUS request. More responses than updates fails; fewer stays
  `NOT_RUN` because the fence can overtake data. The `MAX_REQUEST_UPDATES`
  contexts adapt to the publisher's announced limit (zero or omitted means
  unlimited); only the mandated `TOO_MANY_REQUEST_UPDATES` (0x1B) close
  proves the over-limit rule, because an immediate responder never observes it.
- Response types: `SUBSCRIBE_OK` must answer an accepted SUBSCRIBE. FETCH with
  a start far beyond any Largest Object requires `INVALID_RANGE`. The empty
  track FETCH passes only on `INVALID_RANGE` since emptiness is not observable.
  `NAMESPACE_DONE` ordering is scored only after an observed withdrawal.
- Message Parameters in publisher-originated messages (SUBSCRIBE_OK and any
  PUBLISH or PUBLISH_STATE_NOTIFY) are walked with the draft-21 type deltas:
  an overflowing delta fails ordering, an undefined type fails negotiation,
  and a repeated type outside tokens and range filters fails multiplicity.
- Padding: a 128 KiB padding stream and a padding datagram are sent to the
  publisher before an ordinary SUBSCRIBE. A publisher that stops draining the
  stream stalls the probe and stays `NOT_RUN`.
- Object delivery: Forward State 0 must deliver no Objects until a
  `REQUEST_UPDATE` sets Forward 1. Two subscriptions or FETCHes differing only
  in delivery parameters must carry identical payloads. Gap, forwarding
  preference and FETCH datagram-flag profiles require a track containing
  Group 7, Object 9 (as the first-object profiles do) and compare its
  subscription delivery with a FETCH of the same Object. Datagram and Subgroup
  header bits, Subgroup FIN after End of Group, and reset after Forward 0 are
  scored from the Objects the publisher actually produces.

After a test series finishes, add `--database /path/to/runs.sqlite3` to audit
stored execution evidence. The JSON output gains `execution_audit` with
per-run canonical SHA-256 hashes, scored-row counts, and explicit findings for
passed rows missing declared evidence, score mismatches, active/error runs,
and inconsistent repeats. Run ID, timestamps, and incidental evidence arrival
order are excluded from the hash, while evidence kind counts remain significant;
draft, transport, track, timeout, configured compatibility mapping, and validator revision remain part of the
comparison group. A zero-run audit can be consistent but proves no behavior.
Compare repetitions only when the publisher binary and fixture are the same;
publisher identity is not yet stored as a grouping key. Run this audit after
the service has stopped creating runs so pagination sees a stable database.

The draft release gate accepts a new output directory, audit CLI, and optional
Docker image, synthetic native peer, moqxr executable, and MP4 fixture:

```sh
bash tests/e2e/release-audit.sh run /tmp/moq-interop-release-audit \
  "$PWD/build/moq-interop-audit" "moq-interop-runner:$(git rev-parse --short HEAD)" \
  "$PWD/build/moq-interop-picoquic-peer" \
  /path/to/openmoq-publisher /path/to/locmaf-publisher.mp4
```

Use an image built from the exact current commit; a mismatched revision label
is rejected. Omitting the four optional arguments records the Docker and
publisher stages as `missing`. The command refuses to overwrite an existing
output directory. It writes `release-audit.json`, full draft audits, command
receipts, Docker run results/events, moqxr repeat databases, and logs. The
report records the external publisher version, executable SHA-256, and fixture
SHA-256 so a passing matrix cannot be confused with a different local build.
The `check` mode validates that artifact against the checked-in draft digests and
current source revision. Native tests, focused ASan/UBSan tests, and bounded
libFuzzer smoke tests run in either mode. The manually dispatched `Draft release
audit` workflow builds a pinned moqxr checkout and source-matched image, retains
the same evidence, and fails until every required stage and static gate passes.
The gate is intentionally failing now because both draft catalogs still lack
many evaluators. Local audits require Clang with libFuzzer and `timeout`.

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

Draft-21 MAX_FILTER_RANGES rejection coverage includes distinct named contexts
for an initial aggregate exceeding the advertised positive cap, the omitted
SETUP option default of zero, and a REQUEST_UPDATE adding SetID 1 while
retaining the acknowledged subscription's SetID 0 filters. The last context
exceeds the concurrent cap even though the update itself adds only one range.
