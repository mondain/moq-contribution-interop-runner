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
- In observed mode, read the run's events and reconnect to the same endpoint
  each time a `context_ready` event names the next scenario. In driven mode the
  adapter is started once per context and receives the scenario ID in its
  request. Observed mode does not authenticate the publisher's identity.
- QUIC DATAGRAM must be negotiated; see [the harness guide](publisher-harness-guide.md#7-requirements-your-publisher-must-meet).
- Requests for unsupported scenarios return HTTP 422; they are never scored as
  conformant.

- A publisher may declare that it does not implement FETCH (`publisher_capabilities`
  in the run request, or `--publisher-no-fetch` at startup). Scenarios that need
  FETCH are then skipped, not failed; see [Scenarios that need FETCH](#scenarios-that-need-fetch).

List the executable scenarios (about 165 for draft 18 and 220 for draft 21 at the
time of writing) with the `/healthz` command in [http-api.md](http-api.md). A
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
  CONNECT, exact `moqt-18` or `moqt-21` in `WT-Available-Protocols`). A client
  that sends `Origin` must use an origin listed with `--publisher-origin`;
  clients without `Origin` are accepted unless `--require-publisher-origin` is set.

## Scenario families

The sections below group scenarios by the feature they exercise. "Draft 18" and
"draft 21" scenario IDs differ; draft-21 IDs start with `d21-`.

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

`NOT_RUN` is not a failure and never counts as a pass.
