# MoQ Contribution Interop Runner Design

**Date:** 2026-09-27

## Purpose

The project validates Media over QUIC Transport contribution publishers against
publisher-applicable requirements in selected MoQT drafts. It presents itself to
a publisher as the receiving side of a relay, but it does not forward objects to
real subscribers. Instead, it drives controlled relay-to-publisher interactions,
records observable behavior, evaluates that evidence against the draft, and
serves persistent results over plain HTTP.

The first protocol targets are `draft-ietf-moq-transport-18` and
`draft-ietf-moq-transport-21`. The validator is implementation-neutral.
`moqxr` is the first known publisher supporting both target drafts and will be
used as an external black-box end-to-end fixture, not as a source of protocol
truth or reusable MOQT code.

## Success Criteria

The first complete release must:

1. Build and run as a Docker container on Linux.
2. Accept contribution publishers over native QUIC and WebTransport.
3. Select draft 18 or draft 21 explicitly and reject a mismatched protocol.
4. Exercise publisher behavior without serving any downstream subscriber.
5. Inventory every normative statement in both checked-in draft text files.
6. Classify every statement for contribution-publisher applicability and
   external testability, including statements the runner cannot test.
7. Map every externally testable, publisher-applicable requirement to one or
   more deterministic scenarios and evidence evaluators.
8. Preserve per-requirement results and supporting protocol evidence.
9. Expose machine-readable JSON and a minimal human-readable HTML report over
   plain HTTP.
10. Produce independent results for draft 18 and draft 21 without treating one
    as an alias for the other.
11. Run end-to-end against `moqxr` for each transport and draft combination that
    the publisher supports.
12. Document building, operating, integrating a publisher, interpreting scores,
    and extending the draft or scenario catalogs.

## Design Principles

### The drafts are the protocol authority

The text versions of the two drafts live under `docs/` and are reviewed before
protocol code or test expectations. Message constants, field layouts, stream
rules, state transitions, expected errors, and golden wire bytes are derived
from those files.

No MOQT codec, state machine, expected byte sequence, or protocol fixture is
copied from `moqxr`, `moq5`, `moq-rs`, or another current client or relay.
Existing interoperability projects may inspire process-level features such as
stable test identifiers, timeouts, diagnostics, TAP output, adapters, and Docker
workflows, but they do not define correct MOQT behavior.

### Independence is enforced at module boundaries

The application is a fresh C++20 implementation. Picoquic supplies QUIC and
its H3zero component supplies HTTP/3 primitives. This repository owns strict
WebTransport admission, session-to-stream mapping, and all MOQT behavior.
Neither picoquic's examples nor any publisher implementation define expected
MOQT behavior. Native test peers also use picoquic; quiche build options and
legacy backend targets have been retired.

The protocol implementation has explicit draft-18 and draft-21 modules. They
share bounded byte-buffer and integer primitives only when the draft texts
define identical behavior. Draft-dependent behavior is never selected through
a catch-all `draft 18 or later` path.

### Requirements, scenarios, and evidence are separate

A requirement is a normative statement from a draft. A scenario is a controlled
interaction that may exercise several requirements. Evidence is the immutable
record of what happened. Evaluators convert evidence into per-requirement
outcomes. Reorganizing scenarios cannot silently add, remove, or reword the
requirements used for scoring.

## Scope

### In scope

- Native QUIC and WebTransport server transports required for the target drafts.
- MOQT session establishment and termination as observed by a relay-side peer.
- Publisher-originated namespace and track publication.
- Relay-originated namespace discovery, track discovery, subscriptions,
  updates, fetches, cancellation, and error stimuli needed to validate a
  publisher.
- Subgroup streams and object datagrams.
- Object metadata, properties, priorities, filters, timeouts, lifecycle, and
  request-stream behavior required by drafts 18 and 21.
- Positive, boundary, and negative protocol tests where the draft prescribes a
  publisher response to peer input.
- Persistent run results, scoring, evidence, JSON, HTML, and optional TAP export.
- Manual publishers, externally managed publishers, and adapter-driven Docker
  publishers.

