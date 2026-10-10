# Scenario Reference

This document tells a tester what each family of scenarios needs from the
publisher and from the operator: the track fixture a scenario expects, the
credentials and runner flags that unlock otherwise unscorable rows, port
requirements, transport restrictions, and what `NOT_RUN` means for each family.
Scenarios are grouped by feature, not by development history. Requirement IDs
(for example `D18-5-1-MUST-004`) are the catalog IDs served by
`GET /api/v1/requirements`; the draft text in this directory is the authority
for what each one requires. For how outcomes and scores work see
[scoring-and-audit.md](scoring-and-audit.md); for the request format see
[http-api.md](http-api.md).

## Rules that apply to every scenario

- A scenario is a controlled interaction. A row passes or fails only on bytes the
  publisher put on the wire. Silence, an early close by the runner, a missing
  prerequisite or an ambiguous trace leave the row `not_run`, never `fail`.
- Raw-probe runs accept up to 100 distinct scenario IDs. The runner executes them
  in the order given, uses a fresh session for each context on the same
  endpoint, and applies `timeout_ms` to each context. The full catalog is then
  evaluated once. A requirement that names several contexts needs all of them
  in the same run to pass; an absent or incomplete context cannot supply a pass.
- The original typed scenarios (`subscribe-to-publisher-track`,
  `subscribe-again-to-established-publisher-track`,
  `d21-publisher-request-stream-placement`, `d21-setup-unknown-options`,
  `d21-setup-duplicate-unknown-options`, `d21-server-sends-authority`,
  `d21-server-sends-path`, and the typed FETCH and discovery profiles) take
  exactly one scenario per run. Mixing them with raw probes returns HTTP 422.
- In observed mode, read the run's events and reconnect each time a `context_ready`
  event names the next scenario, using the endpoint URI in that event: the address
  and port stay the same, but some native-QUIC contexts vary the path or query, and
  one draft 18 context uses a URI with an empty host. In driven mode the
  adapter is started once per context and receives the scenario ID in its
  request. Observed mode does not authenticate the publisher's identity.
