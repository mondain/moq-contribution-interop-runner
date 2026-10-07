# imquic Punch List

This is a work list for an agent fixing imquic (the library and its example publisher
`examples/moq-pub.c`, built as `imquic-moq-pub`) so that it conforms to MoQT draft 22.
Every I- item comes from runs of the interop runner in this repository against imquic at
`6836173947a5eb0a6edffe3709dab3340ce43871` (branch `fix/core-parser-and-stream-regressions`,
`imquic 0.0.2/alpha`), driven through the bundled `adapters/imquic/run.sh`. The first sweep
(2026-10-06, 15:48 to 16:10 -07:00) found the items; the final sweep of the same day
(18:33 to 18:57), after three runner and adapter fixes, re-checked each of them. Background,
method and the full triage are in
[interop-notes.md](interop-notes.md#draft-22-sweep-against-imquic-6836173).

Source line numbers below are from the scratch copy of `6836173` that the sweeps ran (built
with `./configure --enable-moq-examples`); the read-only checkout may move, so its line
numbers can differ. Draft line numbers are in `docs/draft-ietf-moq-transport-22.txt`.

Confidence labels: **confirmed** (wire evidence in the final sweep and the source agrees),
**observed** (wire or log evidence, no scored row), **question** (the reading of the draft or
the evidence is open; worded as a question). An item whose evidence was confounded by a
runner defect in the first sweep says so.

Where the fault lies: the peer under test is moq-pub built on imquic. Each item says whether
the fault is in the imquic **library** (it applies to every imquic application) or in the
moq-pub **example** (a demo limit, reported but labeled as such, not a library defect). One
former item, I-12, turned out to be a runner evaluator assumption and is withdrawn; its id is
kept so the numbering stays stable. The runner defect behind it is fixed (`92d3c58`), and
re-runs since then found a genuine, phase-dependent library finding on the same row (see I-12).

## Status in the final sweep

| Item | Status | Rows |
|------|--------|------|
| I-01 WebTransport never connects | Confirmed (unchanged) | every WebTransport row `not_run` |
| I-02 GOAWAY Request ID parity check | Confirmed | D22-9-2-MUST-338 fail; D22-9-2-MUST-340 not_run |
| I-03 zero-field Track Namespace closed | Observed in the first sweep only; no row depends on it after R2 | none |
| I-04 crash on an oversized Full Track Name | Confirmed (library check missing; crash in the example) | D22-8-7-MUST-272 not_run (error run) |
| I-05 absent FORWARD treated as 0 | Confirmed (example) | D22-3-1-2-MUST-047 not_run |
| I-06a REQUEST_UPDATE never answered by moq-pub | Confirmed (example) | D22-9-5-MUST-356, D22-9-5-1-MUST-357, D22-6-4-2-2-MUST-172, D22-9-9-MUST-376, D22-9-5-MUST-355 not_run |
| I-06b REQUEST_UPDATE overrun closed with the wrong code | Confirmed (library) | D22-9-1-7-MUST-328 not_run |
| I-07 malformed or unknown input ignored | Confirmed | D22-6-4-1-MUST-167, D22-6-3-MUST-156, D22-9-MUST-296, D22-8-3-MUST-251, D22-9-20-9-MUST-424, D22-8-3-MUST-249, D22-8-3-MUST-250 fail; D22-11-MUST-488 not_run |
| I-08 native QUIC SETUP without AUTHORITY and PATH | Confirmed | D22-6-3-2-MUST-164, D22-9-1-1-MUST-307, D22-9-1-2-MUST-314 fail |
| I-09 server AUTHORITY closed with INVALID_PATH | Confirmed | D22-9-1-1-MUST-304 fail |
| I-10 namespace REDIRECT with a Track Name ends with NO_ERROR | Confirmed | D22-9-4-1-MUST-352 fail |
| I-11 INVALID_FILTER never sent | Confirmed | D22-3-3-2-MUST-077, D22-9-1-6-MUST-326 not_run |
| I-12 (withdrawn) SUBGROUP_DELIVERY_TIMEOUT | The listed FAIL was a runner evaluator assumption (triage A4), fixed in the runner (`92d3c58`); a phase-dependent FAIL since the fix is genuine: the library does not enforce the timeout (confirmed) | D22-5-2-MUST-144: not_run or fail depending on the phase of moq-pub's one-minute group (was a false FAIL in the final sweep) |
| I-13 duplicate Request ID not detected | Confirmed | D22-6-4-2-1-MUST-169 not_run |
| I-14 `.session` namespace request gets NOT_SUPPORTED | Confirmed | D22-6-5-MUST-186 fail |
| I-15 requests answered before SETUP completes | Observed (MAY row, low) | D22-6-3-MAY-159 not_run |
| I-16 absolute LOCATION_FILTER start not honored | Observed (example) | none |
| I-17 REQUEST_ERROR 0x19 is not a draft 22 code | Observed (example) | none (it limits the D-sub1 rows of the triage) |
| I-18 SUBSCRIBE_OK always `04 0002 00 00` | Question (example) | none |
| I-19 AUTHORIZATION_TOKEN never decoded | Confirmed (new; confounded in the first sweep) | D22-8-9-MUST-279, -281, -289 fail |
| I-20 GROUP_ORDER 0 accepted | Confirmed (new; a false pass in the first sweep) | D22-9-20-8-MUST-421 fail |
| I-21 parameters accepted outside their message scope | Confirmed (new) | D22-9-20-1-MUST-396 fail |
| I-22 duplicate parameters accepted | Confirmed (new; SHOULD) | D22-9-20-SHOULD-393 not_run |
| I-C1 FILL_PARAMETERS encoding | Question (unchanged) | D22-3-4-1-MUST-079, -080, -081 not_run; confounds the D22-9-20-15-MUST-432 pass |

The runner fixes between the two sweeps (all in this repository, none in imquic): R1, the
adapter keeps a small supervisor so that moq-pub's 40 to 160 ms shutdown no longer ends runs
in a SIGKILL `error`; R2, 29 shared probes send the run's namespace and track on the draft 22
wire instead of namespace () and track "x"; R3, three SETUP probes run announce-and-wait.
Rows per triage category (final native sweep, 101 non-pass rows): (a) 1 (D22-5-2-MUST-144,
the evaluator assumption that withdrew I-12), (b) 34 (19 fail, 15 not_run; the items above
except I-01, which covers WebTransport), (c) 3 (I-C1), (d) 63 (not applicable to this demo;
see interop-notes.md). Items not labeled example or withdrawn are library items. Those counts
were taken before the evaluator fix `92d3c58`. With it, the re-run of D22-5-2-MUST-144's
scenario in this sweep gave not_run, so the numbers of this sweep become 45 pass, 19 fail,
82 not_run and the categories (a) 0, (b) 34, (c) 3, (d) 64. That row's outcome depends on
when in the minute a run starts: in some phases it is a genuine FAIL (see I-12).

R2 is what turned the token and parameter probes (I-19 to I-22) from confounded results into
evidence about imquic.

One runner note sits next to the I-19 FAIL of D22-8-9-MUST-281: the run also stores an
`unresolved_error_mapping` event whose text says `result=NOT_RUN`
(`src/app/native_run_manager.cpp` lines 1305-1308). That text describes only a REQUEST_ERROR
answer (draft 22 assigns UNKNOWN_AUTH_TOKEN_ALIAS no REQUEST_ERROR code, lines 7928-7980);
imquic answered SUBSCRIBE_OK, which the evaluator scores FAIL. The FAIL and the event do not
contradict each other.

## Items

### I-01 The WebTransport client never sends its extended CONNECT

- **Rows:** every row of the WebTransport sweep (144, all `not_run`; no session is ever
  established).
- **Observed:** moq-pub logs "Connection established (ALPN=h3)" and, before it, "Buffering
  incoming STREAM data on unknown context" for the server's early HTTP/3 control and QPACK
  streams; no CONNECT request follows, and the run ends at its deadline.
- **Where (hypothesis, confirmed by the log):** `conn->http3` is created only at
  `picoquic_callback_almost_ready` (`src/quic.c` lines 276-296); stream data that arrives
  earlier goes to the MoQ pending buffer (`src/moq.c` line 340) instead of the HTTP/3 layer,
  so the server's SETTINGS are never seen, and `imquic_http3_check_send_connect`
  (`src/http3.c` lines 808-809) waits for `settings_received` forever.
- **Required:** route early HTTP/3 stream data to the HTTP/3 connection so SETTINGS arrive
  and the CONNECT is sent. Highest priority: it hides every WebTransport result.

### I-02 GOAWAY fails a Request ID check on a field draft 22 does not have (confirmed)

- **Rows:** D22-9-2-MUST-338 (fail), D22-9-2-MUST-340 (not_run).
- **Scenarios:** `d22-duplicate-control-goaway`, `d22-publisher-goaway-alternate-uri`.
- **Draft:** lines 4116-4122 (GOAWAY carries a New Session URI and a timeout, no Request
  ID), 4113-4115 (a second GOAWAY is a PROTOCOL_VIOLATION), 4129-4130 (use the URI).
- **Observed:** the first control-stream GOAWAY (`10 0003 00 a7 10`, or with a URI
  `10 0021 1f moqt://...`) closes the session with 0x4 INVALID_REQUEST_ID:
  "imquic_moq_parse_goaway:5031 Invalid Request ID".
- **Where:** `src/moq.c` lines 5029-5031 apply the draft 18 parity check for every version
  `>= 18`, while the field is parsed only at version 18, so `request_id` stays 0.
- **Required:** apply the check only where the field exists; then close on the second GOAWAY
  and honor the URI.

### I-03 A SUBSCRIBE or TRACK_STATUS with zero namespace fields is closed (observed, first sweep)

- **Rows:** none in the final sweep. In the first sweep this close confounded about 28
  probes (runner item R2, fixed): it produced false FAILs (D22-8-9-MUST-279, -281, -289) and
  false passes (for example D22-9-20-8-MUST-421).
- **Draft:** lines 885-886: a Track Namespace has "between 0 and 32" fields.
- **Observed:** `03 0007 01 00 01 78 ...` closed 0x3 "imquic_moq_parse_subscribe Invalid number
  of namespaces".
- **Where:** the `IMQUIC_MOQ_PARSE_NAMESPACES` macro (`src/moq.c` lines 1911-1917) allows zero
  fields only for SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS, NAMESPACE, NAMESPACE_DONE and
  REQUEST_ERROR.
- **Required:** accept zero fields wherever a Track Namespace is allowed. No runner row
  exercises this any more, so confirm with imquic's own tests.

### I-04 An oversized Full Track Name crashes the publisher (confirmed)

- **Row:** D22-8-7-MUST-272 (not_run; the run ends `error`, publisher exit 139).
- **Scenario:** `d22-subscribe-oversized-full-track-name`.
- **Draft:** lines 3576-3580 (a Full Track Name over 4,096 bytes is a PROTOCOL_VIOLATION).
- **Observed:** "imquic_moq_namespace_str:285 Insufficient buffer to render namespace(s)",
  then a segmentation fault ("the monitored command dumped core").
- **Where:** `src/imquic-moq.c` line 285 gives up on a too-small buffer and returns NULL;
  moq-pub then passes the result to `strcasecmp` (`examples/moq-pub.c` lines 188-197), which
  most likely is the crash (not traced in a debugger). The missing 4,096-byte check is the
  library's; the NULL dereference is the example's.
- **Required:** check the length and close with PROTOCOL_VIOLATION; never dereference the
  failed rendering.

### I-05 An absent FORWARD is treated as 0 (confirmed; example)

- **Row:** D22-3-1-2-MUST-047 (not_run).
- **Scenario:** `d22-cancel-subscribe-with-open-streams` (also why the unscored probe
  `d22-location-filter-absolute-origin` stays `not_run`).
- **Draft:** lines 5618-5621: if FORWARD is absent the value is 1.
- **Observed:** SUBSCRIBE without FORWARD (`03 0010 01 01 05 media 06 vide_1 00`) gets
  SUBSCRIBE_OK and no Objects, so no data stream is open when the runner cancels. A
  REQUEST_UPDATE without FORWARD also pauses delivery ("Pausing delivery of objects").
- **Where:** `examples/moq-pub.c` line 295 (`forward_set && forward`).
- **Required:** default FORWARD to 1 in SUBSCRIBE, and leave it unchanged in a REQUEST_UPDATE
  that omits it.

### I-06a moq-pub never answers a REQUEST_UPDATE (confirmed; example)

- **Rows:** D22-9-5-MUST-356, D22-9-5-1-MUST-357, D22-6-4-2-2-MUST-172, D22-9-9-MUST-376,
  D22-9-5-MUST-355 (not_run).
- **Scenarios:** `d22-single-request-update-response`, `d22-failed-subscription-update-cleanup`,
  `d22-established-subscription-publisher-fin`, `d22-publish-done-without-data-streams`,
  `d22-subscriber-update-on-publish`.
- **Draft:** lines 4304-4308 (exactly one REQUEST_OK or REQUEST_ERROR per update), 4320-4321,
  4395-4397 (a failed update ends the subscription with UPDATE_FAILED), 3708-3709.
- **Observed:** no reply on the SUBSCRIBE stream after `02 0004 03 01 20 64`. In
  `d22-failed-subscription-update-cleanup` the SUBSCRIBE carries no FORWARD, so delivery never
  starts; the update that must fail (USE_ALIAS of an unregistered alias,
  `02 0006 03 01 03 02 02 00`) logs "Incoming update (3) for request 1", gets no reply, and the
  runner's FIN is then taken as an unsubscribe. In the scenarios where delivery is running
  (`d22-subscriber-update-on-publish`, `d22-established-subscription-publisher-fin`,
  `d22-publish-done-without-data-streams`) an update without FORWARD logs "Pausing delivery of
  objects" (I-05). For D22-9-5-MUST-355 the TRACK_STATUS and PUBLISH_NAMESPACE legs (since R2 on
  the run's track) correctly close 0x3 (library, `src/moq.c` lines 3440-3442); the permitted
  leg on a subscription gets no answer, so the row stays unscored.
- **Where:** the library is not at fault here. It hands every REQUEST_UPDATE to the
  application's callback (`src/moq.c` lines 3513-3515), rejects it with NOT_SUPPORTED itself
  only when there is no callback (line 3518), and offers `imquic_moq_accept_request_update`
  and `imquic_moq_reject_request_update` (lines 7754, 7806). moq-pub's
  `imquic_demo_request_updated` (`examples/moq-pub.c` lines 325-340) calls neither; it only
  starts or pauses delivery by FORWARD.
- **Required (example):** accept or reject every update from the callback, and end a
  subscription whose update fails with PUBLISH_DONE UPDATE_FAILED.

### I-06b A REQUEST_UPDATE overrun is closed with the wrong code (confirmed; library)

- **Row:** D22-9-1-7-MUST-328 (not_run).
- **Scenarios:** `d22-request-update-overrun`, `d22-request-update-independent-streams` (with
  `d22-request-update-unlimited`).
- **Draft:** lines 4068-4069 (TOO_MANY_REQUEST_UPDATES).
- **Observed:** a second outstanding update against imquic's own MAX_REQUEST_UPDATES=1 closes
  0x3 "Invalid use of REQUEST_UPDATE on bidirectional request" instead of
  TOO_MANY_REQUEST_UPDATES.
- **Where:** `src/moq.c` line 3439 ("FIXME State management needs to be fixed, because an
  update will trigger OK/ERROR too") and the check at lines 3440-3442, which uses one
  PROTOCOL_VIOLATION for every rejected update.
- **Required:** close with TOO_MANY_REQUEST_UPDATES when the advertised limit is exceeded.

### I-07 Malformed or unknown input is logged and ignored (confirmed)

- **Rows:** D22-6-4-1-MUST-167, D22-6-3-MUST-156, D22-9-MUST-296, D22-8-3-MUST-251,
  D22-9-20-9-MUST-424, D22-8-3-MUST-249, D22-8-3-MUST-250 (fail); D22-11-MUST-488 (not_run).
- **Scenarios:** `d22-unknown-unidirectional-stream-type`,
  `d22-invalid-bidirectional-request-stream-opener`, `d22-message-body-length-mismatch`,
  `d22-request-message-truncated-at-fin`, `d22-setup-known-key-value-malformed-value`,
  `d22-location-filter-end-group-overflow`, `d22-setup-key-value-type-overflow`,
  `d22-setup-key-value-declared-length-overflow`, `d22-unknown-datagram-type`.
- **Draft:** lines 2707-2708, 2540-2541, 3882, 3407-3408, 5368-5369, 3377-3379, 3397-3400,
  5955-5956: each requires a session close.
- **Observed:** no close, and the liveness SUBSCRIBE is answered, after: an unknown
  unidirectional stream type `00` ("not allowed on media streams"); REQUEST_OK opening a
  bidirectional stream (`07 0001 00`); GOAWAY with Length 1 (`10 0001 00`, "Broken GOAWAY");
  a SUBSCRIBE truncated at FIN (treated as an unsubscribe); a SETUP AUTHORIZATION_TOKEN value
  `04`; a LOCATION_FILTER whose end overflows; a SETUP Delta Type past 2^64-1 ("Unsupported
  parameter '18446744073709551615'"); a SETUP option Length over 2^16-1 ("Broken SETUP",
  `src/moq.c` line 2697, after which the publisher sent no PUBLISH_NAMESPACE but still accepted
  the SUBSCRIBE). An unknown datagram type (0x132B3E2A) logs "Broken MoQ Message" and the
  window times out. The two SETUP probes were unscored in the first sweep (the adapter ran
  them publish-first and moq-pub refused the liveness SUBSCRIBE); since R3 they FAIL.
- **Required:** close with PROTOCOL_VIOLATION (or the code each cited line names) instead of
  logging.

### I-08 Native QUIC SETUP carries no AUTHORITY or PATH (confirmed)

- **Rows:** D22-6-3-2-MUST-164, D22-9-1-1-MUST-307, D22-9-1-2-MUST-314 (fail).
- **Scenarios:** `d22-native-quic-required-setup-options`, `d22-native-publisher-uri-options`.
- **Draft:** lines 2648-2649, 3943-3944, 3959-3961.
- **Observed:** the client SETUP is `af00 0016 07 12 "imquic 0.0.2/alpha" 01 01`
  (MOQT_IMPLEMENTATION and MAX_REQUEST_UPDATES only).
- **Where:** `src/moq.c` lines 280-281 ("TODO For raw quic connections, we should expose
  ways to fill in and use the PATH and ATTRIBUTE parameters as well").
- **Required:** send AUTHORITY and PATH over native QUIC. The adapter would then also need a
  way to pass the `moqt://` path and query, which moq-pub's command line lacks today.

### I-09 A server AUTHORITY is closed with INVALID_PATH (confirmed)

- **Row:** D22-9-1-1-MUST-304 (fail).
- **Scenario:** `d22-server-sends-authority`.
- **Draft:** lines 3935-3939 (an AUTHORITY option received from a server MUST close the
  session with INVALID_AUTHORITY); the code is 0x19 (line 7917).
- **Observed:** close 0x8 "AUTHORITY received from a server". A server PATH correctly gets 0x8.
- **Where:** `src/moq.c` line 2716 uses `IMQUIC_MOQ_INVALID_PATH` for the AUTHORITY case.
- **Required:** close with INVALID_AUTHORITY.

### I-10 A namespace REDIRECT with a Track Name ends the session with NO_ERROR (confirmed; library)

- **Row:** D22-9-4-1-MUST-352 (fail).
- **Scenario:** `d22-publish-namespace-redirect-nonempty-track-name`.
- **Draft:** lines 4245-4246: a REDIRECT for a namespace request with a nonempty Track Name
  is a PROTOCOL_VIOLATION.
- **Observed:** REQUEST_ERROR REDIRECT (`05 0009 34 00 00 00 01 01 6e 01 78`) on its
  PUBLISH_NAMESPACE: "Got an error announcing namespace: error 52", then close code 0.
- **Required:** close with PROTOCOL_VIOLATION. (moq-pub ends its session after any refused
  PUBLISH_NAMESPACE; the code is what is wrong here.)

### I-11 INVALID_FILTER is never sent (confirmed)

- **Rows:** D22-3-3-2-MUST-077, D22-9-1-6-MUST-326 (not_run).
- **Scenarios:** `d22-range-filter-with-zero-negotiated-limit`,
  `d22-range-filter-default-zero-limit`.
- **Draft:** lines 1501-1505, 4039-4044; INVALID_FILTER is 0x36 (line 7975).
- **Observed:** "Received 1 range filters, where 0 were allowed", then
  "imquic_moq_reject_subscribe:7632 Invalid request/state (Error)" and silence until the window
  ends.
- **Where:** `src/moq.c` lines 3402-3407 set the request state to ERROR before calling
  `imquic_moq_reject_subscribe`, which then refuses to send.
- **Required:** send REQUEST_ERROR INVALID_FILTER.

### I-12 (withdrawn) SUBGROUP_DELIVERY_TIMEOUT: not an imquic defect

- **Row:** D22-5-2-MUST-144 (fail in the final sweep, a false FAIL for that run, triage line
  A4, category a; since the runner fix `92d3c58` not_run or a genuine FAIL, depending on the
  phase of the minute, see below).
- **Scenario:** `d22-subgroup-completion-withheld-acknowledgments`.
- **Draft:** lines 2301-2307: the timer starts "once it becomes aware that all of the objects
  on the subgroup have been published"; only then must an uncommitted stream be reset
  (lines 2307-2310).
- **Observed:** SUBSCRIBE with SUBGROUP_DELIVERY_TIMEOUT 200 ms (`06 80c8`) and FORWARD 1 for
  Group 0. moq-pub's Group 0 is a one-minute clock group: after a date-prefix Object 0 it sent
  one Object per second (Objects 1 to 8, whose 2-byte payloads are the clock seconds "35" to
  "42") until the runner's 64-byte stream credit stalled the stream; its log shows Objects 9 to
  11 produced after that, and the group was still being published when the 12 s window ended.
  The timer never started, so no reset was owed.
- **Why it was listed:** the evaluator (`uncommitted_subgroup_spec` in
  `src/scenarios/draft21_contribution_residual_token.cpp`) assumes the fixture's Group 0 is
  complete (true for moqxr's fixture) and FAILs any subgroup stream still open at the end of
  the window. That was a runner item (interop-notes.md), not imquic work.
- **Resolved in the runner (`92d3c58`):** on the draft 22 wire the evaluator FAILs an open
  stream only after the publisher's PUBLISH_DONE (lines 4613-4615: sent only once every stream
  of the subscription is closed) arrived at least the timer plus 1 s before the window ended;
  otherwise it gives no verdict. moqxr `1883b9f` still passes.
- **The imquic result depends on the phase of the minute.** moq-pub's Group 0 ends when the
  wall-clock minute rolls over, and the subscription (Group 0 only) then ends with PUBLISH_DONE
  status 3 "Reached the end group" about 1 s later. The stream carries 24 + 5n bytes for n
  one-second Objects, plus a 4-byte End of Group marker, against the 64-byte credit. Re-runs of
  the scenario with the fixed runner, started at chosen seconds of the minute (scratch labels
  `FIX-` and `PHASE-490/500/510/520-d22-native` under `/tmp/claude-1000/f-sweep`):
  - Started mid-minute (Objects "11" to "18"): no PUBLISH_DONE in the window, not_run. This is
    the run behind the not_run figures below; it is what that run happened to get, not a fixed
    result.
  - Group 0 ending after 6 or 7 Objects (started at :52 and :51): marker and FIN fit in the
    credit (58 and 63 bytes), the stream finished, then PUBLISH_DONE: not_run.
  - Group 0 ending after 8 Objects or more (started at :50 and :49): the stream stalled at 64
    bytes with its end held back, PUBLISH_DONE arrived at 9.05 s and 10.06 s, and no reset came
    before the window ended at about 12 s: FAIL.
- **That FAIL is a genuine finding (confirmed).** imquic parses SUBGROUP_DELIVERY_TIMEOUT
  (`src/moq.c` lines 6514-6519) but only re-serializes it and logs it to qlog (lines 1566, 1702,
  9608); nothing starts a timer, and its only stream resets are request-stream cancellations
  with CANCELLED (lines 7296, 8215, 8391). So the library does not enforce the subgroup
  delivery timeout (lines 2301-2310), and that becomes visible whenever Group 0 is complete,
  but unacknowledged, while the window is still open. The original I-12 evidence (a group still
  being published) did not show it, which is why the item was withdrawn; the id stays
  withdrawn and this finding is recorded here.

### I-13 A duplicate Request ID is not detected (confirmed)

- **Row:** D22-6-4-2-1-MUST-169 (not_run).
- **Scenarios:** `d22-duplicate-request-id-across-streams`, `d22-duplicate-request-update-id`.
- **Draft:** lines 2733-2734.
- **Observed:** two SUBSCRIBE_NAMESPACE with Request ID 1 (`50 0003 01 00 00`,
  `50 0005 01 01 01 6d 00`) are both answered REQUEST_ERROR NOT_SUPPORTED; no
  INVALID_REQUEST_ID close.
- **Required:** close with INVALID_REQUEST_ID on a reused Request ID, before any handler runs.

### I-14 A `.session` namespace request gets NOT_SUPPORTED instead of DOES_NOT_EXIST (confirmed)

- **Row:** D22-6-5-MUST-186 (fail).
- **Scenario:** `d22-session-namespace-unknown-namespace-request`.
- **Draft:** lines 2811-2812.
- **Observed:** SUBSCRIBE_NAMESPACE for (".session", "x") is answered by the library's default
  handler, REQUEST_ERROR NOT_SUPPORTED "Not handled" (`05 000e 03 00 0b ...`).
- **Where:** `src/moq.c` line 3824 (default when the application registers no
  SUBSCRIBE_NAMESPACE callback; moq-pub registers none).
- **Required:** answer DOES_NOT_EXIST for an unknown `.session` namespace, in the library.

### I-15 A request is answered before the peer's SETUP is complete (observed; MAY row, low)

- **Row:** D22-6-3-MAY-159 (not_run).
- **Scenario:** `d22-request-stream-before-peer-setup`.
- **Draft:** lines 2549-2553 (an endpoint SHOULD buffer such a request).
- **Observed:** with only the first SETUP byte (`af`) delivered, imquic answers the early
  SUBSCRIBE with SUBSCRIBE_OK before the rest (`000000`) arrives ("imquic_read_moqint:9373
  Invalid moqint (2 > 1)" is logged for the partial SETUP). It neither buffered nor reset, so the
  MAY row is inconclusive.
- **Question:** should the library hold request streams until SETUP has been parsed?

### I-16 An absolute LOCATION_FILTER start is not honored (observed; example)

- **Rows:** none (the row this scenario scores, D22-6-3-MUST-NOT-160, passes).
- **Scenario:** `d22-control-stream-lifetime` (SUBSCRIBE `... 02 10 01 11 02 07 09`: FORWARD 1,
  absolute start Group 7 Object 9).
- **Observed:** "Starting delivery of objects: [0/9] --> [...]": delivery starts in Group 0,
  not at Group 7.
- **Where:** `examples/moq-pub.c` lines 276-281. For an absolute start moq-pub adjusts the group
  only `if(group_id >= start.group)`; with the current group 0 and start 7 that branch is
  skipped, the static default start group 0 (line 55) is kept, and only the object (9) is
  taken from the filter.
- **Required:** honor an absolute start that lies in the future. Not scored by any row; fix with
  moq-pub's own tests.

### I-17 REQUEST_ERROR 0x19 DUPLICATE_SUBSCRIPTION is not a draft 22 code (observed; example)

- **Rows:** none directly; it is how moq-pub refuses every SUBSCRIBE with `-X` and every second
  SUBSCRIBE once delivery started, which keeps the one-subscriber rows unscored (triage D-sub1).
- **Draft:** lines 7928-7980 list the draft 22 REQUEST_ERROR codes; 0x19 is not among them.
- **Where:** `examples/moq-pub.c` lines 216-220; the enum marks it "Deprecated in v19"
  (`src/imquic/moq.h` line 991).
- **Question:** which draft 22 code should a single-subscriber publisher use (EXCESSIVE_LOAD
  0x9 or INTERNAL_ERROR)?

### I-18 SUBSCRIBE_OK is always `04 0002 00 00` (question; example)

- **Rows:** none.
- **Draft:** lines 4494-4510 (SUBSCRIBE_OK: Track Alias, parameters EXPIRES and LARGEST_OBJECT,
  Track Properties).
- **Observed:** every SUBSCRIBE_OK in both sweeps carries Track Alias 0 and no parameters, also
  after Objects were published.
- **Question:** does draft 22 require LARGEST_OBJECT once Objects exist (the row
  D22-9-20-17-MUST-441 could not be judged, triage D-sub1), and is Track Alias 0 for every
  subscription intended (moq-pub's `-t` default)?

### I-19 The AUTHORIZATION_TOKEN of a request is never decoded (confirmed; new)

- **Rows:** D22-8-9-MUST-279, D22-8-9-MUST-281, D22-8-9-MUST-289 (fail).
- **Scenarios:** `d22-request-undecodable-authorization-token`,
  `d22-request-unknown-token-alias` (with `d22-request-deleted-token-alias`),
  `d22-request-token-cache-overflow`, `d22-request-alias-registration-with-default-zero-cache`.
- **Draft:** lines 3704-3705 (undecodable Token: close KEY_VALUE_FORMATTING_ERROR), 3708-3709
  (unregistered Alias: reject with UNKNOWN_AUTH_TOKEN_ALIAS), 3760-3765 (registration over
  MAX_AUTH_TOKEN_CACHE_SIZE: close AUTH_TOKEN_CACHE_OVERFLOW; imquic announces no cache, so the
  limit is 0).
- **Observed:** SUBSCRIBEs for media/vide_1 with token `03 01 03` (undecodable), `03 02 02 00`
  (USE_ALIAS 0, never registered), `03 04 01 00 00 78` and `03 04 01 00 80 9d` (REGISTER) are
  all answered SUBSCRIBE_OK `04 0002 00 00`; no close, and the liveness SUBSCRIBE is answered.
- **Confounding history:** in the first sweep these probes named namespace () and track "x";
  imquic closed that with 0x3 (I-03) before it read the token, and the same rows FAILed for that
  reason. Since R2 the probes name the run's track and the FAILs are imquic's own behavior.
- **Where:** `src/moq.c` lines 6486-6500 copy the token bytes into `params->auth_token` without
  decoding Alias Type, Alias or Token Type.
- **Required:** decode the Token structure and apply the three rules (the cache limit included).

### I-20 GROUP_ORDER 0 is accepted (confirmed; new)

- **Row:** D22-9-20-8-MUST-421 (fail).
- **Scenarios:** `d22-group-order-zero` (with `d22-group-order-above-two`,
  `d22-fill-invalid-group-order`).
- **Draft:** lines 5256-5260: allowed values 0x1 and 0x2; anything else is a
  PROTOCOL_VIOLATION.
- **Observed:** SUBSCRIBE with `22 00` gets SUBSCRIBE_OK and the liveness SUBSCRIBE is answered.
  GROUP_ORDER 3 is closed 0x3 correctly. In the first sweep the zero-field namespace close (I-03)
  made this row a false pass.
- **Where:** `src/moq.c` line 6557 rejects only `group_order > IMQUIC_MOQ_ORDERING_DESCENDING`.
- **Required:** reject 0 as well at draft 22.

### I-21 Parameters are accepted outside their message scope (confirmed; new)

- **Row:** D22-9-20-1-MUST-396 (fail).
- **Scenarios:** `d22-parameter-invalid-message-scope`, `d22-fill-timeout-outside-fill-or-fetch`
  (with `d22-group-order-in-subscription-update`).
- **Draft:** lines 5126-5129: a parameter in a message type its definition does not allow
  closes the connection with PROTOCOL_VIOLATION.
- **Observed:** SUBSCRIBE with EXPIRES (`08 01`) or FILL_TIMEOUT (`0a 00`) is accepted with
  SUBSCRIBE_OK. GROUP_ORDER in a subscription REQUEST_UPDATE is accepted silently.
- **Where:** `imquic_moq_parse_request_parameter` (`src/moq.c` lines 6448-6800) parses every
  known type for every message; there is no per-message allow list.
- **Required:** check each parameter against the message it arrived in.

### I-22 Duplicate parameters are accepted (confirmed; new; SHOULD, low)

- **Row:** D22-9-20-SHOULD-393 (not_run: an advisory row records no `fail`).
- **Scenario:** `d22-unexpected-duplicate-message-parameter`.
- **Draft:** lines 5104-5108: receivers SHOULD close with PROTOCOL_VIOLATION on unexpected
  duplicates.
- **Observed:** SUBSCRIBE with FORWARD twice (`10 00 00 00`, the second as Delta Type 0) gets
  SUBSCRIBE_OK; no close in the window. In the first sweep the I-03 close made this row a false
  pass.
- **Where:** the loop in `imquic_moq_parse_request_parameters` (`src/moq.c` lines 6833-6843)
  never checks for a repeated type.
- **Required (SHOULD):** close on a repeated parameter that its definition does not allow.

### I-C1 FILL_PARAMETERS: a bare parameter sequence or a count first? (expectation question)

- **Rows:** D22-3-4-1-MUST-079, -080, -081 (not_run). It also confounds results that look
  fine: D22-9-20-15-MUST-432 passes, and the fill legs of D22-9-20-8-MUST-421 and
  D22-9-20-9-MUST-424 close 0x3, only because of this parse error, not because imquic judged
  the nested parameter.
- **Scenarios:** `d22-cancel-subscription-with-concurrent-fill-streams`,
  `d22-fill-fails-before-first-object`, every `d22-fill-*` probe.
- **Observed:** FILL_PARAMETERS is Parameter Type 0x23; on the wire it is a Type Delta. In
  `d22-cancel-subscription-with-concurrent-fill-streams` the parameters are
  `02 10 01 13 02 21 00`: two parameters, FORWARD 1, then delta 0x13 (0x10 + 0x13 = 0x23),
  Length 2, and the value `21 00`, one nested LOCATION_FILTER with no count before it. (Where
  FILL_PARAMETERS is the first parameter, the delta is `23`.) imquic reads 0x21 as a Number of
  Parameters and closes 0x3 "Broken MoQ request parameter" (moq.c lines 6460 and 6778 in every
  fill probe's log).
- **The two readings:**
  - Runner and moqxr: the value is a bare sequence of Message Parameters. The value is already
    length-prefixed (lines 5478-5479); Figure 24 (lines 5053-5058) defines a Message Parameter
    as Type Delta plus Value, and the Number of Parameters is a field of each message layout
    (for example lines 4498-4499), not of the parameter encoding; "encoded as if they were
    Parameters for a separate message" (lines 5480-5482) then means a fresh Type Delta base and
    a separate scope (line 5527: "The value of FILL_PARAMETERS is a separate parameter scope").
    The "(see Section 16.7)" in that sentence points to the IANA Message Parameters registry
    table (line 7600), that is to a separate Type Delta base and scope, not to a message
    body.
    moqxr `4b615f4` parses it this way (`validate_fill_parameters`,
    `moqt_control_messages.cpp` lines 1496-1530 in the scratch copy of that revision).
  - imquic: a parameter block is preceded by its count. Lines 5089-5090 say that, because
    unknown parameters cannot be skipped, "the block is bounded by a parameter count rather
    than a length", and a separate message's parameters start with a Number of Parameters.
    `src/moq.c` lines 6758-6763 read a count first.
- **Question for both sides:** does "encoded as if they were Parameters for a separate message"
  include the Number of Parameters? This list does not declare imquic wrong. Settle the reading
  (ideally with the draft authors) before changing either the runner or imquic; until then the
  fill rows above say nothing about imquic.

## Ground rules

1. **The draft decides.** `docs/draft-ietf-moq-transport-22.txt` in this repository is the
   only authority. Read the cited lines before changing anything.
2. **The runner is not the authority either.** If, after reading the cited lines, a finding
   looks wrong (the stimulus, the expected code, or the reading of the text), do not change
   imquic to satisfy it; record the dispute with the draft quotation and leave the item open.
   The runner has been wrong before: the first imquic sweep found three runner and adapter
   defects (R1 to R3).
3. **moq-pub is a demo publisher.** It publishes one clock track (one Object per second, one
   Group per minute), serves one subscriber at a time, has no FETCH, SUBSCRIBE_NAMESPACE or
   SUBSCRIBE_TRACKS handling of its own and announces no token cache or filter ranges. Rows
   that need those (triage lines D-clock, D-pad, D-disc, D-sub1, D-tok, D-pub in
   interop-notes.md) are not imquic work for this list. Library items (I-01, I-02, I-03, the
   missing size check of I-04, I-06b, I-07 to I-11, I-13 to I-15, I-19 to I-22) apply to every
   imquic application; example items (the crash of I-04, I-05, I-06a, I-16 to I-18) are
   moq-pub's.
4. **Do not hide a failure by weakening a check.** Fix the code, add imquic tests, and re-run
   the scenarios named in the item.

## How to reproduce a finding

Build the runner (see [building-and-running.md](building-and-running.md)) and imquic with
`./configure --enable-moq-examples && make`, then start the runner in driven mode with the
bundled imquic adapter. moq-pub has no FETCH, so declare the publisher cache-less.

```sh
R=/path/to/moq-contribution-interop-runner       # this repository, built
I=/path/to/imquic                                # imquic, built
T=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/key.pem -out $T/cert.pem \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -days 1

IMQUIC_PUB_BIN=$I/examples/imquic-moq-pub $R/build/moq-interop-runner \
  --bind 127.0.0.1 --port 19811 --database $T/runs.sqlite3 \
  --docs $R/docs --requirements $R/requirements \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 19812 --publisher-port-end 19819 \
  --tls-cert $T/cert.pem --tls-key $T/key.pem \
  --driver-executable $R/adapters/imquic/run.sh \
  --driver-fixture /path/to/locmaf-publisher.mp4 \
  --driver-log-root $T/logs \
  --publisher-no-fetch &

curl -s -X POST http://127.0.0.1:19811/api/v1/runs -H 'Content-Type: application/json' -d '{
  "draft": 22, "transport": "native-quic", "mode": "driven",
  "scenarios": ["d22-group-order-zero"], "timeout_ms": 12000,
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}}'
# then: GET /api/v1/runs/<id>            outcomes per requirement
#       GET /api/v1/runs/<id>/events?limit=100   wire evidence
# moq-pub's output is under $T/logs/<id>/<n>-<scenario>/publisher.log
```

moq-pub reads no fixture; the sweeps passed moqxr's `tests/fixtures/locmaf-publisher.mp4`
to `--driver-fixture`, and the adapter ignores it. A row that names several scenarios is
scored only when all of them run in one request (the catalog entry in
`requirements/draft22.json` lists them).