### Out of scope

- Forwarding objects to downstream subscribers.
- Acting as a production relay, cache, CDN, or authorization service.
- Validating media payload formats such as CMAF, LOC, or MSF. Object payloads
  are opaque to this project.
- Judging requirements that cannot be observed at the protocol boundary. These
  remain visible as `NOT_TESTABLE`.
- Treating optional feature absence as a protocol conformance failure.
- Inferring draft behavior from a client implementation when the draft is
  unclear. Ambiguities are recorded and resolved through draft errata or working
  group guidance before an evaluator becomes normative.
- Combining scores from different draft versions.

## Technology

- C++20 application code.
- CMake for configuration, build, test registration, and install rules.
- Pinned picoquic, H3zero, and picotls sources for QUIC, HTTP/3, and TLS.
- SQLite3 for durable run, outcome, and evidence indexes.
- nlohmann/json for manifests and JSON API serialization.
- cpp-httplib for the plain HTTP control and results service.
- GoogleTest for unit, protocol, and integration tests.
- libFuzzer-compatible parser and state-machine fuzz targets.
- AddressSanitizer and UndefinedBehaviorSanitizer CI configurations.
- A multi-stage Docker build containing pinned dependency revisions.

Dependency versions and source revisions are recorded in the build so a result
document can identify the exact validator binary and dependency set that
produced it.

## Repository Structure

```text
CMakeLists.txt
cmake/
  Dependencies.cmake
include/moq/interop/
src/
  app/
  http/
  requirements/
  scenarios/
  session/
  storage/
  transport/
  wire/common/
  wire/draft18/
  wire/draft21/
requirements/
  schema.json
  draft18.json
  draft21.json
tests/
  unit/
  golden/
  protocol/
  integration/
  e2e/
  fuzz/
adapters/
  moqxr/
docs/
  draft-ietf-moq-transport-18.txt
  draft-ietf-moq-transport-21.txt
Dockerfile
compose.yaml
```

Public headers contain stable interfaces between modules rather than exporting
transport-library types. Source files are grouped by responsibility. Draft
modules own their message and parameter types so a type with draft-dependent
wire semantics cannot cross a version boundary accidentally.

## Architecture

### Transport adapters

The native QUIC listener negotiates only `moqt-18` or `moqt-21` for a run. The
WebTransport listener negotiates HTTP/3, validates the extended CONNECT request,
and requires the matching MOQT application protocol. Each transport converts
transport events into a small internal interface:

- accept or close a session;
- open, accept, read, write, reset, or stop a stream;
- send or receive a datagram;
- expose negotiated protocol and connection identifiers;
- expose timer and transport-error events.

MOQT modules do not call picoquic directly. This keeps transport mechanics from
changing protocol interpretation and permits transport-specific integration
tests.

### Picoquic migration and strict WebTransport profile

Both native QUIC and WebTransport use picoquic in the production runner. The
native listener implements the existing `SessionTransport` interface. Draft selection remains explicit: a run
accepts its exact `moqt-18` or `moqt-21` ALPN and never falls back to another
draft. QUIC DATAGRAM support is required for both drafts. A native MOQT stream
may use RESET_STREAM or RESET_STREAM_AT according to the negotiated extension;
absence of RESET_STREAM_AT alone is not a native-MOQT failure.

The WebTransport listener uses H3zero for HTTP/3 framing and QPACK, but does
not accept H3zero's legacy WebTransport compatibility behavior as proof of
conformance. The target draft's WebTransport reference is authoritative:
draft 21 references `draft-ietf-webtrans-http3-16`. Admission requires its
WebTransport-capable HTTP/3 settings and QUIC transport parameters, including
RESET_STREAM_AT, before processing a CONNECT. The request must use exact
`webtransport-h3`, `https`, the configured authority and path, and an allowed
Origin when present. Its `WT-Available-Protocols` value must be a valid
Structured Fields list of strings containing the run's exact MOQT protocol;
the selected `WT-Protocol` is one offered value. Malformed or missing required
negotiation is rejected, not silently interpreted as a legacy WebTransport
version or a different MoQT draft. Draft 18 instead references
`draft-ietf-webtrans-http3-15`; its admission profile is checked separately
against that version before enablement. The two profiles share behavior only
where their referenced texts define the same semantics.