- QUIC DATAGRAM must be negotiated; see [the harness guide](publisher-harness-guide.md#7-requirements-your-publisher-must-meet).
- Requests for unsupported scenarios return HTTP 422; they are never scored as
  conformant.

- A publisher may declare that it does not implement FETCH (`publisher_capabilities`
  in the run request, or `--publisher-no-fetch` at startup). Scenarios that need
  FETCH are then skipped, not failed; see [Scenarios that need FETCH](#scenarios-that-need-fetch).

List the executable scenarios (165 for draft 18, 222 for draft 21 and 221 for draft 22
at the time of writing) with the `/healthz` command in [http-api.md](http-api.md). A
scenario that needs a `track` and is submitted without one returns 400
`invalid_run_config`.

## Track fixture contracts

`track.namespace_hex` and `track.name_hex` name the track the runner will
request. The publisher under test must serve that track unless stated
otherwise. The runner does not check the media; payload bytes are opaque.

| Contract | Used by | What the publisher must have published |
|---|---|---|
| Configured track | Most subscription, discovery and response-count scenarios | The track exists and is served when the runner subscribes or fetches. The fixture names the target; it does not assert that an Object was already published |
| Group 7, Object 9 | FETCH first-object profiles for both drafts, Object comparison, LARGEST_OBJECT, gap, forwarding-preference and FETCH datagram-flag profiles, and several draft-18 and draft-21 probes noted below | The track retains Group 7 from its first Object, including Object 9. Draft 18 requests it with an exclusive end of 7/10, draft 21 with an inclusive end of 7/9 |
| Two small Groups | These draft-21 scenarios: `d21-overlapping-subscriptions-shared-alias` / `-distinct-aliases`, `d21-forward-location-and-range-filter-conjunction`, `d21-fill-fails-before-first-object`, `d21-cancel-subscription-with-concurrent-fill-streams`, `d21-subscribe-tracks-publish-skipped-then-capacity-recovers`, `d21-subgroup-completion-withheld-acknowledgments`, and the publisher-initiated and token-credential scenarios described in the families below | Groups 0 and 1, each with Objects 0 and 1 (the shape of one GOP per Group) |
| Namespace only | Discovery-prefix and namespace-overlap scenarios | Only the namespace is used |

The fixture the bundled moqxr adapter ships with publishes Groups 0 and 1 (see
[interop-notes.md](interop-notes.md)), so scenarios that need Group 7, Object 9
stay `not_run` against it. The same holds for any publisher whose test track
does not contain the requested Location.

Namespace rules enforced by the runner:

- 0 to 32 nonempty fields, at most 4096 bytes of namespace plus track name.
- Discovery-overlap profiles (exact, ancestor, descendant and prefix-update
  checks) use only the fixture's namespace, allow at most 31 nonempty fields and
  4094 namespace bytes, and require a nonreserved first field. An empty
  configured namespace selects the canonical `(a)` fixture.
- Reserved-namespace scenarios validate the namespace themselves: `.` for the
  three `d21-attempt-single-period-*` scenarios, a longer period-prefixed field
  other than `.session` for
  `d21-attempt-unregistered-period-namespace-publication`, and `.session` for the
  two `d21-application-*-under-session` scenarios. The configured namespace is
  what the publisher is told to publish, so the adapter or operator must make the
  publisher attempt it. The runner rejects a PUBLISH under `.` with
  `DOES_NOT_EXIST`.

## Operator-supplied credentials and policy

Draft 18 defines no Token Type for these rows, and the runner cannot know your
authorization policy. The rows below score only when you start the runner with
the matching option, and the publisher must be configured to match. Without the
option the scenario sends nothing and the row is `NOT_RUN`. A passing static
audit does not mean these rows score by default.

| Flag | Scenarios and rows | Behavior |
|---|---|---|
| `--invalid-auth-token TYPE:HEX` | `receive-well-formed-token-with-invalid-known-type-value` (`D18-10-2-2-MUST-008`); `d21-request-well-formed-invalid-token` (`D21-8-9-MUST-270`) | The credential is a well-formed token of a Token Type the publisher understands but with an invalid value. This is an operator attestation the runner cannot verify: REQUEST_OK is scored FAIL on the strength of it, so supply a credential only for a publisher that validates that Token Type (moqxr validates none). Draft 18: sent as USE_VALUE on a SUBSCRIBE_NAMESPACE for the fixture namespace; passes on REQUEST_ERROR `MALFORMED_AUTH_TOKEN` (0x4), fails on acceptance or any other error code; `NOT_SUPPORTED`, silence or a close leave the row `NOT_RUN` |
| `--expired-auth-token TYPE:HEX` | `register-token-expire-then-use-alias-before-delete` (`D18-10-2-2-MUST-010`); `d21-expired-token-alias-lifetime` (`D21-8-9-MUST-273`) | Draft 18: registered under Alias 1, which must fail with `EXPIRED_AUTH_TOKEN` (0x5); USE_ALIAS must also fail with it; registering Alias 1 again must close the Session with `DUPLICATE_AUTH_TOKEN_ALIAS` (0x14). A publisher that does not register, accepts the credential, stays silent or times out leaves the row `NOT_RUN` |
| `--denied-authorization-token VALUE` | `D21-9-15-MUST-386` and `D21-9-18-MUST-394` (draft 21 discovery authorization probes) | Token type 0 with this value is sent on a discovery request. Only when the option is set does the runner assume the publisher refuses the credential: REQUEST_OK then fails the row and a REQUEST_ERROR passes it. Without the option the request carries `interop-denied` and the rows are not scored on that assumption. Do not set it for a publisher that has no such policy |
| `--denied-authorization-token VALUE` | `receive-subscribe-namespace-denied-by-configured-authorization-policy`, `receive-subscribe-tracks-denied-by-configured-authorization-policy` (draft 18) | Same contract as the draft 21 rows above. Token type 0 with this value is sent (`interop-denied` when unset). Only when the option is set does the runner assume the publisher refuses it: REQUEST_ERROR passes and REQUEST_OK fails. Without it the rows are not scored, because a publisher with an open policy legitimately grants the request |
| `--unknown-auth-token-alias-compat-code CODE` | Unknown token-alias probes in both drafts (`D18-10-2-2-MUST-001/002/009` and the draft-21 alias rows such as `D21-8-9-MUST-269`) | The drafts leave the REQUEST_ERROR code for an unknown alias unassigned. Without the option a structurally valid rejection stays `NOT_RUN`. With it, the named code passes and a different code fails; the run records the code, exposes `scoring_profile: "compatibility"` in JSON and labels the HTML report and TAP output. This validates your configured mapping and does not establish a standards assignment |

Token alias probes (REGISTER, DELETE, USE_ALIAS) are only sent when the
publisher's SETUP advertises `MAX_AUTH_TOKEN_CACHE_SIZE` with room for the
20-byte test entry. Compose does not pass these flags by default; see
[building-and-running.md](building-and-running.md).

## Ports and listeners

Each run holds one UDP port from `--publisher-port-start` to
`--publisher-port-end`, and the number of active runs equals the range size.
Scenarios that offer a replacement session hold a second port for the whole run:
`receive-control-goaway-with-new-session-uri` (draft 18) and
`d21-publisher-goaway-alternate-uri` (draft 21). The runner sends a GOAWAY naming
`<scheme>://<host>:<second port>/moq-next` and records what connects there. With a
single-port range the first returns 503 `publisher_ports_exhausted`; give the
range at least two ports. A publisher that does not migrate is not failed
(`D18-10-4-MUST-004`, `D21-9-2-MUST-329`; staying on the first session leaves the
row `NOT_RUN`).

## Transport restrictions

- Native QUIC only: `native-quic-publisher-client-setup-from-moqt-uri`,
  `receive-message-parameter-on-disallowed-message-native-quic` (draft 18);
  `d21-native-publisher-uri-options`, `d21-native-publisher-uri-query`,
  `d21-native-publisher-empty-query`, `d21-native-quic-required-setup-options`,
  `d21-native-quic-datagram-support`,
  `d21-native-quic-without-datagram-negotiation` (draft 21).
- WebTransport only: `receive-webtransport-setup-with-authority`,
  `receive-webtransport-setup-with-path` (draft 18); `d21-webtransport-publisher-setup`,
  `d21-webtransport-required-setup-options`,
  `d21-webtransport-server-sends-authority`, `d21-webtransport-server-sends-path`,
  `d21-webtransport-h3-datagram-support`,
  `d21-webtransport-h3-without-datagram-negotiation` (draft 21).
- Scenarios that compare AUTHORITY and PATH with the URI (the
  `connect-publisher-to-native-uri-with-*` family in draft 18, the native URI
  scenarios in draft 21) leave WebTransport contexts `NOT_RUN`. The URI is known
  to the runner, so these rows score fully only in driven mode, where the adapter
  is handed the URI (`moqt://host:port/moq`, `/moq?run=1`, `/moq?`, or for draft
  18 the query `?interop=1`); in observed mode start the publisher with the URI
  named by the context's `context_ready` event.
- Over WebTransport the runner admits only the strict profile (HTTP/3,
  QUIC/H3 DATAGRAM, RESET_STREAM_AT, current WebTransport settings, extended
  CONNECT, exact `moqt-18`, `moqt-21` or `moqt-22` in `WT-Available-Protocols`). A client
  that sends `Origin` must use an origin listed with `--publisher-origin`;
  clients without `Origin` are accepted unless `--require-publisher-origin` is set.

## Scenario families

The sections below group scenarios by the feature they exercise. "Draft 18" and
"draft 21" scenario IDs differ; draft-21 IDs start with `d21-`. This page lists
drafts 18 and 21 in the families: draft 22 runs use the draft 21 IDs with `d21-` replaced by
`d22-` for the scenarios the drafts share, plus 8 scenarios of draft 22's own; every draft 22
ID is listed in [Draft 22 scenarios](#draft-22-scenarios) (`/healthz` lists them too; see
[http-api.md](http-api.md)).

### Session setup, SETUP options and GREASE

Receiver-error and setup probes send one isolated malformed or unusual stimulus
and score the publisher's reaction. In observed mode they accept a run request
without `track` (for example `receive-forward-outside-zero-one` for draft 18 or
`d21-forward-value-two` for draft 21), but driven mode always needs one. Exact
close codes are checked where the draft prescribes them. Transport closes, local
closes by the runner, partial writes, a missing SETUP, unsupported datagrams and
timeouts leave the row `NOT_RUN`. Datagram probes need an observed negotiated
payload capacity and complete atomic acceptance by the transport. Stored
evidence includes received stream bytes, submitted bytes and accepted lengths,
delivery ordering, and the peer's close code and error space.

- `subscribe-to-publisher-track` (draft 18) also inspects the publisher's SETUP
  option types for `D18-10-3-MUST-NOT-001`: repeated known non-repeatable types
  fail, repeated AUTHORIZATION TOKEN options are allowed, repeated unknown
  extension types are unscored. Over WebTransport it also scores the AUTHORITY
  and PATH prohibitions, closing with INVALID_AUTHORITY or INVALID_PATH if either
  forbidden option arrives. These rows need a completed request and response.
- Unknown and duplicate options: `receive-setup-with-unknown-option` (GREASE
  `0x9D`; requires a typed response instead of a PROTOCOL_VIOLATION close,
  `D18-10-3-MUST-001`), `receive-setup-with-duplicate-unknown-options`,
  `setup-unknown-grease-options-and-duplicates` (GREASE `0x9D` and `0x11C`, then
  SUBSCRIBE_NAMESPACE; a typed reply passes, an application close fails).
  Draft 21: `d21-setup-unknown-options`, `d21-setup-duplicate-unknown-options`
  send `0x9D` once or twice, then require a valid PUBLISH and REQUEST_OK; a close
  or timeout without that exchange stays `NOT_RUN`.
  `d21-publisher-setup-option-multiplicity` / `observe-publisher-setup-options`
  check the publisher's own SETUP for repeated option types.
- Server-sent forbidden options: `d21-server-sends-authority` and
  `d21-server-sends-path` send an otherwise valid server SETUP with one
  role-forbidden option and score `D21-9-1-1-MUST-293` / `D21-9-1-2-MUST-300`
  only after the publisher's application close is seen. Expected codes:
  `INVALID_AUTHORITY` (0x19) and `INVALID_PATH` (0x8); another application close
  fails, an unobserved or transport-level close is `NOT_RUN`, and a completed
  publication without the close fails. Over WebTransport the WebTransport-only
  rows `D21-9-1-1-MUST-294` / `D21-9-1-2-MUST-301` are also scored.
- Token cache in SETUP: an oversized REGISTER is sent against the publisher's
  announced `MAX_AUTH_TOKEN_CACHE_SIZE` or its default of zero; only an
  application close with `AUTH_TOKEN_CACHE_OVERFLOW` (0x13) fails
  `D21-9-1-4-MUST-NOT-307`. The draft-18 token-cache probes send a 64-byte
  REGISTER (cost 80 bytes) and apply only when the publisher's cache is smaller.
- Session lifetime: `complete-publisher-requests-while-session-remains-open`
  finishes a TRACK_STATUS and fails if the publisher FINs or resets its control
  stream (`D18-3-3-MUST-NOT-002`); `d21-control-stream-lifetime`
  (`D21-6-3-MUST-NOT-146`) fails on a FIN or reset of the control stream before
  a response.
- DATAGRAM: `establish-moqt-with-datagram-capable-peer` (`D18-3-1-MUST-001`)
  passes on an established session with datagram capacity; the listener closes
  non-negotiating peers before any MOQT evidence exists. The draft-21 datagram
  scenarios (`D21-6-2-MUST-139`, one per transport) pass on a negotiated
  connection and fail when the runner had to refuse the publisher for lacking it.
  `D21-6-2-MUST-140` (absence of a session) is scored from transport evidence.
- Native URI SETUP: `native-quic-publisher-client-setup-from-moqt-uri`
  (`D18-3-2-MUST-001`) and `d21-native-quic-required-setup-options`
  (`D21-6-3-2-MUST-150`) require AUTHORITY and PATH in a native-QUIC client's
  SETUP.
- Draft 21 reserved extension values: reserved SETUP options (odd, even and
  repeated) and an unknown REQUEST_ERROR code sent to the publisher's own PUBLISH
  must leave the session usable; a close is never scored against the publisher.
  Also an unknown Auth Token Type (`0x9D`) and a STOP_SENDING with unknown
  Stream Reset code `0x9D` against an open Subgroup stream
  (`D21-13-MUST-593/594`): a fresh request answered afterwards proves survival; a
  nonzero application close before that answer fails both rows; a close with
  code 0 proves nothing.

### GOAWAY

- `observe-publisher-client-goaway` checks every GOAWAY the publisher sends for an
  empty New Session URI. `send-new-request-after-publisher-control-goaway` and
  `publisher-control-goaway-with-pending-request-at-cutoff` wait for a control
  GOAWAY from the publisher, then send a request and require REQUEST_ERROR
  `GOING_AWAY` (0x6). These wait for the publisher to act; if it never sends a
  GOAWAY they stay `NOT_RUN`.
- Request GOAWAY probes establish a request, then send two GOAWAY messages on one
  request stream. The draft-21 control sends one on each of two independent
  request streams and requires a typed response on a fresh request; silence alone
  does not prove success. `d21-publisher-client-goaway-control` and `-request`
  (`D21-9-2-MUST-318`) are publisher-initiated.
- The replacement-session scenarios are covered under "Ports and listeners".

### Close probes and the liveness follow-up

Most `receive-*` and `d21-*` probes send input after which the draft says the
publisher MUST close the session (or MUST close with a given code). A publisher
that closes with the required code passes. A publisher that stays silent used to
leave the row `NOT_RUN`, because silence alone does not show that the publisher
saw the input. For the scenarios listed in `src/scenarios/raw_probe_liveness.cpp`
(control-stream GOAWAY, malformed server SETUP including AUTHORITY and PATH,
malformed requests and parameters, the duplicate GOAWAY on one request stream) the
runner now sends one more thing: after the input was fully written and the
publisher's SETUP seen, it waits 500 ms, opens a new request stream and sends a
valid, parameter-free SUBSCRIBE for the `track` fixture (Request ID 7). If the
publisher answers it with a well-formed SUBSCRIBE_OK, and does not close the
session at any point up to 500 ms after that answer, the publisher demonstrably
kept serving instead of closing and the row is `FAIL`.

The follow-up never produces a pass. Any close of the session ends the claim and
the existing close rules apply unchanged. A REQUEST_ERROR, a reset or no answer
leaves the row `NOT_RUN`: a refusal proves the session is open but can be a
conforming reaction (a publisher may refuse new requests after a GOAWAY, or may
not have the track). The follow-up is recorded as a separate
`raw_probe_liveness_followup` event; the stimulus bytes and their proof are
unchanged. Without a `track` the follow-up is not sent.

Not covered, deliberately: probes that send a datagram (a lost datagram is
indistinguishable from an ignored one), rows that only say SHOULD, the duplicate
Request ID rows (the first request may be refused before its ID is recorded),
Subgroup header probes (a publisher with no subscription may not parse data
streams), multi-step probes gated on an earlier response, and probes where the
runner answers a request the publisher opened.

### Namespaces, reserved names and publisher announcements

- `d21-publisher-request-stream-placement` accepts the publisher's PUBLISH for the
  configured track and answers with an empty REQUEST_OK. It is a
  publisher-announcement test, not a relay or a full draft-21 conformance test.
  An invalid first message on a publisher-opened request stream is recorded and
  fails `D21-6-3-MUST-NOT-141`; a permitted but unsupported opener is not a
  failure.
- The runner acknowledges parameter-free PUBLISH_NAMESPACE requests so the
  publisher keeps serving, and rejects the forbidden `.` namespace. It does not
  authorize token-bearing announcements.
- Reserved-namespace attempts (`D21-2-4-2-MUST-NOT-026/028/029/030`,
  `D21-6-5-MUST-NOT-166/167`): a forbidden PUBLISH or PUBLISH_NAMESPACE fails the
  row. A pass requires both SETUPs exchanged and the session still alive when the
  whole timeout elapsed without such a publication; a publisher that never
  connects or closes early is `NOT_RUN`.
- Draft 18: `publish-track-under-single-period-namespace`,
  `application-publish-track-in-session-namespace`,
  `publish-distinct-content-tracks-in-same-scope` and the namespace variants
  observe a publisher-originated PUBLISH; the publisher must be made to publish.
  `D18-2-4-1-MUST-001` and `D18-10-MUST-002` (non-empty namespace fields and
  first-message placement) are checked after SUBSCRIBE_TRACKS sent with FORWARD 0.
  `D18-10-MUST-005` and `D18-9-5-MUST-003` observe PUBLISH_NAMESPACE placement
  and explicitness passively.
- Draft 21: `d21-publisher-emitted-namespace-fields` (`D21-8-7-MUST-250`) fails on
  an empty field; `d21-publisher-key-value-type-deltas` (`D21-8-3-MUST-NOT-230`)
  fails when the publisher's SETUP, PUBLISH parameters or Track Properties
  overflow the type space; both pass only with a decoded PUBLISH.
  `d21-publisher-namespace-routing-announcement` (`D21-7-5-MUST-206`) passes on
  an explicit PUBLISH_NAMESPACE for the configured namespace and is never a
  failure. `d21-publish-distinct-tracks-in-one-scope` (`D21-2-5-MUST-032`) needs
  two different tracks published and compares Object payloads at a shared
  Location. `d21-concurrent-distinct-track-subscriptions` needs two tracks.
- Publisher-initiated scenarios send nothing and answer what the publisher opens:
  `d21-reject-publish-before-object-production`, `d21-rejected-subscribe-no-delivery`
  (`D21-3-1-1-MUST-NOT-047`, only streams started after the publisher visibly
  reacted to the rejection count), `d21-publisher-update-credit-limit`,
  `-per-stream` and `d21-publisher-update-zero-unlimited` (`D21-9-1-7-MUST-NOT-316`;
  the runner announces `MAX_REQUEST_UPDATES` of 2 and leaves updates
  unanswered), and `d21-publisher-delete-with-pending-alias-uses`
  (`D21-8-9-MUST-NOT-281`). A publisher that gives up after the runner rejects or
  withholds an answer does not void the context.
- `publisher-queries-track-status-before-resuming-publication` passively requires
  every publisher TRACK_STATUS to be the first message of a new bidirectional
  stream (`D18-10-MUST-004`); a publisher that never queries stays `NOT_RUN`.

### Subscriptions, updates and response cardinality

- `subscribe-to-publisher-track` (draft 18) subscribes to the configured track and
  checks the single-response rule. `subscribe-again-to-established-publisher-track`
  waits for SUBSCRIBE_OK, sends a second SUBSCRIBE for the same track and scores
  `D18-5-1-MUST-004` from DUPLICATE_SUBSCRIPTION; if the first subscription is
  not established the row stays `NOT_RUN`. A SUBSCRIBE crossing a still-pending
  PUBLISH is rejected with DUPLICATE_SUBSCRIPTION (`D18-5-1-MUST-005`).
- A SUBSCRIBE must be answered with SUBSCRIBE_OK (a refusal says nothing); a
  forwarded subscription must deliver an Object on its alias. REQUEST_UPDATE gets
  exactly one reply (a quiet window of a quarter of `timeout_ms`, at most 50 ms,
  follows the first); three coalesced updates get three REQUEST_OKs unless one
  REQUEST_ERROR answers the batch.
- Draft-21 REQUEST_UPDATE accounting: single, pipelined successful and pipelined
  failing updates are counted on the request stream and fenced by a later
  TRACK_STATUS request. More responses than updates fails; fewer stays `NOT_RUN`.
  `MAX_REQUEST_UPDATES` contexts adapt to the announced limit (zero or omitted
  means unlimited); only the mandated `TOO_MANY_REQUEST_UPDATES` (0x1B) close
  proves the over-limit rule. Update probes wait for a fully decoded successful
  response before reusing the request stream; the duplicate update ID probe waits
  for the first acknowledgment before repeating its ID. Failed-update probes
  require a complete REQUEST_ERROR before checking `PUBLISH_DONE` with
  `UPDATE_FAILED` or discovery request closure, and verify local FIN and peer FIN
  or RESET on the same stream.
- Draft-21 response-count profiles (`d21-subscribe-accepted`/`-rejected`,
  `d21-fetch-accepted`/`-rejected`, `d21-subscribe-namespace-*`,
  `d21-subscribe-tracks-*`) need both contexts in the run to pass a row, and a
  valid typed reply on the actual request stream collected through peer FIN.
  Two replies fail immediately; missing FIN or reset-only closure is `NOT_RUN`.
  Discovery additionally requires REQUEST_OK or REQUEST_ERROR as the first
  message on the response stream; later namespace notifications are not extra
  replies. Redirect errors for discovery require an empty Track Name
  (`D21-9-4-1-MUST-340`; publishers that never redirect leave it `NOT_RUN`).
  Responses must be `SUBSCRIBE_OK` for an accepted SUBSCRIBE; a FETCH start far
  beyond the Largest Object requires `INVALID_RANGE`.
- Draft-21 LARGEST_OBJECT (`D21-9-20-18-MUST-456`,
  `d21-largest-object-required-after-publication` / `-before-publication`): after
  an Object's first byte is observed, a second SUBSCRIBE, a REQUEST_UPDATE and a
  TRACK_STATUS must carry LARGEST_OBJECT. The "before" context is a control.
  `D18-10-2-11-MUST-001..004` check LARGEST_OBJECT in draft 18 once a FETCH
  delivered Group 7/Object 9.
- PUBLISH_DONE Stream Count must be 0 when no data stream was opened
  (`D21-9-9-MUST-365`, two scenarios that are alternative routes to the
  precondition; draft 18 has `publisher-ends-subscription-with-no-data-streams`
  and a Forward State 0 variant).
- Notifications (draft 21): PUBLISH_STATE_NOTIFY probes establish a namespace or
  FETCH request, then send the notification on that stream and check for a
  PROTOCOL_VIOLATION close; FETCH probes need a track. Subscriber-direction
  probes cover SUBSCRIBE- and PUBLISH-established subscriptions and need both
  contexts. `D21-9-10-MUST-NOT-371` and `D21-9-10-MUST-372` are publisher-initiated: a notification after
  a known Object must carry LARGEST_OBJECT; subscriber-controlled values may differ
  from the last request only after an acknowledged REQUEST_UPDATE. No
  notification means `NOT_RUN`.
- Message Parameters in publisher-originated messages are walked with the
  type-delta rules: an overflowing delta fails ordering, an undefined type fails
  negotiation, a repeated type outside tokens and range filters fails
  multiplicity. Draft 18 parameter-block probes pass only when the publisher's
  TRACK_STATUS_OK or PUBLISH carries several parameters
  (`D18-10-2-MUST-001`, `D18-10-2-MUST-NOT-001`);
  `publish-with-and-without-parameter-extension-negotiation` fails on a message
  parameter the draft does not define (`D18-10-2-MUST-003`) and needs at least
  one parameter sent for a pass.

### FETCH

- `fetch-publisher-track-range` (draft 18) sends a standalone FETCH for Location
  `{0,0}` to `{0,1}` and scores `D18-5-2-MUST-001` when exactly one FETCH_OK or
  REQUEST_ERROR is observed. The response code, range validity and Object
  delivery are not scored.
- First-object profiles (both drafts) need the Group 7, Object 9 fixture. A typed
  FETCH_OK and one associated FETCH stream prove the first ordinary Object.
  Missing Group or Object ID flags fail only that requirement; complete typed data
  at 7/9 passes; empty responses, range markers, other Locations and incomplete
  evidence stay `NOT_RUN`. Object bytes may arrive before FETCH_OK.
- Group-order profiles decode complete Objects on each FETCH stream. Draft 18
  executes both explicit orders in one scenario; draft 21 uses ascending,
  descending and default-ascending scenarios. A pass needs a typed FETCH_OK, at
  least two distinct groups and a complete stream.
- Draft-18 Object comparison: two FETCHes of Group 7/Object 9 compare payload bytes
  (`D18-2-1-MUST-NOT-001`); `retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters`
  also subscribes with different priority and group order (`D18-10-2-MUST-NOT-002`).
  In draft 21, two subscriptions or FETCHes that differ only in delivery
  parameters must carry identical payloads.
- Joining FETCH of a Forward State 0 subscription, and FETCH on an empty track or
  beyond the Largest Object, require `INVALID_RANGE`. The empty-track rows apply
  only when SUBSCRIBE_OK has no LARGEST_OBJECT. A `DOES_NOT_EXIST` reply to a
  FETCH of a track that was just subscribed fails; other error codes are
  inconclusive. A joining FETCH after a Forward 0 to 1 update must end at the
  REQUEST_UPDATE_OK Largest Object once TRACK_STATUS shows the track advanced
  (`D18-5-1-MUST-003`); the subscription uses a far-future AbsoluteStart so no
  Object bytes accumulate.
- Draft-21 FETCH response-count profiles require a valid typed reply and
  collection through peer FIN, as for subscriptions above.

### Scenarios that need FETCH

The drafts let an endpoint that is not a relay implement a subset of MOQT
(draft 18 Section 4, draft 21 Section 1.5), so a live publisher with no cache may
have no FETCH. A run can declare that (`"publisher_capabilities": {"fetch": false}`
or the runner flag `--publisher-no-fetch`; see [http-api.md](http-api.md#declaring-publisher-capabilities)).
Every executable scenario carries a `requires_fetch` flag, shown in the `/healthz`
`executable_profiles`. It is set for exactly the scenarios whose stimulus sends a
FETCH message (message type 0x16 in both drafts) or retrieves Objects through one.
A unit test decodes the stimulus of every executable scenario with the runner's own
wire code and fails when the flag and the decoded stimulus disagree. Two scenarios
build their FETCH at run time from the publisher's own reply
(`receive-fetch-start-beyond-largest-published-object` and
`fetch-object-previously-observed-as-datagram`); every scenario with such a
run-time write is pinned in the test with whether it is a FETCH, and the typed
`fetch-publisher-track-range` is read from its step actions.

`requires_fetch` scenarios, 23 in draft 18:

- `fetch-publisher-track-range`
- `receive-fetch-with-unknown-type`
- `receive-joining-fetch-with-unrelated-or-wrong-state-request-id`
- `cancel-fetch-request-with-open-data-stream`
- `reject-request-update-for-open-fetch`
- `fetch-known-first-object-with-nonzero-group-and-object-ids`
- `fetch-multiple-published-groups-in-each-explicit-order`
- `publish-and-retrieve-same-object-and-track-immutable-properties`
- `repeat-immutable-property-with-alternative-varint-encodings-available`
- `publish-object-with-immutable-properties`
- `receive-joining-fetch-for-forward-zero-subscription`
- `receive-forward-state-update-then-joining-fetch`
- `receive-joining-fetch-for-track-with-no-published-objects`
- `receive-standalone-fetch-for-track-with-no-published-objects`
- `receive-fetch-start-beyond-largest-published-object`
- `fetch-object-previously-observed-as-datagram`
- `retrieve-same-object-at-distinct-times`
- `subscribe-to-track-after-observed-object-publication`
- `publish-existing-track-after-observed-object-publication`
- `accepted-subscription-update-after-observed-object-publication`
- `accepted-track-status-after-observed-object-publication`
- `joining-fetch-after-forward-enabled-and-track-advanced`
- `retrieve-same-object-with-different-subscribe-publish-ok-and-fetch-parameters`

and 22 in draft 21:

- `d21-cancel-fetch-with-open-request-and-data-streams`
- `d21-failed-fetch-update-data-reset`
- `d21-fetch-accepted`
- `d21-fetch-rejected`
- `d21-fetch-first-object-flags`
- `d21-fetch-ascending-groups`
- `d21-fetch-descending-groups`
- `d21-fetch-default-group-order`
- `d21-publish-state-notify-on-fetch`
- `d21-immutable-property-repeat`
- `d21-repeat-object-retrieval`
- `d21-object-immutable-property-singleton`
- `d21-request-stream-terminal-message-order`
- `d21-fetch-start-beyond-largest-object`
- `d21-fetch-track-with-no-published-objects`
- `d21-fetch-parameters-preserve-payload`
- `d21-prior-group-gap-repeat`
- `d21-prior-group-gap-singleton`
- `d21-prior-object-gap-repeat`
- `d21-prior-object-gap-singleton`
- `d21-subscription-forwarding-preference`
- `d21-fetch-datagram-preference`

and 23 in draft 22: the 22 above with `d21-` replaced by `d22-`, and the own
`d22-fetch-bounded-location-range`.

What a declaration of no FETCH changes:

- Selecting only such scenarios is refused with 422
  `scenario_requires_publisher_capability` before any listener or publisher process
  exists.
- In a run that also selects other scenarios, the FETCH ones are skipped. They are
  never given a listener context or a publisher process; the run records one
  `context_skipped` event per skipped scenario with the detail
  `publisher declared no FETCH support`. The selection stays as requested.
- A catalog row is reported `not_applicable` for the run when it names at least one
  scenario and **every** scenario it names requires FETCH. The row leaves the
  required, weighted and coverage denominators and the export gives its reason. A
  row that names any scenario that does not need FETCH is never dropped: it keeps
  the rule that every named scenario must run, so it stays `not_run` when a FETCH
  scenario it names was skipped (draft 21 `D21-9-10-MUST-369`, which names
  `d21-publish-state-notify-on-namespace-request` and
  `d21-publish-state-notify-on-fetch`, is the only such row today; draft 18 has
  none). With the current catalogs 27 draft 18 rows and 25 draft 21 rows are
  `not_applicable` under a no-FETCH declaration.
- Skipped scenarios and `not_applicable` rows never make a run `error` or `fail`.
  A run that is otherwise clean is `incomplete` or `pass` by the usual rules. In the
  TAP export a skipped scenario is `ok N - id # SKIP publisher declared no FETCH
  support`; the HTML report lists them in a "Publisher capabilities" section and
  prints each row's reason in text.

A publisher that omits FETCH is not obliged to stay silent: both drafts say a limited
endpoint SHOULD answer an unsupported message with NOT_SUPPORTED instead of ignoring
it (draft 18 Section 4, draft 21 Section 1.5). A scenario that sends a FETCH to a
declared-no-FETCH publisher and requires NOT_SUPPORTED (an optional row) is possible
future work; it does not exist today, and the runner does not send FETCH to a
publisher that declared it away.

Fill (draft 21 Section 3.4) is not FETCH: a publisher that backfills a subscription
opens FETCH-header streams on its own, but the runner sends it no FETCH message, so
the `d21-fill-*` scenarios are not tagged.

### Discovery (SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS)

- `subscribe-namespace-at-publisher` and `subscribe-tracks-at-publisher` (draft 18)
  send the request with the configured namespace as prefix and score
  `D18-6-1-MUST-001` / `D18-6-1-MUST-003` when exactly one REQUEST_OK or
  REQUEST_ERROR is observed; a short observation window detects duplicates.
  Response ordering (`D18-6-1-MUST-002`) and updates are separate rows.
- Overlap profiles (both drafts) establish active discovery subscriptions, then test
  exact, ancestor and descendant prefixes. Prefix updates establish A and a
  disjoint B, then update B to A on B's request stream with a fresh Request ID.
  A complete REQUEST_ERROR with PREFIX_OVERLAP (0x30) passes; a typed wrong error
  or OK fails; missing establishment or incomplete evidence is `NOT_RUN`. Draft 21
  also runs both request types together. Draft-18 NAMESPACE ordering
  (`D18-6-2-MUST-001`) finishes the first stream before the second prefix to avoid
  PREFIX_OVERLAP.
- Draft 21 `d21-discover-original-publisher-namespaces` (`D21-4-2-MUST-089`) uses an
  empty-prefix SUBSCRIBE_NAMESPACE. A REDIRECT reply to SUBSCRIBE_NAMESPACE must
  leave the Track Name empty; NAMESPACE_DONE must follow its NAMESPACE.
- Stream credit: `subscribe-tracks-with-no-bidirectional-stream-credit` grants the
  publisher a single bidirectional stream (spent on its PUBLISH_NAMESPACE, which
  the runner answers) and requires a REQUEST_OK or REQUEST_ERROR first on the
  SUBSCRIBE_TRACKS stream (`D18-6-1-MUST-004`);
  `restore-bidi-stream-credit-after-publish-blocked` then sends MAX_STREAMS and
  fails if the publisher opens PUBLISH for the blocked Track
  (`D18-6-1-MUST-NOT-001`). Draft 21
  `d21-subscribe-tracks-publish-skipped-then-capacity-recovers`
  (`D21-4-1-MUST-NOT-084`) allows one publisher-opened request stream, waits for
  PUBLISH_SKIPPED, rejects the PUBLISH and fails if a skipped track is published
  afterwards.
- `D21-9-20-3-MUST-NOT-407`: SUBSCRIBE_TRACKS carries a distinctive token; the
  resulting PUBLISH must not carry the same credential by value or through an
  alias the publisher registered.

### Objects, Subgroups and datagrams

- Delivery rules are scored from the Objects the publisher actually produces:
  Datagram and Subgroup header bits, Subgroup FIN after End of Group, Forward
  State 0 delivering no Objects until a REQUEST_UPDATE sets Forward 1, gap
  Properties counted in the mutable list and inside Immutable Properties, and
  non-normal status Objects having no payload. A Subgroup that delivered End of
  Group or End of Track must close with FIN. A Subgroup closed with FIN that
  later continues on another stream fails.
- NextGroupStart subscriptions check the FIRST_OBJECT bit and Subgroup stream
  uniqueness after a Group rollover (`D18-2-2-MUST-001`,
  `D18-2-2-MUST-NOT-002`). Draft 21: `d21-original-publisher-opens-new-subgroup`
  (`D21-2-2-MUST-020`) subscribes from Group 7, Object 0 and requires FIRST_OBJECT
  on the first stream of each Subgroup of Group 7;
  `d21-subscribe-single-subgroup` and `d21-subgroup-restart-after-reset`
  (`D21-2-2-MUST-NOT-018`) need an observed reset and are `NOT_RUN` for publishers
  that finish the stream first.
- `d21-publish-track-with-mandatory-property` (`D21-3-6-MUST-070`) fails when an
  Object carries a property in 0x4000-0x7FFF.
- A SUBSCRIBE for an absent track must deliver no Objects
  (`D18-5-1-1-MUST-NOT-002`); an AbsoluteRange for the Group after the
  TRACK_STATUS Largest Object must deliver nothing outside it
  (`D18-5-1-2-MUST-NOT-001`). `d21-subscribe-bounded-location-range` and
  `d21-update-subscription-location-range` (`D21-3-3-1-MUST-NOT-057`) observe the
  whole timeout; the update scenario subscribes with FORWARD=0 and sets FORWARD=1
  and the Group 7, Object 9 filter in one acknowledged REQUEST_UPDATE. Both
  contexts must run in one run.
- Alias sharing is the publisher's choice: `d21-overlapping-subscriptions-shared-alias`
  / `-distinct-aliases` (`D21-3-1-MUST-041`) score whichever context's alias
  assignment occurred, and pass on either.
- Padding: `d21-padding-*-emission` and `observe-publisher-padding-stream` /
  `-datagram` observe padding the publisher emits (`D21-11-5-1-MUST-565`,
  `D21-11-5-2-MUST-570`: all zero bytes after the type). For padding sent to the
  publisher, a 128 KiB padding stream and a padding datagram are sent before an
  ordinary SUBSCRIBE; a publisher that stops draining the stream stalls the probe
  and stays `NOT_RUN`.
- `fetch-object-previously-observed-as-datagram` fetches an Object first seen as a
  datagram and requires Serialization Flags bit 0x40;
  `redeliver-previously-observed-object-in-later-subscription` compares
  Forwarding Preferences.

### Filters and fill (draft 21)

- Location filter and range filter: `d21-forward-location-and-range-filter-conjunction`
  (`D21-3-3-3-MUST-066`) requires a `FORWARD=0` subscription to stay silent and a
  `FORWARD=1` subscription with Location filter {0,0}-{1,0} (and an
  `OBJECTID_FILTER` for Object 1 when `MAX_FILTER_RANGES` is advertised) to
  deliver only Objects passing all.
- `MAX_FILTER_RANGES` limit probes use the publisher's advertised value: exactly
  one Range over the limit across distinct filter keys, or one Range when the
  limit defaults to zero; capacities too large for a bounded request are
  `NOT_RUN`. A REQUEST_UPDATE adding SetID 1 while keeping SetID 0 filters
  exceeds the concurrent cap even though the update adds one range. Duplicate-key
  update probes wait for a valid SUBSCRIBE_OK. `D21-10-7-MUST-489/490` need
  `MAX_FILTER_RANGES` and an even-typed integer Property that appears only in the
  mutable list (or only inside Immutable Properties); a second subscription
  filters on its value.
- Fill: `d21-fill-fails-before-first-object` (`D21-3-4-1-MUST-068/069`) and
  `d21-cancel-subscription-with-concurrent-fill-streams` (`-067`) make the track
  live with a plain subscription, then send `FILL_PARAMETERS`. A fill failure
  scores only when the fill stream's `FETCH_HEADER` reached the runner and was
  followed by a reset with no Object; a stack that resets before the header proves
  neither row.
- Publisher response to an Object-delivery limit: `d21-subgroup-completion-withheld-acknowledgments`
  (`D21-5-2-MUST-130`) asks for a 200 ms `SUBGROUP_DELIVERY_TIMEOUT` and holds each
  data stream at 64 bytes of credit; the publisher must reset the stream when the
  timer expires.

### Cancellation, resets and cleanup

- FETCH cleanup probes need an actual open FETCH data stream. Cancellation sends
  FIN on the request's sending direction before STOP_SENDING on its receiving
  direction; request and data resets are scored independently. A failed update
  requires a valid REQUEST_ERROR and a reset of its data stream. FIN-only results
  stay `NOT_RUN` because transport timing cannot establish a missing reset.
- SUBSCRIBE cancellation waits for a valid SUBSCRIBE_OK and at least two open
  Subgroup streams before STOP_SENDING. Passing requires resets of the request
  stream and all observed open streams, including reordered late headers.
  Ambiguous FIN or alias ownership is `NOT_RUN`.
- Draft 18: cancelling the subscription, moving the Start Location past an open
  Subgroup, or setting Forward State 0 must reset a Subgroup that was open and
  incomplete when the trigger was sent (a FIN is `NOT_RUN`).
  Draft 21 `D21-11-3-2-MUST-543` sends Forward State 0 while a Subgroup is open,
  then restores it; a reset passes, a FIN fails only when the same Subgroup then
  continues on another stream.
- Delivery timeouts: `subgroup-object-expires-before-transport-handoff` (`D18-8-MUST-003`)
  leaves the publisher without a unidirectional stream for 400 ms against an
  OBJECT_DELIVERY_TIMEOUT of 100 ms and passes only on a reset with
  DELIVERY_TIMEOUT; `withhold-subgroup-acknowledgements-after-application-completion`
  (`D18-8-MUST-006`) discards inbound packets for 400 ms against a
  SUBGROUP_DELIVERY_TIMEOUT of 100 ms. Neither can score a failure, because the
  age of an Object at hand-off is internal to the publisher. Over WebTransport a
  reset whose code is not a WebTransport application error code is reported as
  unavailable.
- Request stream end: `d21-publisher-request-response-before-fin` (`D21-6-4-2-2-MUST-157`),
  `d21-established-subscription-publisher-fin` (`-158`) and
  `d21-request-stream-terminal-message-order` (`-NOT-156`). The last two send a
  REQUEST_UPDATE with an unregistered token Alias, which fails and obliges the
  publisher to end the subscription; PUBLISH_DONE must precede the FIN. No FIN
  means `NOT_RUN`.

### Authorization tokens

Alias probes send REGISTER only when the publisher's SETUP advertises
`MAX_AUTH_TOKEN_CACHE_SIZE` for the test entry, then DELETE and USE_ALIAS on later
TRACK_STATUS requests after each response. Alias state is inferred by comparing
responses with a control request naming a never-registered Alias, because
`UNKNOWN_AUTH_TOKEN_ALIAS` has no REQUEST_ERROR code in either draft; the
compatibility flag above makes it exact. Draft 18: `D18-10-2-2-MUST-001/002/009`,
`receive-delete-for-unregistered-token`, `receive-use-alias-for-unregistered-token`,
`register-same-peer-token-alias-twice-without-delete`;
`withhold-use-alias-response-while-publisher-retires-token` answers only the
publisher's registering request and fails if a DELETE arrives while a USE_ALIAS of
that alias is unanswered (`D18-10-2-2-MUST-NOT-002`). Draft 21: `d21-token-delete-and-reuse`
(`D21-8-9-MUST-264`), `d21-token-register-alias-lifetime` (`-265`),
`d21-request-deleted-token-alias` (`-269`, needs the compatibility code), the two
`d21-register-token-on-*` scenarios (`-271`) and `d21-setup-register-use-value-fallback`
(`D21-9-1-4-MUST-308`). `D21-8-9-MUST-270/273` need the credential flags above.
Token-bearing announcements are not authorized by the runner.

### Publisher-initiated recovery flows (draft 18)

These wait for the publisher's own request and stay `NOT_RUN` if it never
happens. A recovery TRACK_STATUS answered with unknown optional Properties
(ascending types `0x00`, `0x01`, known `0x22` = 1, `0x9D`, `0x11C`) must leave the
session usable; the same reply with invalid `0x22` = 0 after the unknown types must
close with PROTOCOL_VIOLATION (any other close code means the unknown Properties
were not skipped). A PUBLISH or PUBLISH_NAMESPACE rejected with unknown
REQUEST_ERROR `0x9D`, and a request stream stopped with unknown code `0x9D`, must
not close the session; a follow-up SUBSCRIBE_NAMESPACE with a typed reply is the
survival proof. `publish-two-simultaneous-tracks` accepts one PUBLISH and compares
its Track Alias with the fixture track's SUBSCRIBE_OK alias.

## Draft 22 scenarios

Counts: 221 executable draft 22 scenarios (213 shared with draft 21, of the 307 shared scenario IDs, and 8 of draft 22's own) and 2 unscored probes.

A shared scenario is named by its draft 21 ID with `d21-` replaced by `d22-`. The runner executes it with
the draft 21 implementation on the draft 22 wire, so its stimulus, fixture contract, flags and `NOT_RUN`
reasons are those of the draft 21 scenario described in the families above; it scores the draft 22
rows listed here (the lineage maps each draft 21 row to its draft 22 row). The lineage pairs
307 shared scenario IDs, but only the 213 whose draft 21 implementation is
executable run; a run naming one of the others is refused with HTTP 422. A few shared scenarios
send different bytes on the draft 22 wire, noted in the Behavior column; their draft 21 twins are
unchanged. Own scenarios have no draft 21 twin; they exercise what draft 22 changed (LOCATION_FILTER
and the request stream before SETUP). The list is checked against the registry by
`tests/unit/scenario_reference_doc_test.cpp`. For the moqxr adapter's options per scenario see
[the harness guide](publisher-harness-guide.md).

### Executable draft 22 scenarios

| Draft 22 ID | Kind | Draft 21 implementation | Draft 22 rows | Behavior |
|---|---|---|---|---|
| `d22-application-namespace-publication-under-session` | shared | `d21-application-namespace-publication-under-session` | `D22-6-5-MUST-NOT-181` | As `d21-application-namespace-publication-under-session` on the draft 22 wire |
| `d22-application-track-publication-under-session` | shared | `d21-application-track-publication-under-session` | `D22-6-5-MUST-NOT-180` | As `d21-application-track-publication-under-session` on the draft 22 wire |
| `d22-attempt-single-period-namespace-publication` | shared | `d21-attempt-single-period-namespace-publication` | `D22-2-4-3-MUST-NOT-032` | As `d21-attempt-single-period-namespace-publication` on the draft 22 wire |
| `d22-attempt-single-period-namespace-use` | shared | `d21-attempt-single-period-namespace-use` | `D22-2-4-3-MUST-NOT-030` | As `d21-attempt-single-period-namespace-use` on the draft 22 wire |
| `d22-attempt-single-period-track-publication` | shared | `d21-attempt-single-period-track-publication` | `D22-2-4-3-MUST-NOT-031` | As `d21-attempt-single-period-track-publication` on the draft 22 wire |
| `d22-attempt-unregistered-period-namespace-publication` | shared | `d21-attempt-unregistered-period-namespace-publication` | `D22-2-4-3-MUST-NOT-028` | As `d21-attempt-unregistered-period-namespace-publication`; see [Track fixture contracts](#track-fixture-contracts) |
| `d22-cancel-fetch-with-open-request-and-data-streams` | shared | `d21-cancel-fetch-with-open-request-and-data-streams` | `D22-3-2-4-MUST-067`, `D22-3-2-4-MUST-068` | As `d21-cancel-fetch-with-open-request-and-data-streams`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-cancel-subscribe-with-open-streams` | shared | `d21-cancel-subscribe-with-open-streams` | `D22-3-1-2-MUST-047` | As `d21-cancel-subscribe-with-open-streams` on the draft 22 wire |
| `d22-cancel-subscription-with-concurrent-fill-streams` | shared | `d21-cancel-subscription-with-concurrent-fill-streams` | `D22-3-4-1-MUST-079` | As `d21-cancel-subscription-with-concurrent-fill-streams`; see [Filters and fill (draft 21)](#filters-and-fill-draft-21) |
| `d22-coalesced-failed-update-response` | shared | `d21-coalesced-failed-update-response` | `D22-9-5-MUST-356` | As `d21-coalesced-failed-update-response` on the draft 22 wire |
| `d22-coalesced-successful-update-responses` | shared | `d21-coalesced-successful-update-responses` | `D22-9-5-MUST-356`, `D22-9-5-1-MUST-363` | As `d21-coalesced-successful-update-responses` on the draft 22 wire |
| `d22-complete-subgroup-fin` | shared | `d21-complete-subgroup-fin` | `D22-11-3-2-MUST-512` | As `d21-complete-subgroup-fin` on the draft 22 wire |
| `d22-concurrent-distinct-track-subscriptions` | shared | `d21-concurrent-distinct-track-subscriptions` | `D22-3-1-3-MUST-NOT-050` | As `d21-concurrent-distinct-track-subscriptions`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-control-stream-lifetime` | shared | `d21-control-stream-lifetime` | `D22-6-3-MUST-NOT-160` | As `d21-control-stream-lifetime`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-discover-original-publisher-namespaces` | own | none | `D22-4-2-MUST-110` | SUBSCRIBE (FORWARD=0) for the track, then SUBSCRIBE_NAMESPACE with the empty prefix (a NAMESPACE for the track's namespace is required before the response FIN), then a nonmatching prefix |
| `d22-discovery-independent-overlap-spaces` | shared | `d21-discovery-independent-overlap-spaces` | `D22-3-6-MUST-083`, `D22-4-2-MUST-111` | As `d21-discovery-independent-overlap-spaces` on the draft 22 wire. On draft 22 the publisher's PUBLISH is accepted with REQUEST_OK (a courtesy write, not stimulus) |
| `d22-discovery-update-independent-overlap-spaces` | shared | `d21-discovery-update-independent-overlap-spaces` | `D22-9-20-20-MUST-455`, `D22-9-20-20-MUST-456` | As `d21-discovery-update-independent-overlap-spaces` on the draft 22 wire. On draft 22 the publisher's PUBLISH is accepted with REQUEST_OK (a courtesy write, not stimulus) |
| `d22-discovery-update-invalid-forward` | shared | `d21-discovery-update-invalid-forward` | `D22-9-20-18-MUST-445` | As `d21-discovery-update-invalid-forward` on the draft 22 wire |
| `d22-duplicate-control-goaway` | shared | `d21-duplicate-control-goaway` | `D22-9-2-MUST-338` | As `d21-duplicate-control-goaway` on the draft 22 wire |
| `d22-duplicate-range-filter-key-in-request` | shared | `d21-duplicate-range-filter-key-in-request` | `D22-3-3-2-MUST-076` | As `d21-duplicate-range-filter-key-in-request` on the draft 22 wire |
| `d22-duplicate-range-filter-key-in-update` | shared | `d21-duplicate-range-filter-key-in-update` | `D22-3-3-2-MUST-076` | As `d21-duplicate-range-filter-key-in-update` on the draft 22 wire |
| `d22-duplicate-request-goaway` | shared | `d21-duplicate-request-goaway` | `D22-9-2-MUST-339` | As `d21-duplicate-request-goaway` on the draft 22 wire. On draft 22 the SUBSCRIBE_NAMESPACE uses the run's namespace (draft 21: prefix "a") |
| `d22-duplicate-request-id-across-streams` | shared | `d21-duplicate-request-id-across-streams` | `D22-6-4-2-1-MUST-169` | As `d21-duplicate-request-id-across-streams` on the draft 22 wire |
| `d22-duplicate-request-update-id` | shared | `d21-duplicate-request-update-id` | `D22-6-4-2-1-MUST-169` | As `d21-duplicate-request-update-id` on the draft 22 wire |
| `d22-established-subscription-publisher-fin` | shared | `d21-established-subscription-publisher-fin` | `D22-6-4-2-2-MUST-172` | As `d21-established-subscription-publisher-fin`; see [Cancellation, resets and cleanup](#cancellation-resets-and-cleanup) |
| `d22-expired-token-alias-lifetime` | shared | `d21-expired-token-alias-lifetime` | `D22-8-9-MUST-285` | As `d21-expired-token-alias-lifetime`; see [Operator-supplied credentials and policy](#operator-supplied-credentials-and-policy) |
| `d22-failed-fetch-update-data-reset` | shared | `d21-failed-fetch-update-data-reset` | `D22-9-5-1-MUST-358` | As `d21-failed-fetch-update-data-reset`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-failed-subscribe-namespace-update-close` | shared | `d21-failed-subscribe-namespace-update-close` | `D22-9-5-1-MUST-359` | As `d21-failed-subscribe-namespace-update-close` on the draft 22 wire |
| `d22-failed-subscribe-tracks-update-close` | shared | `d21-failed-subscribe-tracks-update-close` | `D22-9-5-1-MUST-360` | As `d21-failed-subscribe-tracks-update-close` on the draft 22 wire. On draft 22 the publisher's PUBLISH is accepted with REQUEST_OK (a courtesy write, not stimulus) |
| `d22-failed-subscription-update-cleanup` | shared | `d21-failed-subscription-update-cleanup` | `D22-9-5-1-MUST-357` | As `d21-failed-subscription-update-cleanup` on the draft 22 wire. On draft 22 the SUBSCRIBE names the run's track (draft 21 sends namespace () and track "x") |
| `d22-fetch-accepted` | shared | `d21-fetch-accepted` | `D22-3-2-MUST-057` | As `d21-fetch-accepted`; see [Subscriptions, updates and response cardinality](#subscriptions-updates-and-response-cardinality) |
| `d22-fetch-ascending-groups` | shared | `d21-fetch-ascending-groups` | `D22-3-2-1-MUST-062` | As `d21-fetch-ascending-groups`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-bounded-location-range` | own | none | `D22-3-3-1-MUST-NOT-069` | FETCH whose range is a LOCATION_FILTER; fails an Object outside it. Needs FETCH (skipped with a no-FETCH declaration) |
| `d22-fetch-datagram-preference` | shared | `d21-fetch-datagram-preference` | `D22-11-4-1-1-MUST-533`, `D22-11-4-1-1-SHOULD-534` | As `d21-fetch-datagram-preference`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-default-group-order` | shared | `d21-fetch-default-group-order` | `D22-3-2-1-MUST-062` | As `d21-fetch-default-group-order`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-descending-groups` | shared | `d21-fetch-descending-groups` | `D22-3-2-1-MUST-062` | As `d21-fetch-descending-groups`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-first-object-flags` | shared | `d21-fetch-first-object-flags` | `D22-11-4-1-1-MUST-527`, `D22-11-4-1-1-MUST-528` | As `d21-fetch-first-object-flags`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-parameters-preserve-payload` | shared | `d21-fetch-parameters-preserve-payload` | `D22-9-20-MUST-NOT-395` | As `d21-fetch-parameters-preserve-payload`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-rejected` | shared | `d21-fetch-rejected` | `D22-3-2-MUST-057` | As `d21-fetch-rejected`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-start-beyond-largest-object` | shared | `d21-fetch-start-beyond-largest-object` | `D22-3-2-MUST-061` | As `d21-fetch-start-beyond-largest-object`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fetch-track-with-no-published-objects` | shared | `d21-fetch-track-with-no-published-objects` | `D22-3-2-MUST-060` | As `d21-fetch-track-with-no-published-objects`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-fill-fails-before-first-object` | shared | `d21-fill-fails-before-first-object` | `D22-3-4-1-MUST-080`, `D22-3-4-1-MUST-081` | As `d21-fill-fails-before-first-object`; see [Filters and fill (draft 21)](#filters-and-fill-draft-21) |
| `d22-fill-forbidden-nested-authorization` | shared | `d21-fill-forbidden-nested-authorization` | `D22-9-20-15-MUST-432` | As `d21-fill-forbidden-nested-authorization` on the draft 22 wire |
| `d22-fill-forbidden-track-property-filter` | shared | `d21-fill-forbidden-track-property-filter` | `D22-9-20-15-MUST-432` | As `d21-fill-forbidden-track-property-filter` on the draft 22 wire |
| `d22-fill-invalid-group-order` | shared | `d21-fill-invalid-group-order` | `D22-9-20-8-MUST-421` | As `d21-fill-invalid-group-order` on the draft 22 wire |
| `d22-fill-location-filter-end-group-overflow` | own | none | `D22-9-20-9-MUST-424` | The same overflowing LOCATION_FILTER inside FILL_PARAMETERS; requires a PROTOCOL_VIOLATION close. Its row also needs `d22-location-filter-end-group-overflow` in the run |
| `d22-fill-recursive-parameter` | shared | `d21-fill-recursive-parameter` | `D22-9-20-15-MUST-432` | As `d21-fill-recursive-parameter` on the draft 22 wire |
| `d22-fill-timeout-outside-fill-or-fetch` | shared | `d21-fill-timeout-outside-fill-or-fetch` | `D22-9-20-1-MUST-396` | As `d21-fill-timeout-outside-fill-or-fetch` on the draft 22 wire |
| `d22-filter-immutable-property` | shared | `d21-filter-immutable-property` | `D22-10-7-MUST-475` | As `d21-filter-immutable-property` on the draft 22 wire |
| `d22-filter-mutable-property` | shared | `d21-filter-mutable-property` | `D22-10-7-MUST-474` | As `d21-filter-mutable-property` on the draft 22 wire |
| `d22-forward-location-and-range-filter-conjunction` | shared | `d21-forward-location-and-range-filter-conjunction` | `D22-3-3-3-MUST-078` | As `d21-forward-location-and-range-filter-conjunction`; see [Filters and fill (draft 21)](#filters-and-fill-draft-21) |
| `d22-forward-value-255` | shared | `d21-forward-value-255` | `D22-9-20-18-MUST-445` | As `d21-forward-value-255` on the draft 22 wire |
| `d22-forward-value-two` | shared | `d21-forward-value-two` | `D22-9-20-18-MUST-445` | As `d21-forward-value-two`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-goaway-on-distinct-request-streams` | shared | `d21-goaway-on-distinct-request-streams` | `D22-9-2-MUST-339` | As `d21-goaway-on-distinct-request-streams` on the draft 22 wire. On draft 22 the two requests are SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS for the run's namespace (draft 21: SUBSCRIBE_NAMESPACE "a" and "b"); the PUBLISH the publisher sends is accepted with REQUEST_OK |
| `d22-goaway-uri-length-boundary` | shared | `d21-goaway-uri-length-boundary` | `D22-9-2-MUST-342` | As `d21-goaway-uri-length-boundary` on the draft 22 wire |
| `d22-grease-auth-token-type` | shared | `d21-grease-auth-token-type` | `D22-13-MUST-568`, `D22-13-MUST-NOT-569` | As `d21-grease-auth-token-type` on the draft 22 wire |
| `d22-grease-request-error` | shared | `d21-grease-request-error` | `D22-13-MUST-568`, `D22-13-MUST-NOT-569`, `D22-13-MUST-NOT-576` | As `d21-grease-request-error` on the draft 22 wire |
| `d22-grease-setup-options` | shared | `d21-grease-setup-options` | `D22-13-MUST-568`, `D22-13-MUST-NOT-569`, `D22-13-MUST-570`, `D22-16-4-MUST-604` | As `d21-grease-setup-options` on the draft 22 wire |
| `d22-grease-stop-sending` | shared | `d21-grease-stop-sending` | `D22-13-MUST-568`, `D22-13-MUST-NOT-569` | As `d21-grease-stop-sending` on the draft 22 wire |
| `d22-group-order-above-two` | shared | `d21-group-order-above-two` | `D22-9-20-8-MUST-421` | As `d21-group-order-above-two` on the draft 22 wire |
| `d22-group-order-in-subscription-update` | shared | `d21-group-order-in-subscription-update` | `D22-9-20-1-MUST-396` | As `d21-group-order-in-subscription-update` on the draft 22 wire |
| `d22-group-order-zero` | shared | `d21-group-order-zero` | `D22-9-20-8-MUST-421` | As `d21-group-order-zero` on the draft 22 wire |
| `d22-immutable-property-repeat` | shared | `d21-immutable-property-repeat` | `D22-10-7-MUST-NOT-465`, `D22-10-7-MUST-NOT-466`, `D22-10-7-MUST-NOT-467` | As `d21-immutable-property-repeat`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-inbound-padding-datagram` | shared | `d21-inbound-padding-datagram` | `D22-11-5-2-MUST-546` | As `d21-inbound-padding-datagram` on the draft 22 wire |
| `d22-inbound-padding-stream` | shared | `d21-inbound-padding-stream` | `D22-11-5-1-MUST-541` | As `d21-inbound-padding-stream` on the draft 22 wire |
| `d22-include-properties-value-255` | shared | `d21-include-properties-value-255` | `D22-9-20-21-MUST-460` | As `d21-include-properties-value-255` on the draft 22 wire |
| `d22-include-properties-value-two` | shared | `d21-include-properties-value-two` | `D22-9-20-21-MUST-460` | As `d21-include-properties-value-two` on the draft 22 wire |
| `d22-invalid-bidirectional-request-stream-opener` | shared | `d21-invalid-bidirectional-request-stream-opener` | `D22-6-3-MUST-156` | As `d21-invalid-bidirectional-request-stream-opener` on the draft 22 wire |
| `d22-largest-object-before-publication` | shared | `d21-largest-object-before-publication` | `D22-9-20-17-MUST-441` | As `d21-largest-object-before-publication` on the draft 22 wire |
| `d22-largest-object-required-after-publication` | shared | `d21-largest-object-required-after-publication` | `D22-9-20-17-MUST-441` | As `d21-largest-object-required-after-publication`; see [Subscriptions, updates and response cardinality](#subscriptions-updates-and-response-cardinality) |
| `d22-location-filter-end-group-overflow` | own | none | `D22-9-20-9-MUST-424` | SUBSCRIBE whose LOCATION_FILTER (Type 0x03) has StartGroup 2^64-1 and EndGroupDelta 1; requires a PROTOCOL_VIOLATION close |
| `d22-message-body-length-mismatch` | shared | `d21-message-body-length-mismatch` | `D22-9-MUST-296` | As `d21-message-body-length-mismatch` on the draft 22 wire |
| `d22-namespace-discovery-authorization` | shared | `d21-namespace-discovery-authorization` | `D22-4-2-1-MUST-115` | As `d21-namespace-discovery-authorization` on the draft 22 wire |
| `d22-namespace-discovery-withdrawal-order` | shared | `d21-namespace-discovery-withdrawal-order` | `D22-4-2-MUST-NOT-112` | As `d21-namespace-discovery-withdrawal-order` on the draft 22 wire |
| `d22-namespace-prefix-update-overlap` | shared | `d21-namespace-prefix-update-overlap` | `D22-9-20-20-MUST-455` | As `d21-namespace-prefix-update-overlap` on the draft 22 wire |
| `d22-native-publisher-empty-query` | shared | `d21-native-publisher-empty-query` | `D22-9-1-2-MUST-315` | As `d21-native-publisher-empty-query`; see [Transport restrictions](#transport-restrictions) |
| `d22-native-publisher-uri-options` | shared | `d21-native-publisher-uri-options` | `D22-9-1-1-MUST-307`, `D22-9-1-2-MUST-314` | As `d21-native-publisher-uri-options`; see [Transport restrictions](#transport-restrictions) |
| `d22-native-publisher-uri-query` | shared | `d21-native-publisher-uri-query` | `D22-9-1-2-MUST-315` | As `d21-native-publisher-uri-query`; see [Transport restrictions](#transport-restrictions) |
| `d22-native-quic-datagram-support` | shared | `d21-native-quic-datagram-support` | `D22-6-2-MUST-153` | As `d21-native-quic-datagram-support`; see [Transport restrictions](#transport-restrictions) |
| `d22-native-quic-required-setup-options` | shared | `d21-native-quic-required-setup-options` | `D22-6-3-2-MUST-164` | As `d21-native-quic-required-setup-options`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-native-quic-without-datagram-negotiation` | shared | `d21-native-quic-without-datagram-negotiation` | `D22-6-2-MUST-154` | As `d21-native-quic-without-datagram-negotiation`; see [Transport restrictions](#transport-restrictions) |
| `d22-object-datagram-flags` | shared | `d21-object-datagram-flags` | `D22-11-2-1-MUST-502` | As `d21-object-datagram-flags` on the draft 22 wire |
| `d22-object-immutable-property-singleton` | shared | `d21-object-immutable-property-singleton` | `D22-10-7-MUST-NOT-477` | As `d21-object-immutable-property-singleton`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-object-property-filter-odd-property-type` | shared | `d21-object-property-filter-odd-property-type` | `D22-9-20-13-MUST-427` | As `d21-object-property-filter-odd-property-type` on the draft 22 wire |
| `d22-original-publisher-opens-new-subgroup` | shared | `d21-original-publisher-opens-new-subgroup` | `D22-2-2-MUST-022` | As `d21-original-publisher-opens-new-subgroup`; see [Objects, Subgroups and datagrams](#objects-subgroups-and-datagrams) |
| `d22-overlapping-subscriptions-distinct-aliases` | shared | `d21-overlapping-subscriptions-distinct-aliases` | `D22-3-1-MUST-043` | As `d21-overlapping-subscriptions-distinct-aliases` on the draft 22 wire |
| `d22-overlapping-subscriptions-shared-alias` | shared | `d21-overlapping-subscriptions-shared-alias` | `D22-3-1-MUST-043` | As `d21-overlapping-subscriptions-shared-alias`; see [Objects, Subgroups and datagrams](#objects-subgroups-and-datagrams) |
| `d22-padding-datagram-emission` | shared | `d21-padding-datagram-emission` | `D22-11-5-MAY-537`, `D22-11-5-2-MAY-544`, `D22-11-5-2-MUST-545` | As `d21-padding-datagram-emission` on the draft 22 wire |
| `d22-padding-stream-emission` | shared | `d21-padding-stream-emission` | `D22-11-5-MAY-536`, `D22-11-5-1-MAY-539`, `D22-11-5-1-MUST-540` | As `d21-padding-stream-emission` on the draft 22 wire |
| `d22-parameter-invalid-message-scope` | shared | `d21-parameter-invalid-message-scope` | `D22-9-20-1-MUST-396` | As `d21-parameter-invalid-message-scope` on the draft 22 wire |
| `d22-parameter-type-delta-overflow` | shared | `d21-parameter-type-delta-overflow` | `D22-9-20-MUST-388` | As `d21-parameter-type-delta-overflow` on the draft 22 wire |
| `d22-prior-group-gap-repeat` | shared | `d21-prior-group-gap-repeat` | `D22-10-8-MUST-NOT-480`, `D22-10-8-MUST-NOT-481` | As `d21-prior-group-gap-repeat`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-prior-group-gap-singleton` | shared | `d21-prior-group-gap-singleton` | `D22-10-8-MUST-NOT-482` | As `d21-prior-group-gap-singleton`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-prior-object-gap-repeat` | shared | `d21-prior-object-gap-repeat` | `D22-10-9-MUST-NOT-485`, `D22-10-9-MUST-NOT-486` | As `d21-prior-object-gap-repeat`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-prior-object-gap-singleton` | shared | `d21-prior-object-gap-singleton` | `D22-10-9-MUST-NOT-487` | As `d21-prior-object-gap-singleton`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-priority-filter-end-above-255` | shared | `d21-priority-filter-end-above-255` | `D22-9-20-12-MUST-425` | As `d21-priority-filter-end-above-255` on the draft 22 wire |
| `d22-priority-filter-start-above-255` | shared | `d21-priority-filter-start-above-255` | `D22-9-20-12-MUST-425` | As `d21-priority-filter-start-above-255` on the draft 22 wire |
| `d22-publish-distinct-tracks-in-one-scope` | shared | `d21-publish-distinct-tracks-in-one-scope` | `D22-2-5-MUST-034` | As `d21-publish-distinct-tracks-in-one-scope`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publish-done-datagram-only` | shared | `d21-publish-done-datagram-only` | `D22-9-9-MUST-376` | As `d21-publish-done-datagram-only` on the draft 22 wire |
| `d22-publish-done-without-data-streams` | shared | `d21-publish-done-without-data-streams` | `D22-9-9-MUST-376` | As `d21-publish-done-without-data-streams` on the draft 22 wire |
| `d22-publish-established-subscriber-sends-publish-state-notify` | shared | `d21-publish-established-subscriber-sends-publish-state-notify` | `D22-9-10-MUST-381` | As `d21-publish-established-subscriber-sends-publish-state-notify` on the draft 22 wire |
| `d22-publish-namespace-ok-with-track-properties` | shared | `d21-publish-namespace-ok-with-track-properties` | `D22-9-3-MUST-348` | As `d21-publish-namespace-ok-with-track-properties` on the draft 22 wire |
| `d22-publish-namespace-redirect-nonempty-track-name` | shared | `d21-publish-namespace-redirect-nonempty-track-name` | `D22-9-4-1-MUST-352` | As `d21-publish-namespace-redirect-nonempty-track-name` on the draft 22 wire |
| `d22-publish-ok-with-track-properties` | shared | `d21-publish-ok-with-track-properties` | `D22-9-3-MUST-348` | As `d21-publish-ok-with-track-properties` on the draft 22 wire |
| `d22-publish-request-error-oversized-reason` | shared | `d21-publish-request-error-oversized-reason` | `D22-8-5-MUST-266` | As `d21-publish-request-error-oversized-reason` on the draft 22 wire |
| `d22-publish-state-notify-before-first-object` | shared | `d21-publish-state-notify-before-first-object` | `D22-9-10-MUST-383` | As `d21-publish-state-notify-before-first-object` on the draft 22 wire |
| `d22-publish-state-notify-known-largest-object` | shared | `d21-publish-state-notify-known-largest-object` | `D22-9-10-MUST-383` | As `d21-publish-state-notify-known-largest-object` on the draft 22 wire |
| `d22-publish-state-notify-on-fetch` | shared | `d21-publish-state-notify-on-fetch` | `D22-9-10-MUST-380` | As `d21-publish-state-notify-on-fetch`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-publish-state-notify-on-namespace-request` | shared | `d21-publish-state-notify-on-namespace-request` | `D22-9-10-MUST-380` | As `d21-publish-state-notify-on-namespace-request`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-publish-state-notify-preserves-subscriber-control` | shared | `d21-publish-state-notify-preserves-subscriber-control` | `D22-9-10-MUST-NOT-382` | As `d21-publish-state-notify-preserves-subscriber-control` on the draft 22 wire |
| `d22-publish-state-notify-requested-forward-change` | shared | `d21-publish-state-notify-requested-forward-change` | `D22-9-10-MUST-NOT-382` | As `d21-publish-state-notify-requested-forward-change` on the draft 22 wire |
| `d22-publish-track-with-mandatory-property` | shared | `d21-publish-track-with-mandatory-property` | `D22-3-7-MUST-088` | As `d21-publish-track-with-mandatory-property`; see [Objects, Subgroups and datagrams](#objects-subgroups-and-datagrams) |
| `d22-publish-update-ok-with-track-properties` | shared | `d21-publish-update-ok-with-track-properties` | `D22-9-3-MUST-348` | As `d21-publish-update-ok-with-track-properties` on the draft 22 wire |
| `d22-publisher-client-goaway-control` | shared | `d21-publisher-client-goaway-control` | `D22-9-2-MUST-329` | As `d21-publisher-client-goaway-control`; see [GOAWAY](#goaway) |
| `d22-publisher-client-goaway-request` | shared | `d21-publisher-client-goaway-request` | `D22-9-2-MUST-329` | As `d21-publisher-client-goaway-request` on the draft 22 wire |
| `d22-publisher-delete-with-pending-alias-uses` | shared | `d21-publisher-delete-with-pending-alias-uses` | `D22-8-9-MUST-NOT-293` | As `d21-publisher-delete-with-pending-alias-uses`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-emitted-namespace-fields` | shared | `d21-publisher-emitted-namespace-fields` | `D22-8-7-MUST-268` | As `d21-publisher-emitted-namespace-fields`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-goaway-alternate-uri` | shared | `d21-publisher-goaway-alternate-uri` | `D22-9-2-MUST-340` | As `d21-publisher-goaway-alternate-uri`; see [Ports and listeners](#ports-and-listeners) |
| `d22-publisher-key-value-type-deltas` | shared | `d21-publisher-key-value-type-deltas` | `D22-8-3-MUST-NOT-248` | As `d21-publisher-key-value-type-deltas`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-location-filter-parameter` | own | none | `D22-9-20-9-MAY-422` | SUBSCRIBE with Absolute Start 7/0; any PUBLISH or PUBLISH_STATE_NOTIFY LOCATION_FILTER the publisher sends must be well formed and, on the probe's subscription, report the requested filter (MAY row: scores only when one is sent) |
| `d22-publisher-namespace-redirect` | shared | `d21-publisher-namespace-redirect` | `D22-9-4-1-MUST-351` | As `d21-publisher-namespace-redirect` on the draft 22 wire |
| `d22-publisher-namespace-routing-announcement` | shared | `d21-publisher-namespace-routing-announcement` | `D22-7-6-MUST-226` | As `d21-publisher-namespace-routing-announcement`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-parameter-multiplicity` | shared | `d21-publisher-parameter-multiplicity` | `D22-9-20-MUST-NOT-391` | As `d21-publisher-parameter-multiplicity` on the draft 22 wire |
| `d22-publisher-parameter-negotiation` | shared | `d21-publisher-parameter-negotiation` | `D22-9-20-MUST-389` | As `d21-publisher-parameter-negotiation` on the draft 22 wire |
| `d22-publisher-parameter-serialization` | shared | `d21-publisher-parameter-serialization` | `D22-9-20-MUST-387` | As `d21-publisher-parameter-serialization` on the draft 22 wire |
| `d22-publisher-request-response-before-fin` | shared | `d21-publisher-request-response-before-fin` | `D22-6-4-2-2-MUST-171` | As `d21-publisher-request-response-before-fin`; see [Cancellation, resets and cleanup](#cancellation-resets-and-cleanup) |
| `d22-publisher-request-stream-openers` | shared | `d21-publisher-request-stream-openers` | `D22-6-3-MUST-NOT-155` | As `d21-publisher-request-stream-openers` on the draft 22 wire |
| `d22-publisher-request-stream-placement` | shared | `d21-publisher-request-stream-placement` | `D22-6-3-MUST-NOT-155`, `D22-9-MUST-294`, `D22-9-1-MUST-NOT-300`, `D22-9-1-1-MUST-NOT-303`, `D22-9-1-2-MUST-NOT-310` | As `d21-publisher-request-stream-placement`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-setup-option-multiplicity` | shared | `d21-publisher-setup-option-multiplicity` | `D22-9-1-MUST-NOT-300` | As `d21-publisher-setup-option-multiplicity`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-publisher-subscribe-tracks-redirect` | shared | `d21-publisher-subscribe-tracks-redirect` | `D22-9-4-1-MUST-351` | As `d21-publisher-subscribe-tracks-redirect` on the draft 22 wire |
| `d22-publisher-update-credit-limit` | shared | `d21-publisher-update-credit-limit` | `D22-9-1-7-MUST-NOT-327` | As `d21-publisher-update-credit-limit`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-publisher-update-credit-per-stream` | shared | `d21-publisher-update-credit-per-stream` | `D22-9-1-7-MUST-NOT-327` | As `d21-publisher-update-credit-per-stream` on the draft 22 wire |
| `d22-publisher-update-zero-unlimited` | shared | `d21-publisher-update-zero-unlimited` | `D22-9-1-7-MUST-NOT-327` | As `d21-publisher-update-zero-unlimited`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-range-filter-default-zero-limit` | shared | `d21-range-filter-default-zero-limit` | `D22-9-1-6-MUST-326` | As `d21-range-filter-default-zero-limit` on the draft 22 wire |
| `d22-range-filter-end-delta-overflow` | shared | `d21-range-filter-end-delta-overflow` | `D22-8-6-MUST-267` | As `d21-range-filter-end-delta-overflow` on the draft 22 wire |
| `d22-range-filter-start-delta-overflow` | shared | `d21-range-filter-start-delta-overflow` | `D22-8-6-MUST-267` | As `d21-range-filter-start-delta-overflow` on the draft 22 wire |
| `d22-range-filter-total-exceeds-negotiated-limit` | shared | `d21-range-filter-total-exceeds-negotiated-limit` | `D22-3-3-2-MUST-077` | As `d21-range-filter-total-exceeds-negotiated-limit` on the draft 22 wire |
| `d22-range-filter-total-limit` | shared | `d21-range-filter-total-limit` | `D22-9-1-6-MUST-326` | As `d21-range-filter-total-limit` on the draft 22 wire |
| `d22-range-filter-update-total-limit` | shared | `d21-range-filter-update-total-limit` | `D22-9-1-6-MUST-326` | As `d21-range-filter-update-total-limit` on the draft 22 wire |
| `d22-range-filter-with-zero-negotiated-limit` | shared | `d21-range-filter-with-zero-negotiated-limit` | `D22-3-3-2-MUST-077` | As `d21-range-filter-with-zero-negotiated-limit` on the draft 22 wire |
| `d22-register-token-on-other-request-error` | shared | `d21-register-token-on-other-request-error` | `D22-8-9-MUST-283` | As `d21-register-token-on-other-request-error` on the draft 22 wire |
| `d22-register-token-on-unauthorized-request` | shared | `d21-register-token-on-unauthorized-request` | `D22-8-9-MUST-283` | As `d21-register-token-on-unauthorized-request` on the draft 22 wire |
| `d22-reject-publish-before-object-production` | shared | `d21-reject-publish-before-object-production` | `D22-3-1-2-MUST-NOT-049` | As `d21-reject-publish-before-object-production`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-rejected-subscribe-no-delivery` | shared | `d21-rejected-subscribe-no-delivery` | `D22-3-1-2-MUST-NOT-049` | As `d21-rejected-subscribe-no-delivery`; see [Namespaces, reserved names and publisher announcements](#namespaces-reserved-names-and-publisher-announcements) |
| `d22-repeat-object-retrieval` | shared | `d21-repeat-object-retrieval` | `D22-2-1-MUST-NOT-016` | As `d21-repeat-object-retrieval`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-request-alias-registration-with-default-zero-cache` | shared | `d21-request-alias-registration-with-default-zero-cache` | `D22-8-9-MUST-289` | As `d21-request-alias-registration-with-default-zero-cache` on the draft 22 wire |
| `d22-request-deleted-token-alias` | shared | `d21-request-deleted-token-alias` | `D22-8-9-MUST-281` | As `d21-request-deleted-token-alias`; see [Authorization tokens](#authorization-tokens) |
| `d22-request-id-wrong-sender-parity` | shared | `d21-request-id-wrong-sender-parity` | `D22-6-4-2-1-MUST-168` | As `d21-request-id-wrong-sender-parity` on the draft 22 wire |
| `d22-request-message-truncated-at-fin` | shared | `d21-request-message-truncated-at-fin` | `D22-9-MUST-296` | As `d21-request-message-truncated-at-fin` on the draft 22 wire |
| `d22-request-single-period-namespace` | shared | `d21-request-single-period-namespace` | `D22-2-4-3-MUST-033` | As `d21-request-single-period-namespace` on the draft 22 wire |
| `d22-request-stream-before-peer-setup` | own | none | `D22-6-3-MAY-159` | Opens the control stream with one byte of SETUP, sends a SUBSCRIBE (FORWARD=0), then completes SETUP; passes (MAY row) when the publisher resets the request before the SETUP is complete or answers it only afterwards; an earlier answer is inconclusive |
| `d22-request-stream-terminal-message-order` | shared | `d21-request-stream-terminal-message-order` | `D22-6-4-2-2-MUST-NOT-170` | As `d21-request-stream-terminal-message-order`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-request-token-cache-overflow` | shared | `d21-request-token-cache-overflow` | `D22-8-9-MUST-289` | As `d21-request-token-cache-overflow` on the draft 22 wire |
| `d22-request-undecodable-authorization-token` | shared | `d21-request-undecodable-authorization-token` | `D22-8-9-MUST-279` | As `d21-request-undecodable-authorization-token` on the draft 22 wire |
| `d22-request-unknown-token-alias` | shared | `d21-request-unknown-token-alias` | `D22-8-9-MUST-281` | As `d21-request-unknown-token-alias` on the draft 22 wire |
| `d22-request-update-independent-streams` | shared | `d21-request-update-independent-streams` | `D22-9-1-7-MUST-328` | As `d21-request-update-independent-streams` on the draft 22 wire |
| `d22-request-update-overrun` | shared | `d21-request-update-overrun` | `D22-9-1-7-MUST-328` | As `d21-request-update-overrun` on the draft 22 wire |
| `d22-request-update-unlimited` | shared | `d21-request-update-unlimited` | `D22-9-1-7-MUST-328` | As `d21-request-update-unlimited` on the draft 22 wire |
| `d22-request-well-formed-invalid-token` | shared | `d21-request-well-formed-invalid-token` | `D22-8-9-MUST-282` | As `d21-request-well-formed-invalid-token`; see [Operator-supplied credentials and policy](#operator-supplied-credentials-and-policy) |
| `d22-responder-update-on-publish-namespace` | shared | `d21-responder-update-on-publish-namespace` | `D22-9-5-MUST-355` | As `d21-responder-update-on-publish-namespace` on the draft 22 wire |
| `d22-server-sends-authority` | shared | `d21-server-sends-authority` | `D22-9-1-1-MUST-304`, `D22-9-1-1-MUST-305` | As `d21-server-sends-authority`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-server-sends-path` | shared | `d21-server-sends-path` | `D22-9-1-2-MUST-311`, `D22-9-1-2-MUST-312` | As `d21-server-sends-path`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-session-namespace-empty-track-request` | shared | `d21-session-namespace-empty-track-request` | `D22-6-5-MUST-184` | As `d21-session-namespace-empty-track-request` on the draft 22 wire |
| `d22-session-namespace-unknown-namespace-request` | shared | `d21-session-namespace-unknown-namespace-request` | `D22-6-5-MUST-186` | As `d21-session-namespace-unknown-namespace-request` on the draft 22 wire |
| `d22-session-namespace-unknown-track-request` | shared | `d21-session-namespace-unknown-track-request` | `D22-6-5-MUST-185` | As `d21-session-namespace-unknown-track-request` on the draft 22 wire |
| `d22-setup-duplicate-unknown-options` | shared | `d21-setup-duplicate-unknown-options` | `D22-9-1-MUST-298`, `D22-9-1-MUST-299`, `D22-9-1-MUST-301` | As `d21-setup-duplicate-unknown-options`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-setup-key-value-declared-length-overflow` | shared | `d21-setup-key-value-declared-length-overflow` | `D22-8-3-MUST-250` | As `d21-setup-key-value-declared-length-overflow` on the draft 22 wire |
| `d22-setup-key-value-type-overflow` | shared | `d21-setup-key-value-type-overflow` | `D22-8-3-MUST-249` | As `d21-setup-key-value-type-overflow` on the draft 22 wire |
| `d22-setup-known-key-value-malformed-value` | shared | `d21-setup-known-key-value-malformed-value` | `D22-8-3-MUST-251` | As `d21-setup-known-key-value-malformed-value` on the draft 22 wire |
| `d22-setup-register-default-zero-cache` | shared | `d21-setup-register-default-zero-cache` | `D22-9-1-4-MUST-NOT-318` | As `d21-setup-register-default-zero-cache` on the draft 22 wire |
| `d22-setup-register-exceeds-token-cache` | shared | `d21-setup-register-exceeds-token-cache` | `D22-9-1-4-MUST-NOT-318` | As `d21-setup-register-exceeds-token-cache` on the draft 22 wire |
| `d22-setup-register-use-value-fallback` | shared | `d21-setup-register-use-value-fallback` | `D22-9-1-4-MUST-319` | As `d21-setup-register-use-value-fallback`; see [Authorization tokens](#authorization-tokens) |
| `d22-setup-unknown-options` | shared | `d21-setup-unknown-options` | `D22-9-1-MUST-298`, `D22-9-1-MUST-299` | As `d21-setup-unknown-options`; see [Session setup, SETUP options and GREASE](#session-setup-setup-options-and-grease) |
| `d22-single-request-update-response` | shared | `d21-single-request-update-response` | `D22-9-5-MUST-356` | As `d21-single-request-update-response` on the draft 22 wire |
| `d22-subgroup-completion-withheld-acknowledgments` | shared | `d21-subgroup-completion-withheld-acknowledgments` | `D22-5-2-MUST-144` | As `d21-subgroup-completion-withheld-acknowledgments`; see [Filters and fill (draft 21)](#filters-and-fill-draft-21) |
| `d22-subgroup-early-handoff-reset` | shared | `d21-subgroup-early-handoff-reset` | `D22-11-3-2-MUST-519` | As `d21-subgroup-early-handoff-reset` on the draft 22 wire |
| `d22-subgroup-header-flags` | shared | `d21-subgroup-header-flags` | `D22-11-3-1-MUST-510` | As `d21-subgroup-header-flags` on the draft 22 wire |
| `d22-subgroup-premature-close-reset` | shared | `d21-subgroup-premature-close-reset` | `D22-11-3-2-MUST-513` | As `d21-subgroup-premature-close-reset` on the draft 22 wire |
| `d22-subgroup-restart-after-reset` | shared | `d21-subgroup-restart-after-reset` | `D22-2-2-MUST-NOT-020` | As `d21-subgroup-restart-after-reset`; see [Objects, Subgroups and datagrams](#objects-subgroups-and-datagrams) |
| `d22-subgroup-start-location-fin` | shared | `d21-subgroup-start-location-fin` | `D22-11-3-2-MUST-512` | As `d21-subgroup-start-location-fin` on the draft 22 wire |
| `d22-subscribe-33-namespace-fields` | shared | `d21-subscribe-33-namespace-fields` | `D22-8-7-MUST-270` | As `d21-subscribe-33-namespace-fields` on the draft 22 wire |
| `d22-subscribe-accepted` | shared | `d21-subscribe-accepted` | `D22-3-1-MUST-035` | As `d21-subscribe-accepted`; see [Subscriptions, updates and response cardinality](#subscriptions-updates-and-response-cardinality) |
| `d22-subscribe-bounded-location-range` | own | none | `D22-3-3-1-MUST-NOT-069` | Five concurrent FORWARD=1 SUBSCRIBEs, each with one LOCATION_FILTER (Relative Start, Absolute Start 7/9, Absolute Bounded, Absolute Range 7/9-7/9, Next Object); fails an Object delivered outside the range the filter selects from the reported Largest Object (Section 9.20.9, Table 6). Needs distinct Track Aliases or LARGEST_OBJECT to attribute Objects |
| `d22-subscribe-empty-namespace-field` | shared | `d21-subscribe-empty-namespace-field` | `D22-8-7-MUST-269` | As `d21-subscribe-empty-namespace-field` on the draft 22 wire |
| `d22-subscribe-namespace-accepted` | shared | `d21-subscribe-namespace-accepted` | `D22-4-2-MUST-108` | As `d21-subscribe-namespace-accepted` on the draft 22 wire |
| `d22-subscribe-namespace-overlap` | shared | `d21-subscribe-namespace-overlap` | `D22-4-2-MUST-111` | As `d21-subscribe-namespace-overlap` on the draft 22 wire |
| `d22-subscribe-namespace-rejected` | shared | `d21-subscribe-namespace-rejected` | `D22-4-2-MUST-108` | As `d21-subscribe-namespace-rejected` on the draft 22 wire |
| `d22-subscribe-oversized-full-track-name` | shared | `d21-subscribe-oversized-full-track-name` | `D22-8-7-MUST-272` | As `d21-subscribe-oversized-full-track-name` on the draft 22 wire |
| `d22-subscribe-parameters-preserve-payload` | shared | `d21-subscribe-parameters-preserve-payload` | `D22-9-20-MUST-NOT-394` | As `d21-subscribe-parameters-preserve-payload` on the draft 22 wire |
| `d22-subscribe-rejected` | shared | `d21-subscribe-rejected` | `D22-3-1-MUST-035` | As `d21-subscribe-rejected` on the draft 22 wire |
| `d22-subscribe-single-subgroup` | shared | `d21-subscribe-single-subgroup` | `D22-2-2-MUST-NOT-020` | As `d21-subscribe-single-subgroup`; see [Objects, Subgroups and datagrams](#objects-subgroups-and-datagrams) |
| `d22-subscribe-tracks-overlap` | shared | `d21-subscribe-tracks-overlap` | `D22-3-6-MUST-083` | As `d21-subscribe-tracks-overlap` on the draft 22 wire. On draft 22 the publisher's PUBLISH is accepted with REQUEST_OK (a courtesy write, not stimulus) |
| `d22-subscribe-tracks-oversized-namespace` | shared | `d21-subscribe-tracks-oversized-namespace` | `D22-8-7-MUST-271` | As `d21-subscribe-tracks-oversized-namespace` on the draft 22 wire |
| `d22-subscribe-tracks-publish-skipped-then-capacity-recovers` | shared | `d21-subscribe-tracks-publish-skipped-then-capacity-recovers` | `D22-3-6-3-MUST-NOT-086` | As `d21-subscribe-tracks-publish-skipped-then-capacity-recovers`; see [Discovery (SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS)](#discovery-subscribe_namespace-subscribe_tracks) |
| `d22-subscriber-sends-publish-state-notify` | shared | `d21-subscriber-sends-publish-state-notify` | `D22-9-10-MUST-381` | As `d21-subscriber-sends-publish-state-notify` on the draft 22 wire |
| `d22-subscriber-update-on-publish` | shared | `d21-subscriber-update-on-publish` | `D22-9-5-MUST-355` | As `d21-subscriber-update-on-publish` on the draft 22 wire |
| `d22-subscription-forwarding-preference` | shared | `d21-subscription-forwarding-preference` | `D22-2-1-1-MUST-018` | As `d21-subscription-forwarding-preference`; see [Scenarios that need FETCH](#scenarios-that-need-fetch) |
| `d22-successful-subscribe-forward-zero` | shared | `d21-successful-subscribe-forward-zero` | `D22-9-6-MUST-366` | As `d21-successful-subscribe-forward-zero` on the draft 22 wire |
| `d22-successful-subscribe-object-delivery` | shared | `d21-successful-subscribe-object-delivery` | `D22-9-6-MUST-366` | As `d21-successful-subscribe-object-delivery` on the draft 22 wire |
| `d22-successful-subscribe-response` | shared | `d21-successful-subscribe-response` | `D22-9-6-MUST-365` | As `d21-successful-subscribe-response` on the draft 22 wire |
| `d22-token-delete-and-reuse` | shared | `d21-token-delete-and-reuse` | `D22-8-9-MUST-276` | As `d21-token-delete-and-reuse`; see [Authorization tokens](#authorization-tokens) |
| `d22-token-duplicate-registration` | shared | `d21-token-duplicate-registration` | `D22-8-9-MUST-280` | As `d21-token-duplicate-registration` on the draft 22 wire |
| `d22-token-register-alias-lifetime` | shared | `d21-token-register-alias-lifetime` | `D22-8-9-MUST-277` | As `d21-token-register-alias-lifetime`; see [Authorization tokens](#authorization-tokens) |
| `d22-track-discovery-authorization` | shared | `d21-track-discovery-authorization` | `D22-3-6-MUST-084` | As `d21-track-discovery-authorization` on the draft 22 wire |
| `d22-track-discovery-does-not-copy-authorization` | shared | `d21-track-discovery-does-not-copy-authorization` | `D22-9-20-2-MUST-NOT-399` | As `d21-track-discovery-does-not-copy-authorization` on the draft 22 wire |
| `d22-track-prefix-update-overlap` | shared | `d21-track-prefix-update-overlap` | `D22-9-20-20-MUST-456` | As `d21-track-prefix-update-overlap` on the draft 22 wire. On draft 22 the publisher's PUBLISH is accepted with REQUEST_OK (a courtesy write, not stimulus) |
| `d22-track-property-filter-odd-property-type` | shared | `d21-track-property-filter-odd-property-type` | `D22-9-20-14-MUST-429` | As `d21-track-property-filter-odd-property-type` on the draft 22 wire |
| `d22-unexpected-duplicate-message-parameter` | shared | `d21-unexpected-duplicate-message-parameter` | `D22-9-20-SHOULD-393` | As `d21-unexpected-duplicate-message-parameter` on the draft 22 wire |
| `d22-unknown-control-message` | shared | `d21-unknown-control-message` | `D22-9-MUST-295` | As `d21-unknown-control-message` on the draft 22 wire |
| `d22-unknown-datagram-type` | shared | `d21-unknown-datagram-type` | `D22-11-MUST-488` | As `d21-unknown-datagram-type` on the draft 22 wire |
| `d22-unknown-message-parameter` | shared | `d21-unknown-message-parameter` | `D22-9-20-MUST-390` | As `d21-unknown-message-parameter` on the draft 22 wire |
| `d22-unknown-request-stream-message` | shared | `d21-unknown-request-stream-message` | `D22-9-MUST-295` | As `d21-unknown-request-stream-message` on the draft 22 wire |
| `d22-unknown-unidirectional-stream-type` | shared | `d21-unknown-unidirectional-stream-type` | `D22-6-4-1-MUST-167` | As `d21-unknown-unidirectional-stream-type` on the draft 22 wire |
| `d22-update-on-track-status` | shared | `d21-update-on-track-status` | `D22-9-5-MUST-355` | As `d21-update-on-track-status` on the draft 22 wire |
| `d22-update-subscription-location-range` | own | none | `D22-3-3-1-MUST-NOT-069` | The same five filters, each set by a REQUEST_UPDATE (FORWARD=1 plus LOCATION_FILTER) on a FORWARD=0 subscription; judged as above |
| `d22-webtransport-h3-datagram-support` | shared | `d21-webtransport-h3-datagram-support` | `D22-6-2-MUST-153` | As `d21-webtransport-h3-datagram-support`; see [Transport restrictions](#transport-restrictions) |
| `d22-webtransport-h3-without-datagram-negotiation` | shared | `d21-webtransport-h3-without-datagram-negotiation` | `D22-6-2-MUST-154` | As `d21-webtransport-h3-without-datagram-negotiation`; see [Transport restrictions](#transport-restrictions) |
| `d22-webtransport-publisher-setup` | shared | `d21-webtransport-publisher-setup` | `D22-9-1-1-MUST-NOT-303`, `D22-9-1-2-MUST-NOT-310` | As `d21-webtransport-publisher-setup`; see [Transport restrictions](#transport-restrictions) |
| `d22-webtransport-required-setup-options` | shared | `d21-webtransport-required-setup-options` | `D22-6-3-2-MUST-164` | As `d21-webtransport-required-setup-options`; see [Transport restrictions](#transport-restrictions) |
| `d22-webtransport-server-sends-authority` | shared | `d21-webtransport-server-sends-authority` | `D22-9-1-1-MUST-305` | As `d21-webtransport-server-sends-authority`; see [Transport restrictions](#transport-restrictions) |
| `d22-webtransport-server-sends-path` | shared | `d21-webtransport-server-sends-path` | `D22-9-1-2-MUST-312` | As `d21-webtransport-server-sends-path`; see [Transport restrictions](#transport-restrictions) |

### Unscored draft 22 probes

These run when a run request names them, like any raw probe, but are not in `executable_profiles` and no
catalog row names them. Each records its verdict as an `unscored_probe_verdict` event; scoring never
sees it.

| Draft 22 ID | Stimulus | Verdict recorded |
|---|---|---|
| `d22-location-filter-unknown-type` | SUBSCRIBE whose LOCATION_FILTER has Type 0x06, the first undefined value | pass on a PROTOCOL_VIOLATION close, fail on another close code, no verdict without a close in the reaction window (Section 9.20.9: any other Location Filter Type is a PROTOCOL_VIOLATION) |
| `d22-location-filter-absolute-origin` | SUBSCRIBE whose LOCATION_FILTER is Absolute Start (Type 0x02) {0, 0}, a form no draft 21 field list can express | pass when the publisher answers SUBSCRIBE_OK and delivers an Object, fail on a PROTOCOL_VIOLATION close (the valid filter read as malformed), no verdict otherwise; a control for the probe above |

## moq-lite-06 scenarios (L1d, L2a, L2b)

Counts: 27 scenarios (19 from L1d, 7 from L2a, 1 from L2b), 40 evaluators, 44 evaluator bindings (39 single-scenario rows and row
`L06-4-4-MUST-027`, which is bound once per scenario on five scenarios). They are defined by `requirements/moq-lite-06.json`,
a complete catalog (`complete: true` since L2c; all 212 rows are reviewed). The publisher under test is the
CLIENT that dials the runner; the runner is the server and the subscriber. A moq-lite-06 run is scored as the drafts' are:
its verdict is Fail when a required row failed, Incomplete while a scored row is unjudged, and Pass when every scored row
is judged or not applicable (a rule out of the publisher's reach is `not_applicable`, see
[scoring-and-audit.md](scoring-and-audit.md#moq-lite-06-audit)). The scenarios exist in the registry and the
audit CLI (`moq-interop-audit --draft moq-lite-06` reports 36 of 36 required reviewed rows covered), and since L1e the
HTTP API accepts moq-lite-06 runs (`"draft": "moq-lite-06"`, see [http-api.md](http-api.md#moq-lite-06-runs)) and
`adapters/moq-lite` drives the `moq` CLI as the publisher. The sweeps are in
[interop-notes.md](interop-notes.md#moq-lite-06-first-sweep-against-the-moq-cli-b8b0d235) and
[interop-notes.md](interop-notes.md#moq-lite-06-second-sweep-l2a-against-the-moq-cli-b8b0d235). The session URL is fixed:
path `/moq`, query `token=l1d`, on both transports (see below).

Scenarios marked "track" need the track fixture (broadcast path = namespace fields joined with `/`, plus the track name).
Every probe ends with an ungated allowance wait, so a late violation is still seen; a probe whose deadline cannot cover
its windows is refused as a harness error for that context.

| Scenario | Stimulus (runner to publisher) | Evaluators (rows) | Fail when | Not run when |
|---|---|---|---|---|
| `l06-setup-stream` | The runner's SETUP stream, then a 2 s allowance | `l06-setup-stream-single-setup` (014), `l06-setup-parameters-unique` (111) | 014: no publisher Setup stream by the end of the allowance or at a close, a second Setup stream, a reset, malformed or trailing bytes, or no FIN. 111: a repeated Parameter ID | 014: the allowance has not elapsed and the publisher has not closed. 111: no decodable SETUP, or fewer than two parameters |
| `l06-setup-unknown-parameter` | SETUP carrying undefined Parameter 0x7f, then ANNOUNCE_REQUEST with the empty prefix | `l06-setup-unknown-parameter-ignored` (110) | The publisher closes the session (any code), or resets or refuses the ANNOUNCE_REQUEST | The runner's SETUP or request was not delivered; no publisher SETUP or no ANNOUNCE_OK within the allowance and no close or refusal (those belong to other rows) |
| `l06-setup-duplicate-parameter` | SETUP carrying Cost (0x4) twice | `l06-setup-duplicate-parameter-close` (112), `l06-errors-code-space` (027) | 112: a close with any code other than PROTOCOL_VIOLATION, or no close by the end of the allowance (a stream reaction alone is a Fail). 027: a close in the wrong code space | The runner's SETUP was not delivered; 027 also when there is no close or the code is unregistered |
| `l06-setup-duplicate-stream` | The ordinary SETUP, then a second SETUP stream | `l06-setup-duplicate-stream-close` (092), 027 | As the row above, for 092 and 027 | As above |
| `l06-setup-server-path` | SETUP carrying Path "/" (client-only) | `l06-setup-server-path-close` (126), 027 | As above | As above; 126 is not judged on WebTransport (a Path there is also a URI-binding violation) |
| `l06-setup-server-role` | SETUP carrying Role 0 (client-only) | `l06-setup-server-role-close` (131), 027 | As above | As above |
| `l06-setup-client-path` | The ordinary SETUP; the publisher's SETUP Path is compared with the session URL fields | `l06-setup-path-query-appended` (120), `l06-setup-path-sent` (124), `l06-setup-path-absent-on-uri-binding` (125) | 120: a Path value that differs from path + "?" + query (exact bytes). 124: no Path parameter. 125: a Path parameter of any value | Binding unknown; the session URL has no path or no query; no complete publisher SETUP; 120 with no Path; 120 and 124 on WebTransport, 125 on native QUIC; the publisher closed before the Setup |
| `l06-announce-prefix` (track) | Three ANNOUNCE_REQUESTs: empty prefix, the broadcast's first segment, a disjoint prefix; response window | `l06-announce-ok-then-starts` (139), `l06-announce-hop-list-excludes-own` (141), `l06-announce-ok-hop-assigned` (143) | 139: a reset or refusal instead of the answer, a session close, no ANNOUNCE_OK or too few STARTs by the end of the window, a decode failure inside the covered prefix, no route covering the broadcast on the empty and broadcast prefixes. 141: the ANNOUNCE_OK Hop ID as the last Hop of a route. 143: a Hop ID of 0 | The runner closed or a request never reached the publisher; an unknown ANNOUNCE type or an END for an initial-set id (inconclusive); an incomplete message still buffered when the window ends and the stream is not finished; 141: no response stream, Hop ID 0, or no START or UPDATE decoded; 143: no ANNOUNCE_OK |
| `l06-announce-lifecycle` (track) | One ANNOUNCE_REQUEST held open for the window while the adapter ends and restarts a broadcast | `l06-announce-retired-id-unused` (152) | An UPDATE or a second END for a retired announce id | No ANNOUNCE_END observed (the adapter did not retract the broadcast), no ANNOUNCE_OK, an unknown type or decode failure, or an incomplete message pending |
| `l06-session-stream-close` (track) | ANNOUNCE_REQUEST and SUBSCRIBE, wait for the first answer on both, then the runner FINs both request streams | `l06-session-peer-closes-send` (025) | A stream the publisher does not end within the allowance after the runner's FIN, including when the publisher closes the session instead | The publisher did not answer on both streams before the FIN, or ended a stream before the runner's FIN |
| `l06-subscribe-latest` (track) | Announce exchange, then the default SUBSCRIBE (latest group, unbounded) and an observation window | `l06-group-starts-with-group` (093), `l06-group-unique-sequence` (097), `l06-group-sequence-increments` (190) | 093: a Group stream whose first message is not GROUP, or that breaks before a GROUP. 097: a repeated Group Sequence. 190: a gap in the sequences not covered by SUBSCRIBE_DROP | No Group stream decoded (093); fewer than two groups (097, 190); 190 also when a Group stream has no decoded header or the subscribe stream is unreadable; the stimulus was not delivered |
| `l06-subscribe-refused` (track) | SUBSCRIBE for an uncovered path and SUBSCRIBE for an unknown track | `l06-subscribe-refused-reset` (062) | A FIN, a session close, or (case a) SUBSCRIBE_OK, a Group stream or no reset by the end of the allowance; a SUBSCRIBE_END without a reset by then, in both cases | Either SUBSCRIBE was not delivered; case b still pending, served or unreadable |
| `l06-subscribe-invalid-frame-bounds` (track) | SUBSCRIBE with Group End 0 and non-zero Frame End | `l06-subscribe-invalid-frame-bounds-reset` (023) | SUBSCRIBE_OK or a Group stream, a FIN, a session close, or no reset by the end of the allowance | The stimulus was not delivered, or the answer is unreadable |
| `l06-subscribe-group-floor` (track) | Learning SUBSCRIBE (Max Age 0) to find the latest group L, then floored SUBSCRIBEs at L and at L+2 | `l06-subscribe-no-group-below-floor` (159), `l06-subscribe-ok-group-at-floor` (172) | 159: a Group more than one below the floor. 172: a SUBSCRIBE_OK group more than one below the floor | No group learned (the probe then times out), no group or SUBSCRIBE_OK on a floored subscription, or a group exactly one below the floor (reading conflict) |
| `l06-subscribe-abutting-frame-start` (track) | Learning SUBSCRIBE gives group G; a subscription for frames 0..N-1 of G, then one starting at frame N | `l06-subscribe-resolved-start` (020) | A non-zero Frame Start on the learning or first subscription, or a second-subscription group delivered at a wrong Position | The pattern could not be set up, frames 0..N-1 not received on the first subscription, no group for the second, or the (G-1, N) reading conflict |
| `l06-errors-unknown-stream-type` | A bidirectional stream with unregistered STREAM_TYPE 0x3f, then a conforming ANNOUNCE_REQUEST | `l06-errors-unknown-stream-type-reset` (108), `l06-errors-unknown-stream-type-not-fatal` (109) | 108: neither a RESET_STREAM nor a STOP_SENDING of the stream within the allowance. 109: any publisher close, or the follow-up request refused or reset | 108: any publisher close (109 judges it), the allowance did not elapse. 109: the follow-up got no ANNOUNCE_OK (and no close or refusal) |
| `l06-errors-unknown-reset-code` (track) | Two live SUBSCRIBEs; STOP_SENDING on a Group stream, then RESET_STREAM and STOP_SENDING of one with unregistered code 0x4d1, then a later SUBSCRIBE; about 9 s | `l06-errors-unknown-code-tolerated` (030), `l06-errors-no-assumed-unauthorized` (032) | 030: any close except UNAUTHORIZED, or the other or later subscription refused. 032: a close with UNAUTHORIZED | The subscriptions were not both live, no step carrying the code was delivered, the cancel was not delivered or did not end the cancelled subscription, the later SUBSCRIBE got no SUBSCRIBE_OK, or the live transport refused the Group STOP_SENDING (harness failure); 030 on an UNAUTHORIZED close, 032 on any other close |
| `l06-errors-reserved-reset-code` (track) | As above with reserved code 0x2a, about 6 s | `l06-errors-reserved-code-tolerated` (033) | Any session close, or the other or later subscription refused | As above |
| `l06-errors-code-space` | SUBSCRIBE for an unserved broadcast (stream half), then an ANNOUNCE_REQUEST whose Message Length covers extra bytes (session half) | `l06-errors-code-space` (027), `l06-errors-message-length-close` (107) | 027: a stream code used as a session close or the reverse. 107: a close other than PROTOCOL_VIOLATION, or no close by the end of the allowance | 027: the run does not hold all five of its scenarios (each once), no stream code on this scenario, or no session close of the right space on any of the five (one close suffices: the catalog rationale), or an unregistered code; 107: the Message Length stimulus was not delivered |
| `l06-track-info` (track) | Announce exchange, a Track Stream for the track, 500 ms, a second Track Stream for the same track | `l06-track-info-immutable` (163), `l06-track-info-timescale-nonzero` (170) | 163: the two TRACK_INFO replies differ in Publisher Priority, Publisher Max Age or Timescale. 170: a Timescale of 0 | Fewer than two replies for 163 (a reset, the track unknown, silence, a second reply on one stream, an unreadable reply); no reply for 170; the stimulus was not delivered |
| `l06-fetch-group` (track) | Announce exchange, a Track Stream lookup, a learning SUBSCRIBE (Max Age 0) until a Group stream ends with FIN and at least 3 frames, then three FETCHes of that group: whole, frames 0 and 1, all but frame 0 | `l06-fetch-short-run` (177) | A FIN after fewer frames than the requested range holds in the reference group | No complete group learned in 15 s (no FETCH is sent), every FETCH reset or unanswered (a reset is conforming: the group may have been dropped), more frames than asked on a range (that range gives no verdict), an unreadable answer, no TRACK_INFO |
| `l06-fetch-unknown-group` (track) | Announce exchange, a Track Stream lookup, one FETCH of group 4 000 000 000 | `l06-fetch-unknown-group-reset` (066) | A FIN (empty or with frames), frames, or a session close instead of the stream reset | No answer by the end of the allowance, an unreadable answer, no TRACK_INFO |
| `l06-probe-report` | A Probe Stream with a 4 Mbit/s target, a second target of 8 Mbit/s once the publisher reported (or after 3 s), unless it already reset or ended the stream, then a 3 s allowance | `l06-probe-target-continues` (072), `l06-probe-none-reset` (075) | 072 (publisher advertised Report or Increase): a RESET_STREAM of the Probe Stream or a session close after a target; 075 (advertised no Probe capability or Level 0): a PROBE report, or a FIN or session close instead of a reset | 072: Level None or no decodable SETUP, no report after the second target, the stream unreadable. 075: Level Report or Increase, no decodable SETUP, no reaction by the end of the allowance |
| `l06-datagram-size` (track) | Announce exchange, the default SUBSCRIBE, then a 6 s window in which every QUIC datagram the publisher sends is decoded | `l06-datagram-size-limit` (105) | A datagram whose body exceeds 1200 bytes | No datagram within the limit (datagrams are a permission, so a publisher that sends none, the moq CLI included, is not run), only datagrams whose header does not decode, an unjudgeable transcript |
| `l06-goaway-single` (track) | Announce exchange, a SUBSCRIBE; once three Group streams were opened (the cadence proof, within 10 s), a Goaway Stream with a URI on the reserved `.invalid` host, then 6 s | `l06-goaway-no-new-streams` (077) | A stream the publisher opened more than 2 s after the GOAWAY was written | No GOAWAY sent (no cadence), a session close after the GOAWAY (a graceful shutdown), fewer Group streams before the GOAWAY than three, an observation after the 2 s flight allowance shorter than the longest gap between those Group streams |
| `l06-goaway-duplicate` | A Goaway Stream with a valid GOAWAY, 500 ms, a second Goaway Stream, then a 3 s allowance | `l06-goaway-second-closes` (186) | A session still open at the end of the allowance, or a close with a code other than PROTOCOL_VIOLATION | The first GOAWAY not delivered, or a close before the second GOAWAY was written (including a close on the first) |
| `l06-goaway-oversize` | A Goaway Stream whose GOAWAY claims and carries a 8193 byte URI (a named deliberate violation), then a 3 s allowance | `l06-goaway-oversize-violation` (179) | A session still open at the end of the allowance (a reset of the Goaway Stream alone included), or a close with another code | The stimulus was not delivered, or the peer closed before it |

In every row the transcript must be judgeable: the publisher connected, the session was not cut short by a harness failure, the event limit or the deadline, and the runner's SETUP was delivered. Otherwise all its rows are not run. Rows 014, 111, 110, 109, 112, 092, 126, 131, 107, 027, 025, 139, 062, 023 and 030 to 033 treat a publisher close as part of the observation and prove their own stimulus; rows 141, 143, 152, 093, 097, 190, 159, 172, 020 and 120, 124, 125 instead require that all stimuli were delivered and the publisher had not closed early. Row 108 is not run on any publisher close. A transcript flagged as harness-failed, event-limit reached or timed out drops all its verdicts to not run.

### moq-lite-06 session URL, runner duties and evidence

- **Session URL.** The path and query are the fixed constants `/moq` and `token=l1d` (`kLiteSessionPath` and
  `kLiteSessionQuery`, unreserved characters only, because row 120 is an exact byte match of path + "?" + query) on
  both transports. Endpoint forms: native QUIC `moql://HOST:PORT/moq?token=l1d`, WebTransport
  `https://HOST:PORT/moq?token=l1d`; the WebTransport listener requires the CONNECT `:path` to be exactly
  `/moq?token=l1d`. The probe is told the session has a path and a query on both transports, so rows 120 and 124 are
  judged on native QUIC and row 125 on WebTransport.
- **Runner duties** (opt-in on the probe definition, on for every lite scenario): when the publisher ends (FIN) a
  bidirectional stream the runner opened, the engine FINs the runner's send side itself, as a conforming moq-lite
  endpoint does; the action is recorded as an engine action in the context evidence and is invisible to the
  stimulus-proof logic. The runner does not answer a publisher STOP_SENDING. When a WebTransport client's strictly
  decoded SETUP carries a Path parameter the runner closes the session with PROTOCOL_VIOLATION (draft 7.3.2: a receiver
  of a Path over WebTransport MUST close): row 125 still fails, row 111 is still judged, and every other row of that
  session is `not_run`, row 014 included. This was never observed against the `moq` CLI, which sends no Path on
  WebTransport. A SETUP with a repeated Parameter ID is not closed on by the runner today (open item).
- **Group payload evidence rule.** The payload bytes of FRAME messages on Group streams are not stored in the
  transcript (length and FIN are kept; the counter `group_payload_bytes_dropped` records the elision), so a
  publisher streaming media does not exhaust the recorder; the evaluators of the Group-stream rows use the stream
  structure (headers, sequences, lengths, FIN), never the payload. Since L2a the same holds for the publisher's bytes
  on a Fetch Stream the runner opened (a FETCH response is bare FRAMEs): the decoded frames are kept, the bytes are not,
  and `group_payload_bytes_dropped` counts both.
- **Evidence status names.** Engine actions and refused steps in the `context_complete` evidence name the transport
  status (`status=Success`, `refused=WouldBlock`). A refused WebTransport CONNECT is reported as `refused CONNECT:
  validator_status=404 reason=... path=...`; `validator_status` is the validator's decision, while the status on the
  wire is the HTTP/3 stack's own (h3zero answered 501 when the L1e tests refused a CONNECT), so the two numbers differ.

## What NOT_RUN means for each family

| Family | Typical reasons for `NOT_RUN` |
|---|---|
| Session setup, GREASE | The publisher closed with a transport-level or local close, never completed SETUP, or the stimulus was only partially written |
| GOAWAY | The publisher never sent a GOAWAY, or did not migrate to the replacement URI |
| Announcements, reserved namespaces | The publisher never connected, closed early, or was never induced to attempt the publication |
| Subscriptions, response counts | A context did not complete (missing FIN, reset-only closure), or only one of the required contexts ran |
| FETCH | The track lacks the requested Location (for example Group 7, Object 9), or the response was empty or a range marker. With a no-FETCH declaration: a row that also names a scenario that does not need FETCH, whose FETCH scenario was skipped; rows that name only FETCH scenarios are `not_applicable`, not `not_run` |
| Discovery | The prerequisite subscription was not established, or response evidence was late or incomplete |
| Objects and cancellation | The publisher finished a stream before the trigger, so no reset could be observed; no Objects arrived |
| Filters, fill, MAX_FILTER_RANGES | The publisher did not advertise the capability, or the advertised capacity is too large to exercise |
| Authorization tokens | The credential flag was not set, `MAX_AUTH_TOKEN_CACHE_SIZE` was too small, or the error code mapping is unassigned |
| Publisher-initiated flows | The publisher never produced the request the scenario waits for |
| WebTransport-only or native-only rows | The run used the other transport |
| moq-lite-06 per-transport rows | 125 on native QUIC; 120 and 124 on WebTransport; 126 on WebTransport (a Path there is a URI-binding violation); 152 on both (needs an adapter that ends and restarts a broadcast; the `moq` CLI keeps one); every row but 111 and 125 of a WebTransport session whose SETUP carried a Path; 027 outside a run holding its five scenarios |

`NOT_RUN` is not a failure and never counts as a pass.
