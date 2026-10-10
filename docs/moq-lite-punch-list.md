# moq CLI (moq-lite-06) Punch List

This is a work list for an agent fixing the `moq` CLI of moq-dev/moq (its moq-lite session
layer, `rs/moq-net/src/lite/`) so that it conforms to moq-lite-06
(`docs/draft-lcurley-moq-lite-06.txt`). Every ML- item comes from driven runs of the interop
runner in this repository against `moq 0.14.1` built from moq-dev/moq
`b8b0d235a99bddb9043f453c46c958362d6c9247`, publishing an ffmpeg fMP4 source through the
bundled `adapters/moq-lite/run.sh` (`moq ... --connect-version moq-lite-06 --broadcast
interop.hang import fmp4`). The first sweep (ML-01 to ML-03, ML-Q1) ran on 2026-10-09
(22:21 to 22:38 -07:00) on native QUIC (`moql://`) and WebTransport (`https://`); the second
sweep (L2a: the track, fetch, probe and goaway scenarios, ML-04 and ML-05) ran on 2026-10-10
on the same two bindings. Background, method, counts and the full triage are in
[interop-notes.md](interop-notes.md#moq-lite-06-first-sweep-against-the-moq-cli-b8b0d235) and
[interop-notes.md](interop-notes.md#moq-lite-06-second-sweep-l2a-against-the-moq-cli-b8b0d235).

The peer is the CLIENT that dials the runner; the runner is the server and the subscriber.
So every item is about what the CLI does as a client and as a publisher. Source line numbers
are from the read-only checkout at `b8b0d235` (`rs/moq-net/src/lite/`); draft line numbers are
in `docs/draft-lcurley-moq-lite-06.txt`. All three items reproduced identically in the three
single-run sweeps and in every group run holding the scenario.

Confidence labels: **confirmed** (wire evidence in every sweep and the source agrees),
**observed** (wire or log evidence, no scored row), **question** (the reading of the draft is
open; worded as a question).

## Status in the first sweep

| Item | Status | Rows |
|------|--------|------|
| ML-01 a server's SETUP Path is accepted | Confirmed | L06-7-3-2-MUST-126 fail (native QUIC; not judged on WebTransport) |
| ML-02 a server's SETUP Role is accepted | Confirmed | L06-7-3-3-MUST-131 fail (both transports) |
| ML-03 a Message Length mismatch resets the stream instead of closing the session | Confirmed (SHOULD) | L06-7-1-SHOULD-107 fail (both transports) |
| ML-Q1 which stream code answers a protocol violation on one stream? | Question | none (L06-3-6-MUST-023 and L06-7-2-MUST-108 pass with CANCELLED) |
| ML-04 a subscriber's PROBE target resets the Probe Stream | Confirmed | L06-5-1-5-MUST-072 fail (both transports) |
| ML-05 an oversize GOAWAY URI resets only the Goaway Stream | Confirmed | L06-7-18-MUST-179 fail (both transports) |

Every other row the 19 first-sweep scenarios judge passed: in the final first sweep 25 of the
30 bound rows pass on native QUIC and 24 on WebTransport (row 027 since the runner fix
`ffecfed`). The second sweep adds seven scenarios and nine bound rows: L06-7-12-MUST-NOT-163,
L06-7-12-MUST-170, L06-7-16-MUST-177 and L06-5-1-3-MUST-066 pass, the two items above fail, and
L06-5-1-5-MUST-075, L06-5-1-6-MUST-NOT-077 and L06-7-18-MUST-186 stay `not_run` (see the
observations at the end). The rows that stay `not_run` do not apply to this peer or transport;
they are listed in the interop notes, not here.

## Items

### ML-01 The client accepts a Path parameter in the server's SETUP (confirmed)

- **Row:** L06-7-3-2-MUST-126 (fail on native QUIC; on WebTransport the row is not judged,
  because a Path there is also a URI-binding violation that the runner would have to close
  on itself).
- **Scenario:** `l06-setup-server-path`.
- **Draft:** lines 1648-1651: the Path parameter "MUST NOT be sent on a binding whose
  handshake carries a request URI (bindings 2 and 4), and only the client sends it; a
  receiver MUST close the session with a PROTOCOL_VIOLATION on either violation".
- **Observed:** the runner's SETUP stream `01 04 01 02 01 2f` (one parameter: Path = "/") is
  delivered with FIN at 0 ms. The CLI logs `received peer setup setup=Setup { probe: None,
  path: Some("/"), role: None, cost: None, hop: None }` and `peer does not support probing;
  skipping probe stream`, then keeps the session open: no close by the end of the 3 s
  allowance (`context_complete ... peer_closed_early=false elapsed_ms=3000`; run
  `run-18dd1384c1d5379a`, and the same in every repeat and group run).
- **Where:** `setup.rs` lines 200-207 decode the Path for either role, and `subscriber.rs`
  lines 739-744 store the peer's SETUP without checking who sent it.
- **Required:** a client that receives a SETUP carrying Path (any value, empty included)
  closes the session with PROTOCOL_VIOLATION (session code 0x3).

### ML-02 The client accepts a Role parameter in the server's SETUP (confirmed)

- **Row:** L06-7-3-3-MUST-131 (fail on both transports).
- **Scenario:** `l06-setup-server-role`.
- **Draft:** lines 1692-1693: Role: "Only the client sends it; a client that receives one
  MUST close the session with a PROTOCOL_VIOLATION."
- **Observed:** the runner's SETUP `01 04 01 03 01 00` (Role = 0, Both, so only the presence
  is wrong) is delivered; the CLI logs `received peer setup setup=Setup { probe: None, path:
  None, role: None, cost: None, hop: None }` and keeps the session open for the 3 s
  allowance (`run-18dd13858a22b7c6` native, `run-18dd13858a22b7ab` WebTransport, and every
  repeat).
- **Where:** `setup.rs` line 208 maps the value through `Role::from_code` (lines 109-115),
  which turns 0 and any unknown value into `None`, so the parameter's presence is lost before
  `subscriber.rs` lines 739-744 store it; nothing checks for a Role from a server.
- **Required:** a client that receives a SETUP carrying Role (any value) closes the session
  with PROTOCOL_VIOLATION. Keep "unknown value means Both" for the
  server side (draft lines 1685-1686), but record that the parameter was present.

### ML-03 A Message Length mismatch resets the stream instead of closing the session (confirmed; SHOULD)

- **Row:** L06-7-1-SHOULD-107 (fail on both transports). Consequence: the session half of
  L06-4-4-MUST-027 on `l06-errors-code-space` is missing; since the runner fix `ffecfed` row
  027 takes that half from the closes of the setup probes instead (catalog rationale), so the
  row no longer depends on this item.
- **Scenario:** `l06-errors-code-space`.
- **Draft:** lines 1496-1498: "An implementation SHOULD close the connection with a
  PROTOCOL_VIOLATION if it receives a message with an unexpected length."
- **Observed:** the runner opens an Announce stream with `01 04 00 6c 31 64` (STREAM_TYPE
  0x1, Message Length 4 covering an empty Broadcast Path Prefix plus three trailing bytes).
  The CLI logs `WARN moq_net::lite::message: decode failed err=long buffer` and `WARN
  moq_net::lite::publisher: control stream error err=long buffer`, resets the stream with
  CANCELLED (0x1) and sends STOP_SENDING 0; the session stays open for the 3 s allowance
  (`run-18dd1392babf5deb` native, `run-18dd1392babf5de1` WebTransport, every repeat).
- **Where:** `message.rs` lines 58-66 detect the trailing bytes (`DecodeError::Long`), but the
  control-stream task only logs a per-stream error (`publisher.rs` lines 276-281); the
  comments at lines 329 ("A decode error propagates to the caller, which logs and continues")
  and 343-344 ("The control loop only logs per-stream errors") state that design.
- **Required (SHOULD):** close the session with PROTOCOL_VIOLATION on a message whose length
  disagrees with its content, at least on the control streams (Announce, Subscribe).

### ML-Q1 Which stream code answers a protocol violation confined to one stream? (question)

- **Rows:** none (L06-3-6-MUST-023 and L06-7-2-MUST-108 accept any reset and pass).
- **Scenarios:** `l06-subscribe-invalid-frame-bounds`, `l06-errors-unknown-stream-type`.
- **Draft:** lines 461-462 ("treat a violation of either rule as a protocol violation and
  reset the stream"), 1518-1519 (reset an unknown stream type); Table 3 (lines 676-754) has no
  stream-level PROTOCOL_VIOLATION, and lines 755-756 say CANCELLED (0x1) is "a routine
  cancellation", 0x0 an INTERNAL_ERROR.
- **Observed:** the CLI resets both the SUBSCRIBE with Group End 0 and Frame End 1 and the
  bidirectional stream of unknown type 0x3f with CANCELLED (0x1), plus STOP_SENDING 0.
- **Question:** is CANCELLED, which the draft calls a routine unsubscribe, the intended code
  for a protocol violation on one stream, or should it be INTERNAL_ERROR (0x0, what the
  runner's scripted publisher uses) or a new stream code? Not a defect against the current
  text; a draft clarification would settle it.

### ML-04 A subscriber's PROBE target resets the Probe Stream (confirmed)

- **Row:** L06-5-1-5-MUST-072 (fail on both transports).
- **Scenario:** `l06-probe-report`.
- **Draft:** lines 1153-1164 (section 5.1.5): "The subscriber sends a PROBE message with a target
  bitrate on the bidirectional stream. The subscriber MAY send additional PROBE messages on the
  same stream to update the target bitrate; the publisher MUST treat each PROBE as a new target
  to attempt." A publisher that advertised Report but not Increase "ignores the target and only
  reports"; reset is for a publisher that advertised no Probe capability (lines 1166-1167).
- **Observed:** the CLI advertises Probe level Report in its SETUP (parameter 1 = 01). The runner
  opens a Probe Stream with `04 05 80 3d 09 00 00` (target 4 000 000 bit/s, RTT 0). The CLI logs
  `WARN moq_net::lite::publisher: probe stream error err=short buffer`, resets the stream with
  MALFORMED_TRACK (0x12) about 1 ms later and sends STOP_SENDING 0, and keeps the session open
  (`run-18dd35ed00c3a457` native QUIC, `run-18dd360c95f22ca1` WebTransport, and every repeat).
- **Where:** `publisher.rs` lines 424-436: `ProbeServe::poll_probe` never decodes anything the
  subscriber writes. It polls `stream.reader.poll_closed`, which only accepts a FIN, so the first
  byte of a target message is a read error that ends the stream. (`probe.rs` lines 15-40 can
  decode the message; it is not used on this path.)
- **Required:** read the PROBE messages the subscriber writes on a Probe Stream as targets (at
  Report level the value is ignored) and keep reporting; only a FIN or reset from the subscriber
  ends the stream.

### ML-05 An oversize GOAWAY URI resets only the Goaway Stream (confirmed)

- **Row:** L06-7-18-MUST-179 (fail on both transports).
- **Scenario:** `l06-goaway-oversize`.
- **Draft:** lines 2375-2379 (section 7.18): "The URI MUST NOT exceed 8,192 bytes; a receiver
  MUST treat a longer URI as a protocol violation and MAY reject it based on the length prefix
  alone." Closing the session with PROTOCOL_VIOLATION is how the draft treats a protocol
  violation elsewhere in the same section (lines 2386, 2391-2392).
- **Observed:** the runner opens a Goaway Stream whose message claims a URI of 8193 bytes and
  carries them (`05 60 03 60 01 61 61 ...`). The CLI resets the Goaway Stream with CANCELLED
  (0x1) 2 ms later, sends STOP_SENDING 0, and the session stays open for the whole 3 s allowance
  (`context_complete ... peer_closed_early=false elapsed_ms=3000`; `run-18dd35ee455d81b7` native
  QUIC, WebTransport the same).
- **Where:** `goaway.rs` lines 28-31 reject a length above 8192 from the prefix alone
  (`DecodeError::InvalidValue`), as the draft allows, but `publisher.rs` lines 329-332 say "A
  decode error propagates to the caller, which logs and continues: a malformed GOAWAY must not
  tear down the session it is trying to drain", so only the stream ends. The same design as ML-03.
- **Required:** close the session with PROTOCOL_VIOLATION when a GOAWAY URI exceeds 8,192 bytes
  (a draining session can still be closed by a protocol violation; the draft does not exempt it).

## Observations that are not items

- After the runner's STOP_SENDING with an unregistered (0x4d1) or reserved (0x2a) code on a
  Group stream, that stream is reset with the same code: the QUIC stack copying the
  STOP_SENDING code into RESET_STREAM (RFC 9000 section 3.5), not a moq-lite meaning given to
  the code. Rows 030, 032 and 033 pass.
- A SUBSCRIBE for an uncovered broadcast is reset with UNROUTABLE (0x36) and one for an
  unknown track of the announced broadcast with NOT_FOUND (0x33): no on-demand track creation
  (row 062 case (b)). No other NOT_FOUND reset appeared on `0.m4s`.
- The CLI opens no Probe stream: the runner's SETUP has no Probe parameter, so the CLI logs
  `peer does not support probing; skipping probe stream`. Its Role=Publisher Probe behavior
  is therefore not exercised.
- The CLI ends the session at once on any inbound GOAWAY: an application close with code 0
  (NO_ERROR, reason `dropped`) within about 1 ms of the GOAWAY, after resetting the Goaway
  Stream with CANCELLED. With a URI on another host it also logs `GOAWAY redirect refused: the
  GOAWAY redirect leaves the current host` (the local-policy check of draft lines 2379-2382, rows
  181 and 182, which are not scored); with a URI on the runner's own host it still ends the
  session and does not dial it (`--connect-once`). That is the graceful shutdown the draft asks
  for, not a defect. Because the first GOAWAY ends the session, rows L06-5-1-6-MUST-NOT-077 (no
  new streams after a GOAWAY) and L06-7-18-MUST-186 (a second GOAWAY closes the session) stay
  `not_run` against this peer; the scripted publisher exercises both in the runner's tests, and
  L2b will add the cases that need a publisher the CLI cannot be.
- `publisher.rs` lines 342-351 close the session with PROTOCOL_VIOLATION on a second GOAWAY
  stream (the behavior row 186 asks for); it cannot be reached through a live run for the reason
  above.
- Row L06-5-1-5-MUST-075 (a publisher that advertised no Probe capability resets the Probe
  Stream) is `not_run`: this CLI advertises Report, so the level None path is not exercised.
- The track and fetch rows pass: TRACK_INFO for `0.m4s` is `{priority 60, max age 30000 ms,
  timescale 15360}` and identical on repeated lookups; a FETCH for a group the subscription just
  delivered returns exactly the requested frames (whole group, the first two, all but the first)
  and FINs, and a FETCH for group 4 000 000 000 is reset.
- L2b: the CLI sends no QUIC datagram (datagrams are a permission it never uses), so row
  L06-6-4-MUST-NOT-105 (datagram body at most 1200 bytes) is `not_run` against it. Groups arrive
  about 0.95 s apart and never overlap, even with a 1 ms Subscriber Max Age, so the priority and
  expiry rows 083, 086 and 090 cannot be provoked on loopback and are classified NotTestable.
  Nothing new for upstream; the list stays local until the reference publisher (L2c) and the
  final review are done.
- L2c: the reference publisher (a conforming lite publisher) was swept against the same runner on both transports and does
  not contradict any item here: the rows ML-01..ML-05 concern are judged on it exactly as the draft reads, and it passes them
  (a defect mode reproduces each failing behavior, for example `probe-resets-on-target` for ML-04 and
  `goaway-oversize-logged` for ML-05). Nothing new for upstream; the list stays local until the final review is done and
  the user decides to send it.