The project-owned WebTransport adapter binds the accepted CONNECT session ID
to its streams and HTTP datagrams, strips and emits only the prescribed
WebTransport framing, translates reset and stop errors, and handles session
close. A wrong session ID, malformed framing, unnegotiated datagram, or
resource-limit violation cannot enter the MOQT decoder. H3zero changes needed
to prevent legacy settings, token, or header acceptance are carried as small,
pinned build-time patches with tests; the sibling picoquic checkout is never
modified by this project. The final Docker runtime contains picoquic and its
TLS dependencies.

Migration is complete only when existing native draft-18 and draft-21
scenarios produce equivalent evidence and scores through picoquic, strict
WebTransport transport tests pass.
Until then, an unimplemented transport profile is reported as unsupported;
there is no transparent backend fallback. Transport negotiation and internal
adapter failures remain distinct from publisher-MOQT requirement failures.

### Wire modules

`wire/common` provides bounds-checked cursors, byte spans, QUIC variable-length
integers where the drafts use them, length-delimited values, and structured
decode errors. It does not define MOQT messages.

Each draft module defines its own:

- protocol version and ALPN/application-protocol identifiers;
- setup options;
- control, request, response, and error messages;
- parameter registries and parameter scopes;
- subgroup headers, object datagrams, object fields, and status values;
- validation rules for reserved, duplicate, unknown, and malformed values;
- encode and decode entry points returning typed values or bounded errors.

Decoders never allocate based solely on an untrusted peer length. Configurable
limits are applied before allocation and before retaining partial frames.

### Relay-side session model

The session layer implements only behavior needed to interact with a
contribution publisher. It tracks:

- negotiated draft and setup options;
- stream roles and incremental receive buffers;
- request identifiers and request-stream lifetimes;
- published namespace and track state;
- track aliases and properties;
- subscription, fetch, and request-update state;
- expected object ranges, forward state, priorities, and delivery limits;
- received groups, subgroups, objects, FINs, resets, and datagrams;
- terminal request and session errors.

The state machine accepts fragmented messages and multiple messages delivered
in one read. It records a typed event before an evaluator changes any outcome.
An evaluator cannot mutate session state.

### Scenario engine

A scenario is a declarative sequence of preconditions, relay actions, expected
event classes, deadlines, and completion rules. Scenario code chooses legal or
deliberately invalid peer inputs from the selected draft module. It does not
contain scoring weights or duplicate requirement text.

The engine supports two modes:

1. **Observed mode:** a manually or externally started publisher connects and
   behaves normally. The validator discovers namespaces and tracks and scores
   everything observable without controlling publisher behavior.
2. **Driven mode:** an adapter starts or configures the publisher for a named
   scenario using the publisher-driver contract.

The publisher-driver contract supplies:

- validator endpoint;
- selected draft and transport;
- run and scenario identifiers;
- namespace and track fixtures;
- media or opaque-object fixture path when needed;
- scenario and process deadlines;
- TLS trust material;
- output directory for publisher logs.

An adapter translates this neutral contract into implementation-specific
arguments. Adapter output is diagnostic evidence only. Expected MOQT behavior
always comes from the requirement catalog and selected draft module.

### Run isolation

`POST /api/v1/runs` reserves an isolated listener from configured UDP port
ranges and returns the exact endpoint to give the publisher. A run selects one
draft and one transport. Connections with a mismatched ALPN or application
protocol are rejected and recorded.

Each run owns its session state, deadlines, scenario selection, evidence stream,
and result aggregation. Port-range exhaustion returns HTTP 503 `publisher_ports_exhausted` rather
than sharing state between runs. Stopping a run closes its listeners and active
sessions without deleting evidence.

