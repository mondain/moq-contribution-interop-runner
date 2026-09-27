# WebTransport Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the applicable draft-18 and draft-21 publisher suites over an independently implemented HTTP/3 WebTransport server transport.

**Architecture:** quiche supplies QUIC and HTTP/3 events; this project validates extended CONNECT, MOQT application-protocol selection, WebTransport stream/session mapping, datagrams, and close behavior behind the existing `SessionTransport` interface.

**Tech Stack:** C++20, quiche C API, HTTP/3, WebTransport, GoogleTest, libFuzzer, Docker

**Spec:** `docs/moq-contribution-interop-runner-design.md`

## Global Constraints

- WebTransport code contains no MOQT draft behavior beyond selecting the negotiated application protocol.
- Require exact draft-specific application-protocol negotiation and record all request headers as evidence.
- Enforce session, stream, datagram, and buffered-byte limits before allocation.
- Reuse scenario/evaluator IDs only when transport-independent requirements are identical.
- Report transport-specific failures without converting them into publisher protocol failures.

## Review Focus

- CONNECT without required WebTransport settings must receive an HTTP error, not create a run session.
- A stream carrying another WebTransport session ID must be rejected.
- Partial session-ID framing must survive arbitrary QUIC read boundaries.
- Oversized datagrams must not allocate or enter the MOQT decoder.
- Closing one WebTransport session must not close another QUIC connection/session accidentally.

---

### Task 1: Extended CONNECT listener

**Files:** Create `src/transport/quiche_h3_listener.cpp`, `src/transport/webtransport_connect.cpp`, and `tests/integration/webtransport_connect_test.cpp`.

**Interfaces:** Produces `validate_connect(const H3Request&, const RunConfig&) -> ConnectDecision` and accepts valid sessions into `SessionTransport`.

```cpp
ConnectDecision validate_connect(const H3Request& request, const RunConfig& run);
std::unique_ptr<SessionTransport> accept_webtransport(const AcceptedConnect&, QuicheConnection&);
```

- [ ] Write tests for method/protocol/scheme/authority/path, required H3 settings, exact `moqt-18` and `moqt-21` offers, multiple offers, malformed structured fields, wrong run token, and escaped error bodies.
- [ ] Confirm tests fail, implement strict CONNECT validation and 2xx/4xx responses, then rerun under sanitizers.
- [ ] Commit with `git commit -m "feat: accept MoQT WebTransport sessions"`.

### Task 2: Streams, datagrams, and session close

**Files:** Create `src/transport/webtransport_session.cpp`, `tests/integration/webtransport_session_test.cpp`, and `tests/fuzz/webtransport_stream_fuzz.cpp`.

**Interfaces:** Implements the existing `SessionTransport` operations for WebTransport session streams and datagrams.

```cpp
class WebTransportSession final : public SessionTransport {
public:
    std::vector<TransportEvent> poll() override;
    TransportResult write(StreamId, std::span<const std::byte>, bool fin) override;
};
```

- [ ] Write tests for every stream direction, fragmented/coalesced session IDs, wrong session IDs, reset/stop propagation, datagrams, oversized input, graceful close, abrupt QUIC close, and two concurrent sessions.
- [ ] Confirm focused tests fail, implement bounded stream mapping and close state, then run a 60-second fuzz smoke.
- [ ] Commit with `git commit -m "feat: map WebTransport events to test sessions"`.

### Task 3: Draft suites over WebTransport

**Files:** Create `tests/e2e/draft18-webtransport-smoke.sh` and `tests/e2e/draft21-webtransport-smoke.sh`; modify `src/scenarios/engine.cpp`, `src/http/server.cpp`, and `compose.yaml`.

**Interfaces:** Produces WebTransport run endpoints and executes existing applicable scenario/evaluator sets.

- [ ] Add tests proving transport-independent outcome parity, transport-specific requirement separation, mismatched protocol rejection, assigned-port release, and persistent evidence after restart.
- [ ] Run both scripts; expect endpoint creation failure before registration.
- [ ] Register WebTransport endpoint allocation and literal-byte scripted publishers for each draft.
- [ ] Run full CTest, sanitizers, WebTransport fuzz smoke, and all four native/WebTransport Docker scripts.
- [ ] Commit with `git commit -m "feat: validate publishers over WebTransport"`.
