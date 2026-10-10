# moq CLI (moq-lite-06) Punch List

This is a work list for an agent fixing the `moq` CLI of moq-dev/moq (its moq-lite session
layer, `rs/moq-net/src/lite/`) so that it conforms to moq-lite-06
(`docs/draft-lcurley-moq-lite-06.txt`). Every ML- item comes from driven runs of the interop
runner in this repository against `moq 0.14.1` built from moq-dev/moq
`b8b0d235a99bddb9043f453c46c958362d6c9247`, publishing an ffmpeg fMP4 source through the
bundled `adapters/moq-lite/run.sh` (`moq ... --connect-version moq-lite-06 --broadcast
interop.hang import fmp4`). The first sweep ran on 2026-10-09 (22:21 to 22:38 -07:00) on
native QUIC (`moql://`) and WebTransport (`https://`); background, method, counts and the
full triage are in
[interop-notes.md](interop-notes.md#moq-lite-06-first-sweep-against-the-moq-cli-b8b0d235).

The peer is the CLIENT that dials the runner; the runner is the server and the subscriber.
So every item is about what the CLI does as a client and as a publisher. Source line numbers
are from the read-only checkout at `b8b0d235` (`rs/moq-net/src/lite/`); draft line numbers are
in `docs/draft-lcurley-moq-lite-06.txt`. All three items reproduced identically in the three
single-run sweeps and in every group run (eight per transport).

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

Every other row the 19 scenarios judge passed: in the final sweep 25 of the 30 bound rows pass
on native QUIC and 24 on WebTransport (row 027 since the runner fix `ffecfed`). The rows that
stay `not_run` do not apply to this peer or transport; they are listed in the interop notes,
not here.

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