Raw probe runs accept up to 100 distinct scenario IDs in the supplied order.
Each context uses a fresh native QUIC or WebTransport session on the same
reserved endpoint, with its own timeout and transport evidence indexes. The
worker retains actual transcripts and evaluates the unchanged catalog once;
separate run results never supply missing contexts. The original typed
controllers continue to accept one scenario per run.

Observed publishers reconnect when the events API reports `context_ready` for
the next scenario. This contract coordinates sessions without authenticating
publisher identity. Driven runs restart the configured process for each context
and retain separate request and process logs. Cancellation, listener errors,
process errors, and forced termination after the stop grace mark the run
`ERROR`; incomplete contexts cannot supply a pass. Bound ports remain reserved
through listener recreation and draining cancellation.

## Requirement Catalog

The checked-in draft text file has a recorded SHA-256 digest. A requirement
catalog entry contains:

- stable requirement ID such as `D18-5.1-MUST-003`;
- draft number and source digest;
- section and source line range;
- normative strength;
- the actor named by the draft;
- a concise normalized requirement;
- contribution-publisher applicability and rationale;
- external testability and rationale;
- scenario and evaluator identifiers where testable;
- any recorded ambiguity and its resolution source.

A catalog audit scans the draft for normative keywords and requires every
occurrence to be classified. One sentence containing multiple independent
normative clauses produces separate entries. Informative uses of normative
words are classified explicitly instead of being silently discarded.

Catalog validation fails when a source digest changes, an ID is duplicated, a
citation is outside the source file, an applicable testable requirement lacks
an evaluator, or an evaluator names an absent scenario.

## Scoring and Verdicts

Requirement weights are:

| Strength | Weight | Conformance effect |
|---|---:|---|
| `MUST` or `MUST NOT` | 10 | Any observed failure fails the run |
| `SHOULD` or `SHOULD NOT` | 3 | Failure is reported as an advisory |
| `MAY` or optional capability | 1 | Measures feature breadth only |

Each row has exactly one outcome:

- `PASS`: all required scenario contexts completed and the evidence satisfies
  the requirement.
- `FAIL`: observed evidence contradicts the requirement. One contradictory
  observation dominates successful repetitions.
- `NOT_RUN`: the requirement is testable, but its required scenario contexts
  did not complete.
- `NOT_TESTABLE`: the behavior is not externally observable by this harness,
  with a catalog rationale.
- `NOT_APPLICABLE`: the statement does not apply to a contribution publisher,
  with a catalog rationale.

The report calculates:

- **Required score:** passed applicable, testable `MUST` weight divided by all
  applicable, testable `MUST` weight.
- **Weighted score:** passed applicable, testable weight divided by all
  applicable, testable weight.
- **Coverage:** executed applicable, testable weight divided by all applicable,
  testable weight.

`NOT_TESTABLE` and `NOT_APPLICABLE` rows remain visible and are excluded from
numerators and denominators. `NOT_RUN` contributes no passed weight and lowers
coverage. Unsupported optional behavior may produce a failed capability row but
does not by itself make the conformance verdict fail.

The run verdict is:

- `PASS` when every applicable, testable required statement passes;
- `FAIL` when any applicable, testable required statement fails;
- `INCOMPLETE` when none fails but one or more applicable, testable statements
  are `NOT_RUN`;
- `ERROR` when the validator cannot establish or preserve a valid run.

Draft scores and verdicts are always independent.

## Initial Scenario Catalog

### Transport and setup

- Native QUIC setup for the exact selected draft.
- WebTransport extended CONNECT and application-protocol selection.
- Required and optional setup options.
- Duplicate, unknown, malformed, and out-of-scope options.
- Legal and illegal stream roles and session termination.

### Namespace contribution

- Publisher-originated `PUBLISH_NAMESPACE` acceptance and rejection.
- Namespace request completion, cancellation, and stream lifetime.
- Relay-originated `SUBSCRIBE_NAMESPACE` and `SUBSCRIBE_TRACKS`.
- Matching and nonmatching namespaces, overlap, and `PUBLISH_SKIPPED`.

### Track publication

