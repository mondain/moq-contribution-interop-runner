# moqxr Punch List

This is a work list for an agent fixing `openmoq-publisher` (moqxr) so that it
conforms to MoQT drafts 18, 21 and 22. Every M- item comes from a run of the interop
runner in this repository against `openmoq-publisher 0.4.1 (commit 9bda5c9)`, with
the evidence and draft citation recorded below. The item bodies keep that original
evidence; the status tables that follow record what later sweeps against moqxr
`0993cf7` and `4b615f4` showed. The D22- items come from the first draft 22 sweep,
against `4b615f4`, its re-sweep after the F1 runner fixes, and a dig into every draft 22
failure against `1883b9f` (see "Status against moqxr 1883b9f"). Background and the full
result set are in [interop-notes.md](interop-notes.md).

## Status against moqxr 0993cf7

A full sweep (about 760 driven runs) against `0993cf7` (`0.4.1-dev`) was compared with
the 0.4.1 sweep. "Pass" means a row now passes. "No verdict" means it no longer fails
but the sweep recorded no pass either (the run ended `incomplete`), so the fix is not
confirmed. An item body below describes the 0.4.1 behavior unless it says otherwise.

| Item | Status on 0993cf7 | Rows still failing |
|------|-------------------|--------------------|
| M-01 duplicate unknown SETUP options | Fixed for draft 18 (4 rows pass); D21-13-MUST-593 has no verdict | none |
| M-02 malformed namespaces and names | Draft 18 fixed; draft 21 open | D21-8-7-MUST-251 |
| M-03 SUBSCRIBE_NAMESPACE 32-field limit | Draft 18 fixed; draft 21 open | D21-9-15-MUST-383 |
| M-04 DOES_NOT_EXIST code | Fixed (7 rows pass) | none |
| M-05 namespace prefix overlap | Draft 18 fixed; D21-9-15-MUST-385 has no verdict | none |
| M-06 duplicate SUBSCRIBE | Fixed | none |
| M-07 FIRST_OBJECT | Fixed | none |
| M-08 server SETUP AUTHORITY or PATH | Fixed (4 rows pass) | none |
| M-09 unknown messages and stream types | Mostly fixed (8 of 10 rows pass or have no failure) | D18-10-4-MUST-007, D18-10-MUST-008 |
| M-10 AUTHORIZATION TOKEN structure | Draft 18 fixed; draft 21 open | D21-8-9-MUST-267, D21-8-9-MUST-277 |
| M-11 Range Filter without MAX_FILTER_RANGES | Open | D21-3-3-2-MUST-065, D21-9-1-6-MUST-315 |
| M-12 nested FILL_PARAMETERS | Open | D21-9-20-9-MUST-429, D21-9-20-10-MUST-432, D21-9-20-16-MUST-447 |
| M-13 REQUEST_UPDATE_OK LARGEST_OBJECT | No verdict | none |
| M-14 Track Properties tolerated | Unscored (timeout artifact, see interop-notes) | none |
| M-15 REDIRECT and oversized REQUEST_ERROR | Fixed (3 rows pass) | none |
| M-16 Request-ID checks | No work needed; still passing | none |
| M-17 unsupported request types | Partly fixed: FETCH and TRACK_STATUS now get NOT_SUPPORTED. See the note under M-17 | D18-10-12-2-MUST-004 |
| M-18 WebTransport datagram validation | No failing rows; D18-11-4-2-MUST-002 unscored | none |
| M-19 control-stream GOAWAY URI | No failing rows; not scored by any sweep run | none |
| M-20 rejected announcement ends the session | No failing rows; D18-14-MUST-004 not scored | none |
| M-21 unknown FETCH Type | New, open | D18-10-12-MUST-001 |

D18-10-12-2-MUST-004 failed in the sweep because the sweep did not declare the
publisher cache-less. Run with `--publisher-no-fetch` (or `"publisher_capabilities":
{"fetch": false}`) and the FETCH-dependent rows become not applicable instead of
failing. Re-check this row with the declaration before treating it as a moqxr defect.

## Status against moqxr 4b615f4