- Publisher-originated `PUBLISH`.
- Acceptance with Forward state zero and one.
- Rejection and exactly-one-response behavior.
- Multiple tracks, track aliases, properties, priorities, and duplicate state.
- Track publication resulting from namespace or track discovery requests.

### Subscriptions and updates

- `SUBSCRIBE` before and after publication discovery.
- Response correlation and exactly-one-response behavior.
- Same-request-stream `REQUEST_UPDATE`.
- Forward pause and resume, priority changes, legal parameter replacement, and
  cancellation.
- Fragmented input, coalesced updates, invalid scope, and failed-update cleanup.

### Object delivery

- Subgroup stream headers and object datagrams.
- Track alias, group, subgroup, object identity, object status, and payload
  length.
- First-object signaling, multiple groups and subgroups, ordering, FIN, reset,
  and incomplete object behavior.
- Track and object properties and unknown-property preservation where observable.
- Delivery timeout behavior where it can be stimulated and observed.

Payload bytes are treated as opaque. No scenario requires a specific media
container or codec.

### Fetch and joining behavior

- Fetch acceptance or rejection, ranges, response fields, object delivery,
  completion, cancellation, and reset.
- Joining fetch and the interaction between retained and live objects.

### Limits and protocol errors

- Invalid message placement and stream type.
- Duplicate or invalid identifiers and aliases.
- Malformed integers, lengths, names, properties, and parameters.
- Unsupported messages and mandatory properties.
- Required request errors, stream resets, and session errors.
- Per-run memory, stream, object, and deadline limits.

### Draft-21 delta scenarios

- Location filters and all negotiated range-filter forms.
- Filter combination, replacement, removal, and conflict handling.
- `FILL_PARAMETERS` inheritance, override rules, and fill-fetch streams.
- `INCLUDE_PROPERTIES` behavior.
- `LARGEST_OBJECT` response and update behavior.
- `MAX_REQUEST_UPDATES` enforcement and `TOO_MANY_REQUEST_UPDATES`.
- Draft-21 response aliases and the reserved legacy `PUBLISH_OK` code.
- `SUBSCRIBE_TRACKS` group order and filter propagation.

Every scenario has a draft-specific definition. A shared scenario helper may be
used only when both catalogs cite identical requirements and both golden suites
verify the resulting bytes independently.

## Evidence

The validator records:

- monotonic and wall-clock timestamps;
- run, connection, stream, request, namespace, track, and object identifiers;
- transport negotiation and close information;
- raw MOQT frame bytes after transport deframing;
- decoded typed messages and validation decisions;
- scenario actions and deadlines;
- state transitions;
- evaluator inputs, expected behavior, observed behavior, and outcome;
- validator build, source revision, draft digest, dependency revisions, and
  runtime configuration;
- publisher identity and adapter metadata supplied by the operator.

Large packet captures and publisher logs are stored as files referenced by path,
size, and SHA-256 digest. The database stores sufficient evidence to explain a
result without requiring those optional artifacts.

## Persistence

SQLite stores schema-versioned data for runs, connections, scenarios,
requirements, outcomes, and evidence indexes. Outcomes are immutable after a
run is finalized. A rerun creates a new run rather than modifying history.

The service applies migrations at startup only when it recognizes both source
and destination schema versions. It refuses to start against a newer unknown
schema. Database transactions finalize scenario outcomes and their evidence
references atomically.

## HTTP API

The plain HTTP service listens on `127.0.0.1:8080` by default. The Docker image
configures it for `0.0.0.0:8080`. The initial API is local-tooling oriented and
has no authentication; documentation warns against exposing it to an untrusted
network.

| Method and path | Purpose |
|---|---|
| `GET /healthz` | Process, database, and listener readiness |
| `GET /api/v1/drafts` | Supported drafts and requirement counts |
| `GET /api/v1/requirements?draft=18` | Full requirement inventory |
| `POST /api/v1/runs` | Create a draft/transport/scenario run |
| `GET /api/v1/runs` | List runs and summary verdicts |
| `GET /api/v1/runs/{id}` | Full run, scoring, and requirement outcomes |
| `GET /api/v1/runs/{id}/events` | Paginated protocol and evaluator evidence |
| `POST /api/v1/runs/{id}/stop` | Stop a pending or active run |
| `GET /results` | Human-readable filtered report |
| `GET /results/{id}.json` | Stable machine-readable result document |
| `GET /results/{id}.tap` | Optional scenario-level TAP 14 export |

`POST /api/v1/runs` accepts a draft, transport, mode, scenario selection,
and timeout. It returns the run identifier and assigned publisher endpoint.
Native QUIC returns an address, UDP port, and `moqt-18` or `moqt-21` ALPN.
WebTransport returns an HTTPS URL and path, HTTP/3 ALPN, and selected
`moqt-18` or `moqt-21` WebTransport application protocol. The operator supplies
the PEM certificate and private key at startup and must separately establish
publisher trust in that certificate; the response does not embed private key
material or claim a publisher has trusted the certificate.

JSON responses carry an API schema version. Collection endpoints are paginated.
Malformed input returns a structured client error; port range exhaustion and an
unavailable listener return HTTP 503; internal storage or listener failures mark
the run `ERROR` where possible. The current routes, fields and error codes are in
[http-api.md](http-api.md).

## HTML Report

The report is server-rendered and usable without JavaScript. It uses dark text
on light backgrounds, clearly contrasting status colors, and text labels so
color is never the only status signal.

The report provides:

- verdict, required score, weighted score, and coverage;
- draft, implementation, transport, and run metadata;
- filters for strength, outcome, draft section, and scenario;
- every applicable requirement and its draft citation;
- applicability and testability rationale;
- expandable expected-versus-observed evidence;
- dedicated views for `NOT_RUN`, `NOT_TESTABLE`, and `NOT_APPLICABLE` rows;
- JSON and TAP download links.

## Error Handling and Resource Safety

The service distinguishes harness errors from publisher outcomes. Configuration,
listener, database, manifest, or internal invariant failures produce `ERROR`.
A valid stimulus that elicits incorrect publisher behavior produces a scored
`FAIL`.

Every network operation has a deadline. Limits cover sessions, streams, partial
frame bytes, message length, property count, namespace and track length, object
payload retained for evidence, event count, database growth per run, and total
run duration. Limit violations follow the selected draft when it prescribes a
peer-visible result; otherwise they end the run with an explicit harness error.

Unexpected exceptions are caught at thread and request boundaries. The process
does not terminate because one publisher sends malformed input. Secrets such as
private TLS keys are never returned through result endpoints or recorded in
evidence.

## Verification Strategy

### Catalog completeness

A deterministic audit scans each checked-in draft for normative keywords. Tests
prove that every occurrence is mapped to one or more catalog records and that
every applicable, testable record names a valid evaluator and scenario.

### Golden wire tests

Every supported message, option, parameter, subgroup header, datagram, and
error code has hand-derived expected bytes. Encode tests compare exact bytes;
decode tests begin from the hand-derived bytes rather than encoder output.
Boundary vectors cover every variable-length integer width, zero and maximum
legal lengths, truncation at every byte boundary, duplicate fields, reserved
values, and values immediately outside allowed ranges.

### State-machine tests

Tests split each legal frame at every byte boundary, coalesce legal frames,
interleave stream events, and inject FIN, reset, stop-sending, timeout, and close
events at each meaningful state. They verify both the state transition and the
evidence emitted for evaluation.

### Fuzzing and sanitizers

Fuzz targets cover each draft decoder, parameter parser, WebTransport stream
mapping, session event dispatcher, and requirement evaluator. CI runs bounded
fuzz smoke tests under address and undefined-behavior sanitizers. Longer fuzz
runs can reuse the same targets outside CI.

### Integration tests

An independent byte-scripted publisher fixture uses literal golden frames and
does not call production encoders. It covers transport/session integration,
run isolation, persistence, API responses, report rendering, and timeout paths.
Picoquic supplies the native test peer. Test expectations come from the
checked-in drafts and the exact WebTransport draft they reference. Negative
tests cover legacy WebTransport settings and tokens, missing required settings
or transport parameters, malformed Structured Fields, wrong MOQT protocol,
CONNECT origin and path errors, incorrect session IDs, datagram boundaries,
reset and stop error translation, and connection/session cleanup.