The draft 21 and draft 22 sweep of 2026-10-06 against `4b615f4` (`0.4.2-dev`, "Add moqt
draft 22") re-checked the items below; the method and the full triage are in
[interop-notes.md](interop-notes.md#draft-22-sweep-against-moqxr-4b615f4). Draft 18 was
not swept, so draft 18-only items keep their `0993cf7` status. Several draft 21 rows
still fail at draft 21 only because the bundled adapter runs their probes with
`--forward 1` (moqxr then blocks on its own PUBLISH and closes with code 0 inside the
reaction window); draft 21 command lines are frozen, the draft 22 twins run paced and
pass, so those draft 21 FAILs are adapter artifacts, not moqxr defects.

| Item | Status on 4b615f4 | Evidence |
|------|-------------------|----------|
| M-01 duplicate unknown SETUP options | No verdict (unchanged) | D21-13-MUST-593 and its twin D22-13-MUST-568 stay not_run (see D22-C1) |
| M-02 malformed namespaces and names | Fixed | D22-8-7-MUST-269 to -272 pass (close 0x3); D21-8-7-MUST-251 to -254 fail only through the adapter flag artifact and pass with paced flags |
| M-03 SUBSCRIBE_NAMESPACE 32-field limit | Fixed for draft 21 | D21-9-15-MUST-383 passes with paced flags (the bundled-adapter FAIL is the flag artifact); draft 22 has no such row |
| M-05 namespace prefix overlap | Fixed for SUBSCRIBE_NAMESPACE | D22-4-2-MUST-111 passes (PREFIX_OVERLAP). SUBSCRIBE_TRACKS overlap is not detected: D22-09 |
| M-08 server SETUP AUTHORITY or PATH | Fixed (unchanged) | D22-9-1-1-MUST-304, D22-9-1-2-MUST-311 pass; -305 and -312 pass over WebTransport |
| M-09 unknown messages and stream types | Partly fixed | D22-6-4-1-MUST-167 (unknown unidirectional stream type) passes; D22-9-MUST-295 (unknown control message) fails: D22-02 |
| M-10 AUTHORIZATION TOKEN structure | Fixed | D22-8-9-MUST-279 (close 0x6) and -289 (close 0x13) pass; D21-8-9-MUST-267 and -277 fail only through the flag artifact |
| M-11 Range Filter without MAX_FILTER_RANGES | Open | D22-3-3-2-MUST-077, D22-9-1-6-MUST-326 fail (and the draft 21 twins): D22-01 |
| M-12 nested FILL_PARAMETERS | Fixed | D22-9-20-8-MUST-421, D22-9-20-15-MUST-432 and D22-9-20-9-MUST-424 pass; the draft 21 FAILs are the flag artifact |
| M-13 REQUEST_UPDATE_OK LARGEST_OBJECT | Fixed | D21-9-20-18-MUST-456 and D22-9-20-17-MUST-441 pass |
| M-14 Track Properties tolerated | Unscored (unchanged) | D22-9-3-MUST-348, D22-9-5-MUST-355 not_run |
| M-15 REDIRECT and oversized REQUEST_ERROR | Fixed (unchanged) | D22-9-4-1-MUST-352, D22-8-5-MUST-266 pass |
| M-18 WebTransport datagram validation | Open (by source) | D22-11-MUST-488 passes on native QUIC; its WebTransport run is confounded by the adapter's `--forward 1`, so the evidence is moqxr's source: D22-07 |
| M-19 control-stream GOAWAY URI | Changed: now a PROTOCOL_VIOLATION close | D22-04 |
| M-20 rejected announcement ends the session | Still observed | A refused PUBLISH (GREASE code) ends the session with code 0: D22-C1 |

M-04, M-06, M-07, M-16, M-17 and M-21 were not re-checked (draft 18 rows, or FETCH,
which the sweep declared away).

After the F1 runner fixes (29 shared probes send the run's namespace and track on the draft
22 wire; see [interop-notes.md](interop-notes.md#draft-22-sweep-against-moqxr-4b615f4)) the
same build was swept again on both transports. One row changed: `D22-8-9-MUST-281` went from
`not_run` to `fail`, because a SUBSCRIBE for the run's track that uses an unregistered token
alias is served (D22-10). Every status above is unchanged.

## Status against moqxr 1883b9f

moqxr `main` moved after the sweeps above. On 2026-10-06 the checkout was at
`1883b9febe35c4173f3a8e6ccf439cfdf0d913ae` (`v0.4.3-2-g1883b9f`, `--version` `0.4.3-dev`), nine
commits after `4b615f4`. Those commits change only `src/transport/moqt_session.cpp` (a wait for a
forward=1 REQUEST_UPDATE in the `--forward 1` file-publish path, and a split of multi-traf moof
boxes in live stdin ingest) and the CMAF segmenter; `moqt_control_messages.cpp`,
`picoquic_client.cpp` and `webtransport_client.cpp` are byte-identical, and `moqt_session.cpp` is
identical up to line 5731. A scratch build of `1883b9f` was swept at draft 22 on both transports
with the runner at `ef47f8b` and the bundled adapter unchanged, by the method of the `4b615f4`
sweep (every executable id as a single run, then the 41 row-completion groups).

Result: no row changed. Every run's verdict and pass and fail counts, and every row outcome, equal
the `4b615f4` re-sweep after the F1 fixes: native QUIC 69 pass, 6 fail, 72 not_run; WebTransport
69 pass, 6 fail, 69 not_run. Nothing was fixed upstream and nothing regressed. The dig then read
moqxr's code for every failure and every non-pass row classed as a moqxr question, and ran the four
adapter-mode experiments listed below. The D22- items now carry the source location, a repro, the
severity, a fix direction and a confidence label.

| Item | Status on 1883b9f | Dig result |
|------|-------------------|------------|
| D22-01 Range Filter error code | Open | Confirmed: the SUBSCRIBE decoder has no Range Filter branch |
| D22-02 unknown control message | Open | Confirmed; cause corrected: the framing helper does not know the type (D22-11), not a silent erase while serving |
| D22-03 padding stream closes | Open | Confirmed; cause found: the session's integer decoder stops at 4-byte integers |
| D22-04 control-stream GOAWAY | Open | Confirmed by source |
| D22-05 REQUEST_UPDATE then close | Refined | The close is caused by the TRACK_STATUS that follows the update (D22-11); the REQUEST_UPDATE is not shown to be at fault. The intermittent "truncated REQUEST_UPDATE" close has its own likely cause |
| D22-06 silence | Refined | Three legs confirmed by source (still unscored); the TRACK_STATUS leg is D22-11 |
| D22-07 WebTransport datagrams | Open | A paced WebTransport run now reaches the datagram: no close in 12 s, as the source predicts |
| D22-08 second GOAWAY on SUBSCRIBE_NAMESPACE | Open | Confirmed by source |
| D22-09 SUBSCRIBE_TRACKS overlap | Open | Confirmed by source |
| D22-10 unregistered token alias | Open | Confirmed by source |
| D22-11 message framing knows a fixed list of types | New | Root cause of D22-02, of the D22-05 close and of one D22-06 leg; also leaves `D22-9-10-MUST-381` unscored |
| D22-C1 refused PUBLISH ends the session | Unchanged | Not re-examined |

Adapter-mode experiments (scratch copy of the adapter, `1883b9f`, native QUIC unless noted). None
changed a verdict, so `adapters/moqxr/run.sh` is unchanged:

| Scenario | Bundled options | Tried | Result |
|----------|-----------------|-------|--------|
| `d22-unknown-request-stream-message` | `--forward 1` | `--forward 0 --paced`, timeout+3 | Still `not_run`, now for moqxr's behavior: it answers the SUBSCRIBE (`04 0002 01 00`), serves every Object and PUBLISH_DONE, and ignores the trailing `7e 00 00` for 12 s (D22-11). The probe has no liveness follow-up, so silence is not scored. With `--forward 1` moqxr never answered the request: it waited 2 s on its own PUBLISH for `catalog`, reset and closed with code 0. `D22-9-MUST-295` stays `fail` through the control-stream scenario |
| `d22-unknown-datagram-type` | `--forward 1` | `--forward 0 --paced`, timeout+3 | Native QUIC: `D22-11-MUST-488` passes either way. WebTransport: still `not_run`; the paced run delivers the datagram (`f0 13 2b 3e 2a`) and moqxr neither closes nor reacts for 12 s, which matches D22-07. Silence after a datagram is not scored |
| `d22-publish-established-subscriber-sends-publish-state-notify` | `--forward 0 --paced`, timeout+3 | `--forward 1` | Worse: an `error` run. moqxr's first PUBLISH is for `catalog` (`1d 0012 02 01 05 media 07 catalog 00 00`); the probe waits for a PUBLISH of the run's track, so nobody answers, and moqxr resets the stream after 2 s and closes with code 0. `D22-9-10-MUST-381` stays `not_run` |
| `d22-subscribe-tracks-publish-skipped-then-capacity-recovers` | `--forward 0` | `--forward 1` | Still `not_run`. The probe grants the publisher one bidirectional stream; moqxr opens its PUBLISH_NAMESPACE on it, sends no PUBLISH in either mode and resets that stream after about 2 s. moqxr has no PUBLISH_SKIPPED message at all (no encoder for it), so the condition the row constrains never arises: `D22-3-6-3-MUST-NOT-086` is not applicable to moqxr, not adapter-hidden |

## Draft 22 items

From the same sweep, with draft 22 rows and line numbers in
`docs/draft-ietf-moq-transport-22.txt`. They follow the ground rules above; read the
cited lines first. Items that carry an M- item over to draft 22 say so.

Source locations (the **Where** lines) are from the scratch copy of moqxr `1883b9f` that the
dig ran; `moqt_session.cpp` up to line 5731 and all of `moqt_control_messages.cpp`,
`picoquic_client.cpp` and `webtransport_client.cpp` are byte-identical in `4b615f4`, so these
numbers hold for both revisions. The read-only checkout can move, so its line numbers can differ.
Wire bytes are as the runner recorded them (draft 22 control messages: type, 16-bit length,
body). **Repro** names the scenarios to run together: start the runner as in "How to reproduce a
finding" below and post them with `"draft": 22, "transport": "native-quic"` (or
`"webtransport"`), `"timeout_ms": 12000` and the reference track; the bundled adapter picks
moqxr's options. **Confidence:** confirmed (wire evidence and the code that produces it),
likely (one of the two), suspected (neither settles it).

### D22-01 A Range Filter with no advertised MAX_FILTER_RANGES gets UNAUTHORIZED (carries M-11)

- **Rows:** D22-3-3-2-MUST-077, D22-9-1-6-MUST-326 (fail).
- **Scenarios:** `d22-range-filter-with-zero-negotiated-limit`,
  `d22-range-filter-default-zero-limit`.
- **Draft:** lines 1501-1505 and 4039-4044: with no MAX_FILTER_RANGES advertised the
  limit is zero, and a Range Filter must be rejected with INVALID_FILTER (0x36, line 7975).
- **Observed:** the SUBSCRIBE `03 0015 01 01 05 media 06 vide_1 01 26 03 00 01 00` (one
  OBJECTID_FILTER, type 0x26) is answered with REQUEST_ERROR code 0x1 (UNAUTHORIZED), reason
  "invalid SUBSCRIBE" (`05 0014 01 00 11 ...`, with FIN). The same on `1883b9f`.
- **Where:** `decode_subscribe_message` (`moqt_control_messages.cpp` lines 1643-1742) has no
  branch for the Range Filter types 0x25-0x29, so the parameter falls to the "unknown parameter"
  `return false` (lines 1687-1691 for even types, 1733-1737 for odd). The session then answers
  every undecodable SUBSCRIBE with REQUEST_ERROR 0x1 and closes with PROTOCOL_VIOLATION
  (`moqt_session.cpp` lines 4256-4263; the runner has its verdict before the close). SUBSCRIBE_TRACKS
  already does this right: `decode_subscribe_tracks_message` skips a well-framed Range Filter and
  sets `has_unnegotiated_range_filter` (lines 1784-1795), and the session answers INVALID_FILTER
  (`moqt_session.cpp` lines 4492-4507).
- **Repro:** `d22-range-filter-with-zero-negotiated-limit` alone (row 077 also names
  `d22-range-filter-total-exceeds-negotiated-limit`, which waits for an advertised limit and
  sends nothing).
- **Severity:** MUST (both rows). A subscriber that sends a Range Filter by mistake gets a
  misleading UNAUTHORIZED and loses the session instead of a retryable INVALID_FILTER.
- **Fix direction:** mirror the SUBSCRIBE_TRACKS handling in `decode_subscribe_message` (skip a
  well-framed 0x25-0x29 parameter and flag it), then answer INVALID_FILTER (0x36) on the request
  stream without closing the session.
- **Confidence:** confirmed.
- **Required:** use INVALID_FILTER for a filter over the negotiated limit.

### D22-02 An unknown control message type is skipped (carries M-09)

- **Row:** D22-9-MUST-295 (fail).
- **Scenario:** `d22-unknown-control-message`.
- **Draft:** lines 3877-3878: "An endpoint that receives an unknown message type MUST
  close the session."
- **Observed:** the runner writes its SETUP and then `7e 00 00` on its control stream (draft
  22: the unidirectional stream that starts with SETUP), right after moqxr's SETUP and before the
  runner's first request. moqxr never closes; 500 ms later it answers the liveness SUBSCRIBE
  (`03 0010 07 01 05 media 06 vide_1 00`) with SUBSCRIBE_OK (`04 0002 01 00`), serves all four
  Objects and PUBLISH_DONE (`0b 0003 02 02 00`). The same on `1883b9f`.
- **Where (corrected by the dig):** the file publisher (`--input` with an MP4) serves through
  `serve_subscriptions`, whose control loop (`moqt_session.cpp` lines 4636-4702) would close with
  PROTOCOL_VIOLATION on any message it does not handle (lines 4689-4692, "received unknown or
  unsupported control-stream message", which is why the GOAWAY of D22-04 closes). The loop never
  sees 0x7e: it extracts messages with `next_control_message` (`moqt_control_messages.cpp` lines
  664-757), which knows a fixed list of types and returns "incomplete" for any other type (line
  755-756). `7e 00 00` therefore stays in the buffer as an unfinished message forever, and every
  later control message queues behind it. This is D22-11. The earlier statement that a
  "while serving" loop around lines 10462-10533 erases unknown messages was wrong: those lines are
  in `publish_live_objects`, the DASH live path, which the file publisher does not run.
- **Repro:** `d22-unknown-control-message` (row 295 also names
  `d22-unknown-request-stream-message`; run both for the row). By hand: after SETUP, write
  `7e 00 00` on the control stream, wait 500 ms, then send a valid SUBSCRIBE on a new
  bidirectional stream: it is served.
- **Severity:** MUST (3877-3878). The session survives, but its control stream is wedged: any
  later control message (a GOAWAY, for instance) is never read.
- **Fix direction:** fix D22-11 (frame every draft 22 message by its 16-bit length), after which
  the existing close at lines 4689-4692 applies to unknown types.
- **Confidence:** confirmed.
- **Required:** close the session on an unknown control message type.

### D22-03 A padding stream closes the session

- **Row:** D22-11-5-1-MUST-541 (not scored: the runner judges this row by the liveness
  follow-up only).
- **Scenario:** `d22-inbound-padding-stream`.
- **Draft:** lines 6633-6647: an endpoint MAY open a padding stream (type 0x132B3E28);
  "The receiver MUST discard all data received on a padding stream".
- **Observed:** moqxr logs "received unknown or malformed unidirectional stream type" and
  closes with PROTOCOL_VIOLATION.
- **Evidence:** the padding stream starts `f0 13 2b 3e 28` (the 5-byte vi64 for 0x132B3E28,
  draft lines 3253-3280: 28 bits fit in 4 bytes, this type needs 29) and ends with FIN; moqxr
  closes 0x3 at once. The same on `1883b9f`.
- **Where (cause found by the dig; the earlier partial-read hypothesis is withdrawn):**
  `is_known_peer_unidirectional_stream_type` accepts 0x132b3e28 (`moqt_session.cpp` line
  1571), but the type never gets there. The prefix is decoded with `decode_moqint`
  (lines 4204-4207), which at drafts 18 and later calls the session's own `decode_vi64` (lines
  574-605); that decoder knows only the 1- to 4-byte forms and returns false for a first byte of
  0xf0 or more (lines 593-594). The decoder in `moqt_control_messages.cpp`
  (`decode_vi64_impl`, lines 326-370) handles all nine forms. Any session-level read of a value
  of 2^28 or more (stream types, Request IDs, parameter types) fails the same way.
- **Repro:** `d22-inbound-padding-stream`. By hand: open a unidirectional stream to moqxr
  and write `f0 13 2b 3e 28` followed by any bytes.
- **Severity:** MUST (6646-6647). A peer that pads loses the session; the row itself stays
  unscored because its evaluator judges only the liveness follow-up.
- **Fix direction:** make `decode_vi64` in `moqt_session.cpp` decode the 5- to 9-byte forms
  (or call the full decoder), then drain and discard padding streams.
- **Confidence:** confirmed.
- **Required:** accept and discard padding streams.

### D22-04 A control-stream GOAWAY with a New Session URI closes the session (carries M-19)

- **Row:** D22-9-2-MUST-340 (not_run; the run ends `error`).
- **Scenario:** `d22-publisher-goaway-alternate-uri`.
- **Draft:** lines 4127-4130: "The client MUST use this URI for the new session if
  provided."
- **Observed:** after SUBSCRIBE_OK, the GOAWAY
  `10 0021 1f moqt://127.0.0.1:<port>/moq-next 00` on the control stream draws "received
  unknown or unsupported control-stream message" and close 0x3; moqxr exits. The same on
  `1883b9f`.
- **Where:** the `serve_subscriptions` control loop handles only REQUEST_UPDATE (and, before
  draft 18, SUBSCRIBE and SUBSCRIBE_NAMESPACE) and closes on every other complete message at
  drafts 18 and later (`moqt_session.cpp` lines 4684-4692). GOAWAY is framed by
  `next_control_message`, so it reaches that close. A GOAWAY on a request stream is handled
  (`send_request_stream_and_wait`, lines 1403-1414, resets the request and reports a retryable
  failure), but not one on the control stream.
- **Repro:** `d22-publisher-goaway-alternate-uri`.
- **Severity:** MUST (4129-4130) for the migration; closing with PROTOCOL_VIOLATION on a valid
  GOAWAY is also wrong in itself. Low interop impact while relays rarely migrate publishers.
- **Fix direction:** decode GOAWAY in the control loop, stop new requests, and reconnect to the
  New Session URI (or the current one if empty) after the current work drains.
- **Confidence:** confirmed.
- **Required:** decode GOAWAY on the control stream and reconnect to the given URI. Lowest
  priority, as M-19.

### D22-05 A valid REQUEST_UPDATE on an accepted SUBSCRIBE is followed by close 0x3 (refined: the TRACK_STATUS causes the close)

- **Rows:** D22-9-5-MUST-356, D22-9-5-1-MUST-363 (not_run).
- **Scenarios:** `d22-single-request-update-response`,
  `d22-coalesced-successful-update-responses`, `d22-coalesced-failed-update-response`.
- **Draft:** lines 4304-4308: "The receiver of a REQUEST_UPDATE MUST respond with exactly
  one REQUEST_UPDATE_OK or REQUEST_UPDATE_ERROR"; coalescing at 4406-4408.
- **Observed:** after SUBSCRIBE_OK the runner sends REQUEST_UPDATE (`02 0004 03 01 20 64`,
  SUBSCRIBER_PRIORITY 100; three for the coalesced probe) on the SUBSCRIBE stream and then a
  TRACK_STATUS with FIN on a new request stream
  (`0d 0010 05 01 05 media 06 vide_1 00`, FIN; Request ID 9 in the coalesced probes). moqxr sends
  no REQUEST_OK and closes 0x3 "request stream closed before a complete message" in all three
  scenarios. The same on `1883b9f`.
- **Where (the dig settles which request closes the session):** the TRACK_STATUS. The serve
  loop accepts the new request stream and reads it with `read_request_stream_message`
  (`moqt_session.cpp` lines 4232-4244, 3520-3558), which waits for `next_control_message` to
  report a complete message. `next_control_message` does not list TRACK_STATUS (0x0D)
  (`moqt_control_messages.cpp` lines 671-756, D22-11), so the complete TRACK_STATUS looks
  incomplete, and the FIN produces the "request stream closed before a complete message"
  PROTOCOL_VIOLATION (line 3555). The NOT_SUPPORTED answer that moqxr has for TRACK_STATUS
  (`moqt_session.cpp` lines 4443-4459) is unreachable for that reason. In each pass of the
  serve loop the new-stream accept (lines 4213-4244) runs before the retained-stream update
  reader (lines 4553-4629), so when the update and the TRACK_STATUS arrive together the session
  closes before the update is read; the REQUEST_UPDATE is not shown to be at fault. moqxr does answer a REQUEST_UPDATE on a
  SUBSCRIBE when nothing else intervenes (REQUEST_OK `07 0004 01 09 00 01` in D22-10's
  probes).
- **Separately (likely):** `d21-cancel-subscription-with-concurrent-fill-streams` (and once its
  draft 22 twin) drew REQUEST_OK and then close 0x3 "retained SUBSCRIBE stream closed with a
  truncated REQUEST_UPDATE" when the update was sent with FIN; this is intermittent. The reader
  at lines 4602-4627 closes when a read reports FIN and the pending bytes are not a complete
  message, but it does not check whether the pending bytes are empty. When the FIN arrives in
  its own read, after the update has been consumed, the empty buffer counts as a truncated
  update. The other retained-stream reader has the check (lines 3621-3624); whether the FIN
  arrives with the data or alone depends on packetization, which fits the intermittency.
- **Repro:** `d22-single-request-update-response` (the row names all three scenarios). By
  hand: on a new bidirectional stream send `0d 0010 05 01 05 media 06 vide_1 00` with FIN.
- **Severity:** MUST for the rows; the practical effect is larger: any TRACK_STATUS sent with
  FIN ends the session, and one sent without FIN stalls moqxr (D22-11).
- **Fix direction:** fix D22-11 so that TRACK_STATUS reaches its NOT_SUPPORTED branch; in the
  retained-SUBSCRIBE reader treat FIN with an empty buffer as a clean end, as lines 3621-3624 do.
- **Confidence:** confirmed for the TRACK_STATUS close; likely for the truncated-update close.
- **Required:** answer each REQUEST_UPDATE; a complete message followed by FIN is not
  truncated. Confirm with moqxr's own tests before changing anything.

### D22-06 Silence where an answer or a close is required (refined: three legs confirmed by source, unscored)

- **Rows:** D22-4-2-MUST-110, D22-9-20-18-MUST-445, D22-9-5-MUST-355 (not_run).
- **Scenarios:** `d22-discover-original-publisher-namespaces`,
  `d22-discovery-update-invalid-forward`, `d22-update-on-track-status`,
  `d22-responder-update-on-publish-namespace`, `d22-subscriber-update-on-publish`.
- **Draft:** lines 1929-1931 (NAMESPACE for each matching namespace after accepting
  SUBSCRIBE_NAMESPACE), 5608-5613 (a FORWARD value other than 0 or 1 is a
  PROTOCOL_VIOLATION), 4300-4302 (REQUEST_UPDATE outside
  the permitted cases is a PROTOCOL_VIOLATION).
- **Observed:** after accepting SUBSCRIBE_NAMESPACE with the empty prefix moqxr sends no
  NAMESPACE for `media`; FORWARD 255 in a REQUEST_UPDATE on SUBSCRIBE_TRACKS gets no
  answer and no close; a REQUEST_UPDATE on its own PUBLISH_NAMESPACE (sent after the
  runner's REQUEST_OK) gets no answer and no close.
- **Where (the dig reads the code for each leg; all the same on `1883b9f`):**
  - NAMESPACE (`D22-4-2-MUST-110`): after an accepted SUBSCRIBE_NAMESPACE moqxr writes
    REQUEST_OK and keeps only the prefix for the overlap check (`moqt_session.cpp` lines
    4431-4440); `moqt_control_messages.cpp` has no encoder for NAMESPACE (0x08) at all, so no
    NAMESPACE is ever sent. Confirmed.
  - FORWARD 255 on SUBSCRIBE_TRACKS (`D22-9-20-18-MUST-445`): after REQUEST_OK on SUBSCRIBE_TRACKS
    (lines 4509-4549) the request stream is never read again, so the update is never seen.
    Confirmed. (The SUBSCRIBE legs `d22-forward-value-two` and `-255` close 0x3 correctly.)
  - REQUEST_UPDATE on moqxr's own PUBLISH_NAMESPACE (`D22-9-5-MUST-355`): once
    `send_request_stream_and_wait` has the REQUEST_OK (lines 1339-1448), the namespace stream is
    not read again while serving (`serve_subscriptions` uses `namespace_stream_id` only to send
    PUBLISH_NAMESPACE_DONE, lines 5618 and 5643). Confirmed.
  - TRACK_STATUS followed by REQUEST_UPDATE on the same stream (`d22-update-on-track-status`,
    `0d 0010 01 01 05 media 06 vide_1 00 02 0002 03 00`, no FIN): moqxr answers nothing for the
    whole 12 s context. Cause: TRACK_STATUS is not framed (D22-11), so
    `read_request_stream_message` waits for the rest of a message that is already complete, for
    up to moqxr's `--timeout` (lines 4234-4241), and the whole serve loop waits with it. The
    earlier explanations (the fixed track "x", or moqxr not reading the stream while it awaits a
    SUBSCRIBE) are withdrawn. Confirmed; tracked as D22-11.
  - `d22-subscriber-update-on-publish` (a permitted case, lines 4298-4299): moqxr reset its
    PUBLISH streams before the update arrived and sent no REQUEST_UPDATE_OK, so nothing was
    observed there.
- **Repro:** `d22-discover-original-publisher-namespaces`; `d22-discovery-update-invalid-forward`
  with `d22-forward-value-two` and `d22-forward-value-255`; `d22-update-on-track-status`,
  `d22-responder-update-on-publish-namespace` and `d22-subscriber-update-on-publish`.
- **Severity:** MUST for each row. Silence is not proof, so the runner keeps these rows
  unscored; the code shows the silence is real. A relay that uses SUBSCRIBE_NAMESPACE to find
  moqxr's namespace learns nothing from it.
- **Fix direction:** keep accepted SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS request streams and
  the PUBLISH_NAMESPACE stream in the set the serve loop reads (as it does for retained SUBSCRIBE
  streams), validate REQUEST_UPDATE and GOAWAY there, close on a responder REQUEST_UPDATE, and send
  NAMESPACE for the publisher's own namespace when a SUBSCRIBE_NAMESPACE prefix matches it.
- **Confidence:** confirmed by source for the three legs; the rows stay unscored by the runner.

### D22-07 WebTransport does not reject an unknown datagram type (carries M-18)

- **Row:** D22-11-MUST-488 (pass on native QUIC, not_run on WebTransport).
- **Scenario:** `d22-unknown-datagram-type`.
- **Draft:** lines 5955-5956: "An endpoint that receives an unknown datagram type MUST
  close the session."
- **Observed:** over native QUIC moqxr closes 0x3 "invalid MOQT datagram". The WebTransport
  run is confounded: the adapter runs this probe `--forward 1`, and moqxr sent its PUBLISH,
  reset it and closed with code 0 ("timed out waiting for stream data") before anything
  about the datagram showed. The dig ran the probe with `--forward 0 --paced` from a scratch
  copy of the adapter against `1883b9f`: moqxr waits for requests, the datagram
  `f0 13 2b 3e 2a` (type 0x132B3E2A) is delivered, and moqxr neither closes nor reacts for the
  12 s context. The row stays `not_run` (silence after a datagram is never scored, since a
  datagram can be lost), but the run is no longer confounded and agrees with the source.
- **Where (the evidence for this item):** `webtransport_client.cpp` lines 666-669 return 0
  for `picohttp_callback_post_datagram` (incoming datagrams are dropped), while
  `picoquic_client.cpp` lines 581-589 validate each datagram and close with 0x3.
- **Repro:** `d22-unknown-datagram-type` over WebTransport; with the bundled adapter the run is
  confounded as above, so run moqxr with `--forward 0 --paced` (see the adapter-mode table).
- **Severity:** MUST (5955-5956), WebTransport only. moqxr also never validates object
  datagrams there (M-18).
- **Fix direction:** in the WebTransport datagram callback run the same
  `validate_publisher_datagram` check as the native client and close the session on failure.
- **Confidence:** confirmed by source; the paced run is consistent with it.
- **Required:** validate datagrams on the WebTransport path as on native QUIC.

### D22-08 A second GOAWAY on an accepted SUBSCRIBE_NAMESPACE stream is not detected

- **Row:** D22-9-2-MUST-339 (fail).
- **Scenarios:** `d22-duplicate-request-goaway`, `d22-goaway-on-distinct-request-streams`
  (run together).
- **Draft:** lines 4113-4115: close with PROTOCOL_VIOLATION on more than one GOAWAY "on the
  control stream or on a single request stream".
- **Observed:** moqxr accepts SUBSCRIBE_NAMESPACE for `media` (`07 0001 00`); two GOAWAYs
  (`10 0003 00 a7 10`) on that request stream draw no close, and the liveness SUBSCRIBE is
  served. moqxr stops reading the request stream after accepting SUBSCRIBE_NAMESPACE, so
  it never sees them. The distinct-streams control (one GOAWAY on each of two streams)
  correctly drew no close. The same on `1883b9f`.
- **Where:** after an accepted SUBSCRIBE_NAMESPACE the serve loop writes REQUEST_OK and
  `continue`s (`moqt_session.cpp` lines 4431-4440); the request stream is not kept in any set
  the loop reads, unlike a SUBSCRIBE's stream (kept in `ActiveSubscription::request_stream_id`
  and read at lines 4553-4629).
- **Repro:** `d22-duplicate-request-goaway` with `d22-goaway-on-distinct-request-streams`. By
  hand: send SUBSCRIBE_NAMESPACE `50 ...` for prefix `media`, wait for `07 0001 00`, then write
  `10 0003 00 a7 10` twice on that stream.
- **Severity:** MUST (4113-4115). Low direct interop impact; the same unread stream also
  hides REQUEST_UPDATE (D22-06) and any later message on it.
- **Fix direction:** retain accepted SUBSCRIBE_NAMESPACE streams and read them in the serve loop;
  count GOAWAYs per stream and close with PROTOCOL_VIOLATION on the second.
- **Confidence:** confirmed.
- **Required:** keep reading accepted SUBSCRIBE_NAMESPACE request streams (also needed
  for REQUEST_UPDATE on them, D22-06) and close on a second GOAWAY.

### D22-09 SUBSCRIBE_TRACKS prefix overlap is not detected

- **Row:** D22-3-6-MUST-083 (fail).
- **Scenarios:** `d22-subscribe-tracks-overlap`, `d22-discovery-independent-overlap-spaces`.
- **Draft:** lines 1716-1719: a SUBSCRIBE_TRACKS whose prefix shares a common prefix with
  an established SUBSCRIBE_TRACKS MUST get SUBSCRIBE_TRACKS_ERROR with PREFIX_OVERLAP.
- **Observed:** a second SUBSCRIBE_TRACKS for `media`, and one for the empty prefix, are
  accepted with REQUEST_OK. The same check for SUBSCRIBE_NAMESPACE works
  (D22-4-2-MUST-111 passes). The same on `1883b9f`.
- **Where:** the SUBSCRIBE_NAMESPACE branch checks `established_namespace_prefixes` with
  `namespace_prefixes_overlap` and answers PREFIX_OVERLAP (`moqt_session.cpp` lines
  4403-4419); the SUBSCRIBE_TRACKS branch (lines 4462-4515) checks only that the prefix matches
  the publisher's namespace and goes straight to REQUEST_OK. No SUBSCRIBE_TRACKS prefixes are
  recorded.
- **Repro:** `d22-subscribe-tracks-overlap` with `d22-discovery-independent-overlap-spaces`.
  By hand: two SUBSCRIBE_TRACKS (`51 ...`) for prefix `media` on two request streams.
- **Severity:** MUST (1716-1719). A relay that repeats SUBSCRIBE_TRACKS gets duplicate
  PUBLISHes rather than an error it can act on.
- **Fix direction:** keep a second prefix list for SUBSCRIBE_TRACKS and run the same
  `namespace_prefixes_overlap` check against it, answering REQUEST_ERROR 0x30.
- **Confidence:** confirmed.
- **Required:** apply the overlap check to SUBSCRIBE_TRACKS, in its own overlap space.

### D22-10 A message that references an unregistered token alias is not rejected (SUBSCRIBE and REQUEST_UPDATE)

The first sweep saw this on REQUEST_UPDATE only (the item was titled "A REQUEST_UPDATE that
references an unregistered token alias is not rejected"). The re-sweep after the F1 runner
fixes shows the same missing check on SUBSCRIBE, where it is scored.

- **Rows:** D22-8-9-MUST-281 (fail after the F1 fixes; `not_run` before, when the probe
  asked for track "x", which moqxr refused as unknown before looking at the token). It also
  keeps D22-9-5-1-MUST-357, -359 and -360 unscored: their probes use such an update to make
  it fail.
- **Scenarios:** `d22-request-unknown-token-alias` (with `d22-request-deleted-token-alias`,
  which waits for a token cache and sends nothing); `d22-failed-subscription-update-cleanup`,
  `d22-failed-subscribe-namespace-update-close`, `d22-failed-subscribe-tracks-update-close`.
- **Draft:** lines 3708-3709: "The receiver of a message referencing an Alias that is not
  currently registered MUST reject the message with UNKNOWN_AUTH_TOKEN_ALIAS."
- **Observed:** a SUBSCRIBE for `media`/`vide_1` with AUTHORIZATION TOKEN USE_ALIAS 0, never
  registered (`03 0014 01 01 05 media 06 vide_1 01 03 02 02 00`), is answered SUBSCRIBE_OK
  (`04 0002 01 00`) and Objects follow, on both transports. The update
  `02 0006 03 01 03 02 02 00` (the same token) is answered REQUEST_OK
  (`07 0004 01 09 00 01`) on a SUBSCRIBE, and gets no answer on SUBSCRIBE_NAMESPACE or
  SUBSCRIBE_TRACKS. The run also stores an `unresolved_error_mapping` event reading
  `result=NOT_RUN`; that text is about a REQUEST_ERROR answer (draft 22 assigns
  UNKNOWN_AUTH_TOKEN_ALIAS no REQUEST_ERROR code) and does not contradict the FAIL.
  The same on `1883b9f`.
- **Where:** `valid_authorization_token` (`moqt_control_messages.cpp` lines 478-505) checks
  only the structure (USE_ALIAS and DELETE are an Alias Type followed by one Alias), and the
  REQUEST_UPDATE decoder (around lines 1995-2017) likewise; the session only acts on a
  malformed token or a REGISTER (`authorization_token_status`, called at `moqt_session.cpp`
  lines 4265-4268 for SUBSCRIBE). moqxr keeps no alias registry, so a well-formed reference to
  any alias is accepted.
- **Repro:** `d22-request-unknown-token-alias` with `d22-request-deleted-token-alias`. By hand:
  `03 0014 01 01 05 media 06 vide_1 01 03 02 02 00` on a new request stream.
- **Severity:** MUST (3708-3709). moqxr never checks tokens, so the practical risk is a client
  that believes an alias is registered and is not told otherwise.
- **Fix direction:** while moqxr advertises no token cache, treat every USE_ALIAS and DELETE as
  a reference to an unregistered alias and reject the message (the draft names
  UNKNOWN_AUTH_TOKEN_ALIAS, 0x17, a session error code in the table at line 7913).
- **Confidence:** confirmed.
- **Required:** reject a reference to an unregistered alias. With no token cache
  (ground rule 3) every alias is unregistered, so every USE_ALIAS reference is rejected.

### D22-11 Message framing knows a fixed list of types (new; root cause of D22-02, the D22-05 close and a D22-06 leg)

- **Rows:** D22-9-MUST-295 (fail, through D22-02), D22-9-10-MUST-381 (not_run), and the
  TRACK_STATUS effects in D22-05 (D22-9-5-MUST-356, D22-9-5-1-MUST-363) and D22-06
  (D22-9-5-MUST-355).
- **Scenarios:** `d22-unknown-control-message`, `d22-unknown-request-stream-message`,
  `d22-subscriber-sends-publish-state-notify`, `d22-single-request-update-response`,
  `d22-update-on-track-status`.
- **Draft:** lines 3876-3880 (an unknown message type MUST close the session; every control
  message carries its length "to simplify parsing", and none is meant to be ignored); 4711-4712
  (PUBLISH_STATE_NOTIFY from the subscriber MUST close the session with PROTOCOL_VIOLATION);
  the message table at 3826-3872 (TRACK_STATUS 0x0D, PUBLISH_STATE_NOTIFY 0x22, PUBLISH_SKIPPED
  0x0F).
- **Where:** `next_control_message` (`moqt_control_messages.cpp` lines 664-757) decides where a
  control or request message ends by switching on its type. Only the types it lists are framed;
  any other type returns false (lines 755-756), which every caller reads as "not complete yet".
  At draft 22 every control and request message is a type, a 16-bit length and a body (lines
  3876-3880), so the length is known for any type, listed or not. The list lacks TRACK_STATUS (0x0D), PUBLISH_STATE_NOTIFY
  (0x22), PUBLISH_SKIPPED (0x0F) and every unknown type. The callers then wait for bytes that
  never come: the control loop (`moqt_session.cpp` lines 4636-4702), `read_request_stream_message`
  (lines 3520-3558, which closes "request stream closed before a complete message" at FIN and
  otherwise blocks up to `--timeout`) and the retained-stream readers (lines 3561-3653,
  4553-4629).
- **Observed (draft 22, `1883b9f`; each is a separate effect of the same cause):**
  - `7e 00 00` on the control stream: no close, the session goes on (D22-02, row 295 FAIL).
  - `03 0010 01 01 05 media 06 vide_1 00 7e 00 00` on one request stream (a SUBSCRIBE with an
    unknown message after it; moqxr run `--forward 0 --paced` in the adapter-mode experiment):
    SUBSCRIBE_OK, every Object and PUBLISH_DONE, no close in 12 s. Unscored (the probe has no
    liveness follow-up).
  - `22 00 01 00` (PUBLISH_STATE_NOTIFY) from the subscriber on the SUBSCRIBE stream after
    SUBSCRIBE_OK (`d22-subscriber-sends-publish-state-notify`): moqxr keeps serving and sends
    PUBLISH_DONE, no close in 12 s. Row 381 stays `not_run`; its other scenario,
    `d22-publish-established-subscriber-sends-publish-state-notify`, cannot reach its stimulus
    against moqxr in either adapter mode (see the adapter-mode table above), so it was
    `not_run` for that reason and not, as earlier noted, only because of the adapter mode.
  - TRACK_STATUS with FIN: close 0x3 (D22-05); without FIN: silence for the whole context and a
    stalled serve loop (D22-06).
- **Repro:** the scenarios above. By hand, after SETUP: `7e 00 00` on the control stream (no
  close); or `0d 0010 01 01 05 media 06 vide_1 00` on a new request stream without FIN (no answer
  until moqxr's `--timeout`), then with FIN (close 0x3).
- **Severity:** MUST (3877-3878, 4711-4712). The most consequential item of the draft 22 list:
  one unknown or unlisted message wedges the stream it arrives on (the whole control stream, in
  the D22-02 case), and a TRACK_STATUS, a request any subscriber may send, either ends the
  session or stalls it.
- **Fix direction:** at draft 22 (and at any other draft whose messages all share that framing;
  check drafts 18 and 21 before changing them) frame every message by its 16-bit length whatever
  its type, then dispatch on the type: close with PROTOCOL_VIOLATION on an unknown type (the
  existing close at lines 4689-4692 then applies), answer TRACK_STATUS with NOT_SUPPORTED (the
  existing branch at lines 4443-4459), and close on a PUBLISH_STATE_NOTIFY from the subscriber.
  This does not require implementing PUBLISH_STATE_NOTIFY or PUBLISH_SKIPPED (ground rule 3).
- **Confidence:** confirmed (wire evidence for each effect, and the code path).
- **Required:** close the session on an unknown message type on any stream, and frame the
  draft 22 messages moqxr does not implement so it can refuse or close on them.

### D22-C1 A refused PUBLISH ends the session, GREASE code included (expectation question)

- **Rows:** D22-13-MUST-568, D22-13-MUST-NOT-569, D22-13-MUST-NOT-576 (not_run).
- **Scenario:** `d22-grease-request-error` (with `-grease-setup-options`,
  `-grease-auth-token-type`, `-grease-stop-sending`).
- **Draft:** lines 7024-7039: an unknown error code in a REQUEST_ERROR "MUST be treated
  as equivalent to INTERNAL_ERROR", and an endpoint "MUST NOT close the session because it
  received an unknown error code in a REQUEST_ERROR or PUBLISH_DONE".
- **Observed:** the runner rejects moqxr's PUBLISH with REQUEST_ERROR code 0x9d; moqxr
  resets the stream and ends the session with code 0, as it does after any refused
  PUBLISH (M-20). Whether it closed because of the unknown code cannot be told.
- **Question for the runner, not moqxr work yet:** pair the GREASE code with a known-code
  refusal in the same run, or treat "a publisher ends its session after a refused
  publication" as unrelated to these rows.

## Ground rules

1. **The drafts decide.** The checked-in texts
   `docs/draft-ietf-moq-transport-18.txt`, `docs/draft-ietf-moq-transport-21.txt` and
   `docs/draft-ietf-moq-transport-22.txt` in this repository are the only authority.
   Line numbers below refer to those files. Read the cited lines before changing
   anything.
2. **The runner is not the authority either.** If, after reading the cited draft
   lines, you believe a finding is wrong (the runner's stimulus, its expected code,
   or its reading of the text), do not change moqxr to satisfy it. Record the
   dispute under "Disputes" at the end with the draft quotation, and leave the item
   open. The runner can be wrong; several earlier findings were.
3. **moqxr is a live publisher with no cache.** It publishes objects as it produces
   them and keeps no history, so it is not expected to serve FETCH, joining FETCH,
   or anything that needs past objects. Do not implement FETCH or add a cache for
   this list. What the drafts still ask of a limited endpoint is covered in M-17.
   Likewise do not implement token validation, PUBLISH_STATE_NOTIFY, padding, or the
   optional SETUP capabilities (`MAX_REQUEST_UPDATES`, `MAX_FILTER_RANGES`, a token
   cache); see "Out of scope".
4. **Scope is drafts 18, 21 and 22 (draft 22 for the D22- items).** moqxr also speaks draft 16; do not change
   draft 16 behavior. Where a fix touches shared code, keep draft-dependent behavior
   behind the existing draft checks. Draft 21 is not draft 18 with a different
   version number; confirm each item against the draft it names.
5. **Do not hide a failure by weakening a check.** Fix the publisher, add or extend
   moqxr's own unit tests for the behavior, and re-run the runner scenarios listed
   in each item.

## How to reproduce a finding

Build the runner in this repository (see [building-and-running.md](building-and-running.md))
and start it in driven mode with the bundled moqxr adapter
(`adapters/moqxr/run.sh`; the adapter chooses moqxr's command-line options per
scenario). The commands below are the ones used for the runs in this list; use
absolute paths.

```sh
R=/path/to/moq-contribution-interop-runner       # this repository, built
M=/path/to/moqxr                                 # moqxr, built
T=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/key.pem -out $T/cert.pem \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -days 1

MOQXR_BIN=$M/build/openmoq-publisher $R/build/moq-interop-runner \
  --bind 127.0.0.1 --port 19811 --database $T/runs.sqlite3 \
  --docs $R/docs --requirements $R/requirements \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 19812 --publisher-port-end 19819 \
  --tls-cert $T/cert.pem --tls-key $T/key.pem \
  --driver-executable $R/adapters/moqxr/run.sh \
  --driver-fixture $M/tests/fixtures/locmaf-publisher.mp4 \
  --driver-log-root $T/logs \
  --publisher-no-fetch &

curl -s -X POST http://127.0.0.1:19811/api/v1/runs -H 'Content-Type: application/json' -d '{
  "draft": 21, "transport": "webtransport", "mode": "driven",
  "scenarios": ["d21-grease-setup-options"], "timeout_ms": 6000,
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}}'
# then: GET /api/v1/runs/<id>            outcomes per requirement
#       GET /api/v1/runs/<id>/events?limit=100   wire evidence
# moqxr's stderr is under $T/logs/<id>/<n>-<scenario>/stderr.bin
```

Notes for reading results:

- Use `"transport": "native-quic"` or `"webtransport"`; moqxr 0.4.1 works over both.
  Native QUIC needs QUIC DATAGRAM, which 0.4.1 negotiates.
- A requirement that names several scenarios is scored only when all of them have
  run. To see a row finish, pass every scenario the row names (the catalog entry in
  `requirements/draft18.json` or `draft21.json` lists them) in one run; a
  single-scenario run reports `incomplete`.
- The catalog entry for each requirement also holds its rationale and the draft
  section it cites. `build/moq-interop-audit --draft 21 --format json` lists the
  state of the whole catalog.
- Outcomes: `pass`, `fail`, `not_run` (nothing provable). `not_run` is not a pass.

## Priority 1: clear violations with wire evidence

These score `fail` today (or are proven by the liveness follow-up). Fix these first.

### M-01 Duplicate unknown SETUP options close the session

- **Rows:** D18-10-3-MUST-003, D18-14-MUST-001, D18-14-MUST-008,
  D18-14-MUST-NOT-001 (setup scenario), D18-15-4-MUST-001, D21-13-MUST-593,
  D21-13-MUST-NOT-594.
- **Scenarios:** `receive-setup-with-duplicate-unknown-options`,
  `setup-unknown-grease-options-and-duplicates` (draft 18);
  `d21-grease-setup-options` (draft 21).
- **Draft:** draft 18 lines 3541-3548 (also 6406-6408, 6541-6542); draft 21 lines
  3478-3481 and 6745-6747. "Receivers MUST allow duplicates of unknown Setup
  Options."
- **Observed:** a SETUP that repeats an unknown option (odd type 0x9d, and even type
  0x11c) makes moqxr close with PROTOCOL_VIOLATION and log "received invalid SETUP
  message". A single unknown option is accepted.
- **Cause:** `decode_parameter_type` in `src/transport/moqt_control_messages.cpp`
  (around line 458) rejects a zero delta after a non-zero previous type unless the
  type is the explicitly repeatable one, and the SETUP decoders pass none.
- **Required:** accept repeated instances of unknown SETUP options (odd and even
  types) and ignore them. Keep rejecting a zero delta where the draft forbids a
  repeat (known, non-repeatable options).

### M-02 Malformed namespaces and names are answered with an error, not a close

- **Rows:** D18-2-4-1-MUST-002 to -005, D21-8-7-MUST-251 to -254.
- **Scenarios:** `receive-zero-length-namespace-field`,
  `receive-namespace-with-33-fields`, `receive-track-namespace-over-4096-bytes`,
  `receive-full-track-name-over-4096-bytes` (draft 18);
  `d21-subscribe-empty-namespace-field`, `d21-subscribe-33-namespace-fields`,
  `d21-subscribe-tracks-oversized-namespace`,
  `d21-subscribe-oversized-full-track-name` (draft 21).
- **Draft:** draft 18 lines 997-998, 1003-1004, 1021-1022; draft 21 lines 3109-3122.
  A namespace with a zero-length field, more than 32 fields, a field or namespace
  over the stated size, or a full track name over 4096 bytes is a protocol
  violation: close the session with PROTOCOL_VIOLATION (0x3).
- **Observed:** moqxr treats the malformed name as an unknown track or namespace,
  replies `REQUEST_ERROR` code 0x02 ("track does not exist" or "unsupported
  namespace prefix"), then closes with application code 0.
- **Required:** validate namespace and name structure when decoding SUBSCRIBE (and
  SUBSCRIBE_TRACKS, SUBSCRIBE_NAMESPACE, FETCH if decoded) and close with
  PROTOCOL_VIOLATION instead of replying REQUEST_ERROR.
- **Where:** the track-namespace decode helper in
  `src/transport/moqt_control_messages.cpp` and the "does not exist" branches in
  `src/transport/moqt_session.cpp` (lines 4207, 4217, 4848, 7542, 7669, 8886).

### M-03 SUBSCRIBE_NAMESPACE has no 32-field limit

- **Rows:** D18-10-18-MUST-001, D21-9-15-MUST-383. (SUBSCRIBE_TRACKS already closes
  correctly: D18-10-19-MUST-001, D21-9-18-MUST-391 pass.)
- **Scenarios:** `receive-subscribe-namespace-with-33-prefix-fields`,
  `d21-subscribe-namespace-prefix-too-many-fields`.
- **Draft:** draft 18 lines 4787-4788; draft 21 lines 4512-4513. More than 32 prefix
  fields is a PROTOCOL_VIOLATION.
- **Observed:** a 33-field prefix is accepted; moqxr answers REQUEST_ERROR
  ("unsupported namespace prefix") and closes with code 0.
- **Where:** `decode_subscribe_namespace_message`
  (`moqt_control_messages.cpp` around line 1198) has no limit check;
  `decode_subscribe_tracks_message` (around line 1560) does. Mirror it. This is
  probably fixed together with M-02.

### M-04 "Does not exist" uses REQUEST_ERROR code 0x02 (TIMEOUT)

- **Rows:** D18-3-2-1-MUST-002, D18-3-2-2-MUST-001, D18-3-2-2-MUST-002,
  D21-2-4-2-MUST-031, D21-6-5-MUST-170, D21-6-5-MUST-171, D21-6-5-MUST-172.
- **Scenarios:** `request-track-in-single-period-namespace`,
  `request-empty-track-name-in-session-namespace`,
  `request-unrecognized-session-level-name` (draft 18);
  `d21-request-single-period-namespace`, `d21-session-namespace-empty-track-request`,
  `d21-session-namespace-unknown-track-request`,
  `d21-session-namespace-unknown-namespace-request` (draft 21).
- **Draft:** draft 18 lines 1426-1427, 1445-1450 and the REQUEST_ERROR table at
  6848 (TIMEOUT 0x2) and 6860 (DOES_NOT_EXIST 0x10); draft 21 lines 857-858,
  2359-2364 and the table at 7657 (TIMEOUT 0x2) and 7677 (DOES_NOT_EXIST 0x10).
- **Observed:** moqxr answers these requests with `05 ... 02 00 14 "track does not
  exist"` (or `... 1c "unsupported namespace prefix"`); the code is 0x02, which is
  TIMEOUT. The reason text is right, the code is the one from an earlier draft.
- **Required:** use DOES_NOT_EXIST (0x10) wherever the publisher means "no such
  track, namespace or prefix", for drafts 18 and 21. Audit every hard-coded `0x2`
  REQUEST_ERROR in `moqt_session.cpp` (lines 4207, 4217, 4312, 4346, 4848, 7542,
  7669, 8886, 9015) and confirm which of them mean TIMEOUT before changing it.
  Leave draft 16 behavior alone.

### M-05 SUBSCRIBE_NAMESPACE prefix overlap is not detected

- **Rows:** D18-10-18-MUST-003, D21-9-15-MUST-385.
- **Scenarios:** `receive-overlapping-subscribe-namespace-in-same-session`,
  `d21-subscribe-namespace-overlap`.
- **Draft:** draft 18 lines 4803-4807; draft 21 lines 4528-4532. Within a session, a
  SUBSCRIBE_NAMESPACE whose prefix shares a common prefix with an established
  SUBSCRIBE_NAMESPACE MUST be answered with REQUEST_ERROR PREFIX_OVERLAP (0x30).
  SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS have independent overlap spaces.
- **Observed:** with prefixes [media], [media] again, [] (empty), and [media, c],
  moqxr accepted the first three (REQUEST_OK) and answered the fourth with code 0x02.
  It tracks no overlap.
- **Required:** remember the established namespace-subscription prefixes per session
  and reject any new prefix that is equal to, an ancestor of, or a descendant of one
  of them with PREFIX_OVERLAP (0x30). Keep the SUBSCRIBE_TRACKS space separate.

### M-06 A second SUBSCRIBE to the same track is accepted

- **Row:** D18-5-1-MUST-004.
- **Scenario:** `subscribe-again-to-established-publisher-track`.
- **Draft:** draft 18 lines 1979-1982. An endpoint can have at most one subscription
  to a track in a given role; a second attempt MUST fail with DUPLICATE_SUBSCRIPTION
  (0x19 in the draft 18 table at 6866).
- **Observed:** the second SUBSCRIBE (new Request ID, same track) is answered and
  moqxr serves the objects again.
- **Required:** fail it with DUPLICATE_SUBSCRIPTION. Check how draft 21 names and
  numbers this error before changing draft 21 behavior; the runner has no draft 21
  row for it.

### M-07 FIRST_OBJECT is never set on a subgroup header

- **Row:** D18-2-2-MUST-001.
- **Scenario:** `publish-new-subgroup`.
- **Draft:** draft 18 lines 899-905 and 5303-5305. When the original publisher opens
  a new subgroup it MUST set the FIRST_OBJECT bit (0x40) in the subgroup header type
  to mark that the first object on the stream is the first object ever published in
  that subgroup.
- **Observed:** moqxr's subgroup streams open with type 0x38 for groups 0 and 1, so
  bit 0x40 is never set.
- **Where:** `encode_subgroup_header` in `moqt_control_messages.cpp` (around line
  2030) and its caller in `moqt_session.cpp` (around line 2485).
- **Required:** set the bit whenever the stream starts at the subgroup's first
  object (for a live publisher, when it opens a subgroup it produced from the
  start); do not set it when a stream begins mid-subgroup. Check the draft 21 text
  for the equivalent rule before changing draft 21.

### M-08 Server SETUP carrying AUTHORITY or PATH is accepted by moqxr as a client

- **Rows:** D18-10-3-1-1-MUST-001 and -002, D18-10-3-1-2-MUST-001 and -002,
  D21-9-1-1-MUST-293 and -294, D21-9-1-2-MUST-300 and -301.
- **Scenarios:** `receive-server-setup-with-authority`,
  `receive-webtransport-setup-with-authority`, `receive-server-setup-with-path`,
  `receive-webtransport-setup-with-path` (draft 18); `d21-server-sends-authority`,
  `d21-webtransport-server-sends-authority`, `d21-server-sends-path`,
  `d21-webtransport-server-sends-path` (draft 21).
- **Draft:** draft 18 lines 3562 and 3579; draft 21 lines 3490-3510. The AUTHORITY
  and PATH options are for a client's SETUP; a client that receives them from the
  server MUST close the session (INVALID_AUTHORITY 0x19, INVALID_PATH 0x8; see
  the session termination tables at draft 18 line 1703/1750 and draft 21 6478/6522).
- **Observed:** moqxr never closes; it goes on to announce and publish.
- **Where:** `decode_server_setup_message` (`moqt_control_messages.cpp` around line
  857) skips every option value without checking AUTHORITY or PATH.
- **Required:** when decoding a server SETUP in the client role, close with the
  matching error if AUTHORITY or PATH is present. Confirm per draft what applies on
  WebTransport versus native QUIC (the scenarios differ by transport).

### M-09 Unknown messages, unknown stream types and malformed GOAWAY do not close the session

- **Rows:** D18-10-MUST-008 (unknown control-stream message type), D18-3-4-MUST-001
  (unknown unidirectional stream type), D18-10-4-MUST-002 (two GOAWAYs on the control
  stream), D18-10-4-MUST-005 (GOAWAY URI length 8193), D18-10-4-MUST-007 (GOAWAY with
  wrong-parity Request ID), D21-9-2-MUST-327, D21-9-2-MUST-331, D21-9-MUST-285
  (message body length mismatch, here a 1-byte GOAWAY body), D18-1-4-3-MUST-003 and
  D21-8-3-MUST-233 (SETUP TOKEN option with an undefined Alias Type).
- **Scenarios:** `receive-unknown-message-type`,
  `receive-unknown-unidirectional-stream-type`, `receive-two-goaways-on-control-stream`,
  `receive-goaway-uri-length-8193`,
  `receive-control-goaway-with-wrong-receiver-request-id-parity`,
  `d21-duplicate-control-goaway`, `d21-goaway-uri-length-boundary`,
  `d21-request-message-truncated-at-fin`,
  `receive-understood-key-value-invalid-serialization`,
  `d21-setup-known-key-value-malformed-value`.
- **Draft:** draft 18 lines 3688, 3719, 3741 for the GOAWAY rules; the catalog entry
  for each row (its `source` field and rationale) gives the others. Each states
  "close the session" for the input. Draft 21 lines 3434-3440 (an unknown message type MUST close the session; a length that does not match the body MUST be a PROTOCOL_VIOLATION) and 3679-3709 (GOAWAY).
- **Observed:** moqxr logs `skipping unhandled control message type=0x..` (in
  `moqt_session.cpp` around line 4551) and carries on. After each input the runner
  sends a valid SUBSCRIBE (a "liveness follow-up"); moqxr answers it with
  SUBSCRIBE_OK and serves objects, so it demonstrably did not close.
- **Required:** close the session (the code is PROTOCOL_VIOLATION unless the draft
  names another: INVALID_REQUEST_ID, 0x4, for a GOAWAY whose Request ID has the wrong
  parity (draft 18 lines 3738-3741), and KEY_VALUE_FORMATTING_ERROR, 0x6, for
  malformed key-value pairs in SETUP) when the control
  stream carries an unknown message type, when a peer opens a unidirectional stream
  of an unknown type, and on the GOAWAY violations above.
- **Note:** these rows pass only if the close arrives within about 1.5 seconds of
  the input and is not a NO_ERROR (code 0) close; see
  [scoring-and-audit.md](scoring-and-audit.md).

### M-10 AUTHORIZATION TOKEN structure is not validated

- **Rows:** D18-10-2-2-MUST-005 (undecodable Token structure), D18-10-2-2-MUST-011
  (REGISTER exceeding the advertised cache size), D21-8-9-MUST-267, D21-8-9-MUST-277.
- **Scenarios:** `receive-undecodable-authorization-token-structure`,
  `register-request-token-exceeding-advertised-cache-size`,
  `d21-request-undecodable-authorization-token`,
  `d21-request-token-cache-overflow`, `d21-request-alias-registration-with-default-zero-cache`.
- **Draft:** draft 18 lines 3160-3161 and 3221; draft 21 lines 3262-3263 and 3319.
  A Token that cannot be decoded is a KEY_VALUE_FORMATTING_ERROR (0x6) session
  error; a REGISTER that would exceed the cache size the receiver advertised is an
  AUTH_TOKEN_CACHE_OVERFLOW (0x13) session error. moqxr advertises no cache size,
  which means zero.
- **Observed:** moqxr treats the parameter as opaque
  (`moqt_control_messages.cpp` around line 1532, "opaque to this publisher") and
  serves the request.
- **Required (minimum):** decode the Token structure (alias type, token type, value
  framing) and treat a malformed one as KEY_VALUE_FORMATTING_ERROR; treat any
  REGISTER as AUTH_TOKEN_CACHE_OVERFLOW while moqxr advertises a zero cache. **Not
  required:** judging whether a token's value is valid for its type; moqxr has no
  token types, and that is out of scope.

### M-11 A Range Filter with no advertised MAX_FILTER_RANGES gets the wrong error

- **Rows:** D21-3-3-2-MUST-065, D21-9-1-6-MUST-315.
- **Scenarios:** `d21-range-filter-with-zero-negotiated-limit`,
  `d21-range-filter-default-zero-limit`.
- **Draft:** draft 21 lines 1226 and 3602-3607. `MAX_FILTER_RANGES` defaults to 0, so
  a peer that does not advertise it MUST NOT send Range Filter parameters, and a
  receiver MUST reject one with REQUEST_ERROR INVALID_FILTER (0x36, table at 7695).
- **Observed:** moqxr rejects the subscription but with code 0x01 (UNAUTHORIZED) and
  the reason "invalid SUBSCRIBE".
- **Required:** use INVALID_FILTER (0x36) for this rejection.

### M-12 Nested FILL_PARAMETERS contents are not validated (draft 21)

- **Rows:** D21-9-20-9-MUST-429 (invalid GROUP_ORDER), D21-9-20-10-MUST-432
  (overflowing location filter), D21-9-20-16-MUST-447 (forbidden nested parameters).
- **Scenarios:** `d21-fill-invalid-group-order`,
  `d21-fill-location-filter-end-group-overflow`,
  `d21-fill-forbidden-nested-authorization`,
  `d21-fill-forbidden-track-property-filter`,
  `d21-fill-recursive-parameter`, `d21-fill-parameter-whitelist-protocol-violation`.
- **Draft:** draft 21 lines 4956-4957, 5015-5016, 5129 and 5186-5187. A parameter
  inside FILL_PARAMETERS that the table does not allow there, or an invalid value
  inside it, MUST close the session with PROTOCOL_VIOLATION. The non-nested
  variants of 429 and 432 already close correctly.
- **Observed:** the SUBSCRIBE decoder sets `fill_requested` and skips the nested bytes
  (`moqt_control_messages.cpp` around lines 1532-1538, 1635, 1798-1801).
- **Required:** decode and validate the nested parameter block with the same rules as
  the top level.

### M-13 REQUEST_UPDATE_OK omits LARGEST_OBJECT (draft 21)

- **Row:** D21-9-20-18-MUST-456.
- **Scenarios:** `d21-largest-object-required-after-publication`,
  `d21-largest-object-before-publication`.
- **Draft:** draft 21 lines 5236-5244 (section 9.20.18): LARGEST_OBJECT may appear in
  REQUEST_UPDATE_OK, and "if Objects have been published on this Track the Publisher
  MUST include this parameter"; also lines 3903-3908, where a REQUEST_UPDATE that
  raises the End Location is answered by a REQUEST_UPDATE_OK that includes it.
- **Observed:** `07 00 01 00` after Object 0/0 had been received, while a second
  SUBSCRIBE_OK in the same run did carry LARGEST_OBJECT (0,1).
- **Where:** `largest_object_for_response` in `moqt_session.cpp` (around lines
  4029-4134) supplies it only on some paths.
- **Required:** for a live publisher, the largest location published so far;
  include it in every REQUEST_UPDATE_OK once any object exists.

### M-14 Track Properties and responder-side updates are tolerated (draft 21)

- **Rows:** D21-9-3-MUST-337, D21-9-5-MUST-344. (These do not score FAIL in the
  runner; see the note.)
- **Scenarios:** `d21-publish-namespace-ok-with-track-properties`,
  `d21-responder-update-on-publish-namespace`, `d21-update-on-track-status`.
- **Draft:** draft 21 lines 3761-3764: Track Properties in a REQUEST_OK,
  REQUEST_UPDATE_OK, SUBSCRIBE_NAMESPACE_OK or PUBLISH_NAMESPACE_OK MUST be answered
  with a PROTOCOL_VIOLATION close. Lines 3852-3854: a REQUEST_UPDATE from anyone
  other than the request's sender (or other than the two permitted cases) is also a
  PROTOCOL_VIOLATION.
- **Observed:** moqxr accepts a REQUEST_OK carrying a MAX_CACHE_DURATION Track
  Property ("not consumed by this minimal request-ok parser",
  `decode_request_ok`, `moqt_control_messages.cpp` around line 1092) and takes no
  action on a responder-side REQUEST_UPDATE or on a REQUEST_UPDATE after
  TRACK_STATUS. It stays silent, so the runner leaves the rows unscored.
- **Required:** reject both with a PROTOCOL_VIOLATION close. Fix it anyway; the
  runner will start scoring these once a liveness follow-up covers their probe
  family.

## Priority 2: should-fix

### M-15 REDIRECT and oversized REQUEST_ERROR are not validated

- **Rows:** D18-10-6-1-MUST-005, D21-9-4-1-MUST-341 (REDIRECT with a non-empty track
  name MUST be a PROTOCOL_VIOLATION: draft 18 lines 3835-3836, draft 21 lines
  3797-3798); D21-8-5-MUST-248 (a REQUEST_ERROR reason phrase over 1024 bytes MUST be
  a PROTOCOL_VIOLATION: draft 21 lines 3035-3039).
- **Scenarios:** `receive-publish-namespace-redirect-with-nonempty-track-name`,
  `d21-publish-namespace-redirect-nonempty-track-name`,
  `d21-publish-request-error-oversized-reason`.
- **Observed:** moqxr treats any REQUEST_ERROR on its own PUBLISH_NAMESPACE or PUBLISH
  as an ordinary failure ("request failed"), exits with status 1 and closes with
  application code 0. It does not validate the REDIRECT fields or the reason length.
- **Required:** validate those fields when decoding REQUEST_ERROR and close with
  PROTOCOL_VIOLATION. Following a valid REDIRECT is not required.

### M-16 Request-ID parity and duplicate checks (no work needed)

moqxr already closes correctly on a wrong-parity or duplicate Request ID and on an
oversized SUBSCRIBE_TRACKS prefix: D18-10-1-MUST-001, D18-10-1-MUST-002,
D21-6-4-2-1-MUST-154 and D18-10-19-MUST-001 and D21-9-18-MUST-391 score `pass` in the
latest sweep. (`D21-6-4-2-1-MUST-155` also closed with the right code when its
scenarios were run together, but is not part of that sweep's passing set.) Listed
here so an agent does not re-investigate them, and so they act as regression checks.

### M-17 Unsupported request types: answer NOT_SUPPORTED, and keep the session

Because moqxr is a live publisher with no cache, FETCH and joining FETCH are not
expected to work, and that alone is not a defect. The drafts say what a limited
endpoint is encouraged to do with a request it does not implement. This is a
recommendation (a SHOULD), not a MUST:

- **Draft:** draft 18 lines 1863-1870 and draft 21 lines 595-601: "Limited endpoints
  SHOULD respond to any unsupported messages with the appropriate NOT_SUPPORTED
  error code, rather than ignoring them." NOT_SUPPORTED is code 0x3 in both drafts
  (draft 18 table at 6850, draft 21 at 7659). Draft 18 line 3895 defines it: the
  endpoint does not support the type of request.
- **Observed (0.4.1; fixed in `0993cf7`, which now answers NOT_SUPPORTED without ending the session, but see M-21):** a well-formed FETCH (or TRACK_STATUS) gets `REQUEST_ERROR` code 0x1
  (UNAUTHORIZED) with the text "unsupported request stream", and then moqxr closes the
  session with PROTOCOL_VIOLATION and logs "received unsupported request stream".
- **Where:** `moqt_session.cpp` lines 4331-4333, 7522, 8999-9001, 10107.
- **Recommended (SHOULD):** reply NOT_SUPPORTED (0x3) instead of 0x1, and let the
  session continue after a request that is well-formed but unsupported. The draft
  states no MUST for either part, so treat this as should-fix. Surviving is the
  intended behavior: the draft's answer to an unsupported request is a response, and
  PROTOCOL_VIOLATION is defined for a peer that did something not allowed, which a
  valid FETCH is not. Keep the PROTOCOL_VIOLATION close for requests that are
  actually malformed. Do not implement FETCH.
- **Effect on runner results:** the rows that need FETCH to be served
  (for example D18-10-12-2-MUST-004, the Joining Fetch rule) cannot be scored
  against moqxr and stay `not_run`/ambiguous; that is expected. The runner side of
  this was listed under "Runner-side follow-ups" and is done there (the runner
  now accepts a no-FETCH declaration).

### M-18 WebTransport does not validate publisher datagrams or peer data streams

- **Rows:** D18-11-3-1-MUST-002 and -003 (invalid object datagram types),
  D18-11-4-2-MUST-002 and -003 (invalid subgroup header types) and their draft 21
  counterparts.
- **Observed:** on native QUIC `validate_publisher_datagram`
  (`picoquic_client.cpp` line 582, `moqt_control_messages.cpp` line 548) closes on an
  invalid datagram type and the rows pass. On WebTransport there is no such call, and
  peer-opened unidirectional streams are buffered in `received_streams`
  (`webtransport_client.cpp` around lines 717-755) and never parsed, so the rows stay
  unscored.
- **Required:** apply the same datagram validation on the WebTransport path, and parse
  the subgroup header type of peer-opened streams so that an invalid type closes the
  session as it should.

### M-20 A rejected announcement or publication ends the session (draft 18 section 14)

- **Rows:** D18-14-MUST-004, D18-14-MUST-NOT-002, D18-14-MUST-NOT-001 (the
  unknown-error context). Draft 21 has the same wording (lines 6755-6759);
  find the corresponding draft 21 scenario before
  changing draft 21.
- **Scenario:** `publisher-request-rejected-with-unknown-error`: the runner answers
  moqxr's PUBLISH_NAMESPACE (or PUBLISH) with a REQUEST_ERROR carrying an error code
  the publisher cannot know, with FIN, then sends a valid follow-up request on a new
  stream and checks that the session still works.
- **Draft:** draft 18 lines 6412-6416: "Receipt of an unknown error code in any error
  context (Session Termination, REQUEST_ERROR, PUBLISH_DONE, or Data Stream Reset)
  MUST be treated as equivalent to INTERNAL_ERROR for that context. An endpoint MUST
  NOT close the session because it received an unknown error code in a REQUEST_ERROR
  or PUBLISH_DONE."
- **Observed:** about 10 ms after the rejection moqxr closes the session with
  application code 0, logs `transport publish failed: request failed:`, and exits with
  status 1. It closes before the runner's follow-up request can be sent. An earlier
  control run (REQUEST_ERROR codes 0x0 INTERNAL_ERROR, 0x10 DOES_NOT_EXIST and an
  unknown code) ended identically, so the close does not depend on the code.
- **Where:** `moqt_session.cpp` around line 1406: any decoded REQUEST_ERROR on a
  request becomes `TransportStatus::failure(... kEndpointPermanent)` for the whole
  endpoint, whatever the code.
- **Two readings, so this is should-fix rather than an established violation.**
  The literal one: the close is not shown to be *because the code is unknown*,
  since moqxr closes for every rejection. The stricter one: an unknown code must be
  treated exactly as INTERNAL_ERROR, and an endpoint must not close the session
  because of an unknown code; an endpoint that closes on every REQUEST_ERROR,
  INTERNAL_ERROR included, cannot satisfy both statements for an unknown code. The
  second reading is the safer one to implement, because it is consistent with both
  sentences of the draft.
- **Recommended:** a REQUEST_ERROR answering one of moqxr's own announcements or
  publications ends that request, not the session or the process. Treat an unknown
  code like INTERNAL_ERROR, and keep serving requests on the session. If moqxr has
  genuinely nothing left to do it may finish later through its normal end of
  session, but not as an immediate reaction to the rejection.
- **Verify:** run `publisher-request-rejected-with-unknown-error`. Today the rows are
  `not_run`, because a NO_ERROR close right after the rejection cannot be attributed
  to the unknown code (the runner deliberately does not fail on it). After the fix
  the session stays open, the follow-up request is answered or at least not closed,
  and the rows should score `pass`. Add this scenario to the regression set.
- **Runner note:** a control context (the same rejection with a known code, in the
  same run) would let the runner tell "closes only for unknown codes" from "closes
  for every rejection" and score this row directly. It is not implemented.

## Priority 3: observed, lower value

### M-19 Control-stream GOAWAY URI is ignored

- **Rows:** D18-10-4-MUST-004, D21-9-2-MUST-329 (not scored today: moqxr never
  reconnects, so the rows stay `not_run`).
- **Draft:** draft 18 lines 3714-3716 and draft 21 lines 3699-3701: "The client MUST
  use this URI for the new session if provided" (a zero-length URI means reuse the
  current one).
- **Required:** reconnect to the URI given in a control-stream GOAWAY. Lowest priority
  of the list; it needs reconnect logic.

## Out of scope (do not implement for this list)

- FETCH, joining FETCH, and anything that needs a cache or history (see M-17 for
  what to do with such requests).
- Token validation by type, expiry, or policy (D18-10-2-2-MUST-008 and -010,
  D21-8-9-MUST-270 and -273). moqxr understands no token type. Its `--auth-*`
  options only make it present credentials as a client.
- Advertising `MAX_REQUEST_UPDATES`, `MAX_FILTER_RANGES`, or a token cache size. They
  are optional. Not advertising them leaves some rows unscored; M-10 and M-11 only
  require correct handling of the default (zero).
- PUBLISH_STATE_NOTIFY, padding, early subgroup termination, mutable-property
  filters, discovery authorization, or any other behavior the drafts leave to the
  publisher's discretion.
- A Group 7 / Object 9 fixture, or two Subgroups in one Group. The test fixture
  (`locmaf-publisher.mp4`) has Groups 0 and 1; the rows that need other fixtures are
  not moqxr defects.
- Changes to draft 16 behavior.

## Suggested order

1. M-01, M-04, M-11 (small, local, high confidence).
2. M-02 and M-03 together (shared validation), then M-05, M-06, M-07.
3. M-08, M-09 (close paths on the control and unidirectional streams), M-10.
4. M-12, M-13, M-14 (draft 21 only).
5. M-15, M-20, M-17, M-18, then M-19 if time allows.

After each group, rebuild moqxr and re-run the scenarios named in the items; compare
with the "Done criteria" below.

## Done criteria

- Every Priority 1 item: its rows score `pass` when all scenarios the row names are
  run together, on both transports, against the rebuilt moqxr. For items marked
  "not scored", the rows may stay `not_run`; the fix is verified by moqxr's own unit
  tests plus a manual check that the session closes with the expected code.
- No row that passes today starts failing. The regression set is the rows named in
  M-16, plus any row you can see passing in a fresh run before you start (run the
  whole catalog once first and keep the list).
- M-20: `publisher-request-rejected-with-unknown-error` scores `pass` for
  D18-14-MUST-004, D18-14-MUST-NOT-002 and D18-14-MUST-NOT-001 (the last row also
  names other scenarios; run them together to see it complete).
- moqxr's own test suite passes, with new tests for each behavior added.
- The runner's audit is unchanged (`build/moq-interop-audit --draft 18` and
  `--draft 21`: 173/173 required rows); nothing in this list asks for a runner change.

## Reporting back

For each item: done, partly done, or disputed; the moqxr commit; the runner rows and
scenarios re-run with their new outcomes; and, for any disputed item, the draft lines
and the reading that leads you to disagree. Update nothing in the runner repository
from the moqxr side.

## Disputes

(Leave empty until used. One entry per disputed item: item id, draft quotation, why
the runner's expectation is not what the draft requires.)

## Runner-side follow-ups (not moqxr work)

These come out of the same results but belong to the runner repository:

- **FETCH-based scenarios against a live publisher. Done.** 45 scenarios (23 in
  draft 18, 22 in draft 21) start with a FETCH and used to end in a run-level error
  against moqxr ("received unsupported request stream"). The runner now lets a
  publisher declare that it has no FETCH (`--publisher-no-fetch`, or
  `"publisher_capabilities": {"fetch": false}` in a run): those scenarios are skipped,
  rows that need only them are `not_applicable`, and a selection of only such
  scenarios is refused with 422 `scenario_requires_publisher_capability`. Start the
  runner with `--publisher-no-fetch` when you reproduce a finding against moqxr, and
  do not select FETCH scenarios alone. M-17 above stays moqxr work; the declaration
  does not test the NOT_SUPPORTED answer. See [interop-notes.md](interop-notes.md).
- **Draft 21 AUTHORITY/PATH rows (M-08)** are scored through the announcement
  controller and do not use the liveness follow-up; their rule only checks that the
  PUBLISH came after the runner's SETUP was written.
- **Peer-close and response probe families** (probes where the publisher opens the
  request stream) do not yet use the close-attribution rule.
- **Remaining hard-coded baseline SUBSCRIBEs** in a few draft 21 probes (for example
  `d21-unknown-request-stream-message`, `d21-duplicate-invalid-request-id`); draft 21 is
  frozen. At draft 22 the F1 work made the last 29 such probes send the run's names (three
  had done so since the first draft 22 sweep), `d22-unknown-request-stream-message` and
  `d22-update-on-track-status` included. Against moqxr this changed only D22-8-9-MUST-281
  (D22-10). `d22-unknown-request-stream-message` still runs moqxr `--forward 1`, so with the
  bundled adapter the request-stream half of D22-9-MUST-295 is never exercised against moqxr
  (moqxr sends its own PUBLISH and ends the session). In `d22-update-on-track-status` moqxr
  answers nothing to a TRACK_STATUS for the run's track; the dig traced that to D22-11.
  `d22-unknown-datagram-type` also still runs `--forward 1`, which confounds its WebTransport
  run (D22-07).
- **Adapter-mode questions for four draft 22 ids: resolved, adapter unchanged.** The dig of
  2026-10-06 against moqxr `1883b9f` ran each with the opposite mode from a scratch copy of the
  adapter (the table under "Status against moqxr 1883b9f"). Pacing
  `d22-unknown-request-stream-message` and `d22-unknown-datagram-type` lets the stimulus reach
  moqxr, which then ignores it, but silence is unscored and no verdict changes, so they keep
  `--forward 1`. `--forward 1` does not help
  `d22-publish-established-subscriber-sends-publish-state-notify` (moqxr publishes `catalog`
  first and the run ends `error`) or `d22-subscribe-tracks-publish-skipped-then-capacity-recovers`
  (no PUBLISH on the single granted stream; moqxr has no PUBLISH_SKIPPED). So
  D22-3-6-3-MUST-NOT-086 is not applicable to moqxr, and D22-9-10-MUST-381 is unscored because
  of moqxr's behavior (D22-11), not the adapter mode.
- **Rows moqxr cannot be scored on by silence** (M-14) need a liveness follow-up for
  their probe family.

### M-21 An unknown FETCH Type must close the session, not draw REQUEST_ERROR (draft 18)

Found against moqxr `0993cf7` (0.4.1-dev), which resolved M-17 by answering FETCH and
TRACK_STATUS with NOT_SUPPORTED. That reply is sent before the FETCH Type is looked at.

- **Draft:** draft 18 line 4355: "An endpoint that receives a Fetch Type other than 0x1,
  0x2 or 0x3 MUST close the session with a PROTOCOL_VIOLATION."
- **Row:** D18-10-12-MUST-001, scenario `receive-fetch-with-unknown-type`. It passed on
  0.4.1 (which closed on every FETCH) and fails deterministically on `0993cf7`, on both
  transports: no close, and the run records no PROTOCOL_VIOLATION.
- **Where:** `moqt_session.cpp`, the `request_type == 0x16 || 0x0d` branch near line 4444.
  It decodes only the Request ID and never reads the Fetch Type.
- **Fix:** for FETCH (0x16), decode the Fetch Type after the Request ID; if it is not
  0x1, 0x2 or 0x3, close with PROTOCOL_VIOLATION. A valid type still gets NOT_SUPPORTED.
  This needs no cache and does not require implementing FETCH.