### Black-box interop

Docker Compose starts the validator and the target publisher through an adapter.
The first adapter targets `moqxr` for native draft 18, native draft 21, and each
supported WebTransport combination. Expected results come from this project's
draft catalogs. Known publisher limitations appear as ordinary failed,
unsupported, or not-run requirement rows rather than being built into the
validator.

When another publisher supports draft 18 or 21, it can implement the same driver
contract without changes to protocol or evaluator code.

## Implementation Status

As of 2026-10-01, every applicable, testable MUST/MUST NOT row has an
executable scenario and evaluator binding: 173 of 173 for draft 18 and 173 of
173 for draft 21. Four rows were reclassified `not_testable` because the
behavior cannot be observed or expressed on the wire even from a cooperating
publisher; each carries a draft citation in the catalog. The static
completeness gate therefore passes for both drafts. Optional-row coverage is
1/90 and 1/97 and appears as non-blocking findings. A binding is a static
gate, not proof of publisher behavior: many scenarios can only pass on
positive wire evidence and stay `NOT_RUN` when the publisher never produces
the behavior. Live results against `moqxr` are reported per row and are never
used to define expected behavior. `D18-10-2-2-MUST-008` and `-010` (and their
draft-21 twins) score only with an operator-supplied credential
(`--invalid-auth-token`, `--expired-auth-token`) and are `NOT_RUN` otherwise.

## Delivery Milestones

### 1. Protocol corpus and application foundation

Add the exact draft text files and their digests, requirement schemas and
catalog audit, scoring engine, SQLite schema, HTTP skeleton, CMake project,
tests, and multi-stage Docker build. This milestone can create runs and render a
complete inventory even though protocol-driven rows are still `NOT_RUN`.

### 2. Draft-18 native QUIC

Implement native transport, draft-18 wire codecs, relay-side session state, and
the draft-18 setup, namespace, track, subscription, update, object, fetch,
lifecycle, limit, and error scenario families. Complete golden, state-machine,
fuzz, and scripted-publisher coverage before enabling scored draft-18 results.

### 3. Draft-21 native QUIC

Perform an explicit draft-18-to-21 requirement and wire delta audit. Implement
draft-21 modules and scenarios for filters, fill, properties, largest-object,
request-update limits, response-code changes, and track discovery changes. Do
not enable draft-21 scoring until every applicable catalog row is reconciled.

### 4. WebTransport

Implement HTTP/3 extended CONNECT, WebTransport session negotiation, stream
mapping, datagrams, close behavior, and transport-specific limits. Run the same
applicable draft scenario suites over WebTransport and keep transport-specific
requirements and failures visible.

### 5. Publisher integration and operations

Finalize the publisher-driver contract, add the `moqxr` adapter and Compose
matrix, expose JSON/TAP exports, complete the accessible report, and document
local, Docker, manual-publisher, adapter, and CI workflows.

### 6. Completeness audit

Reconcile every draft catalog entry with evaluator coverage and evidence. Run
clean unit, golden, protocol, integration, sanitizer, fuzz-smoke, Docker, and
supported `moqxr` end-to-end suites. Publish the remaining `NOT_TESTABLE`,
`NOT_APPLICABLE`, and `NOT_RUN` reasons. A draft is called complete only when
the catalog audit passes and every applicable, externally testable required
statement has an implemented evaluator.

## Documentation Deliverables

The project documentation will cover:

- architecture and trust boundaries;
- exact build and Docker commands;
- configuration and port ranges;
- HTTP API and result schema;
- scoring and verdict interpretation;
- manual observed-mode operation;
- the publisher-driver adapter contract;
- the `moqxr` Compose example;
- adding a requirement, evaluator, scenario, or draft;
- testing, fuzzing, and troubleshooting;
- declared limitations and the current completeness report.

Documentation examples use high-contrast diagrams if diagrams are needed. No
result or diagram relies on color alone.
