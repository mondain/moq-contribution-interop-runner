# Picoquic Transport Migration Implementation Plan

Status (2026-10-01): partially implemented. Strict WebTransport admission and session mapping are committed; picoquic parity and full draft suites over WebTransport are not complete.

Backend update (2026-10-01): picoquic is the sole supported backend and local
test peer. Quiche options, dependencies, legacy sources, and parity targets
have been retired; quiche references below describe the earlier migration.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace production quiche with pinned picoquic for native draft-18/21 sessions, then add strict draft-specific WebTransport over H3zero without changing publisher-MOQT scoring.

**Architecture:** Preserve `SessionTransport` so the scenario engine remains transport-neutral. Establish native parity before removing quiche from the runtime. Admit HTTP/3 CONNECT only after the referenced WebTransport profile is proven, then map each accepted session through a bounded project-owned adapter. Quiche may remain an independent test peer only.

**Tech Stack:** C++20, C11, picoquic, H3zero, picotls, CMake FetchContent, GoogleTest, CTest, libFuzzer, Docker

**Spec:** `docs/moq-contribution-interop-runner-design.md`; protocol authority: `docs/draft-ietf-moq-transport-18.txt` and `docs/draft-ietf-moq-transport-21.txt` (read these before secondary sources). Draft 18 references WebTransport-over-HTTP/3 revision 15; draft 21 references revision 16.

## Global Constraints

- Both native QUIC and WebTransport use picoquic in the production runner; there is no transparent backend fallback.
- A run accepts its exact `moqt-18` or `moqt-21` ALPN/application protocol and never falls back to another draft.
- QUIC DATAGRAM support is required for both drafts. Absence of RESET_STREAM_AT alone is not a native-MOQT failure.
- H3zero handles HTTP/3 framing and QPACK; project code owns strict WebTransport admission, session mapping, limits, and evidence.
- Draft 18 follows WebTransport-over-HTTP/3 revision 15; draft 21 follows revision 16. Do not merge behavioral differences for convenience.
- Malformed or missing required WebTransport negotiation is rejected; legacy settings, tokens, or headers are not conformance.
- H3zero changes are small, pinned, tested build-time patches; never modify the sibling picoquic checkout.
- Transport negotiation and adapter failures remain distinct from publisher-MOQT requirement failures.
- Existing native draft-18 and draft-21 scenarios retain equivalent evidence and scores before quiche runtime removal.
- The final Docker runtime contains picoquic and its TLS dependencies but no quiche runtime dependency.
- Do not create new Markdown files without authorization; update existing documentation. Do not add a Codex commit tagline.

## Review Focus

- Wrong-draft or invalid ALPN must fail before a publisher session exists; Task 2 tests no MOQT result is scored.
- Only legacy WebTransport settings or CONNECT token must cause an HTTP error; Task 4 tests no session admission.
- Split or overlong session-ID prefix must not lose bytes or allocate beyond the cap; Task 5 tests fragmentation and malformed varints.
- Another session's HTTP datagram quarter-stream ID must not enter MOQT decoding; Task 5 tests cross-session injection.
- Native success followed by WebTransport failure must retain separate endpoint, evidence, and score state; Task 6 tests sequential runs and restart persistence.

## File Map and Dependency Order

1. `cmake/Dependencies.cmake`, `cmake/Picoquic.cmake`, and `CMakeLists.txt` own pinned dependencies and link isolation; `version.h` owns provenance.
2. `src/transport/picoquic_native_listener.cpp`, `picoquic_connection.cpp`, and its private header own UDP dispatch, callbacks, bounded event queue, stream operations, and timers behind existing public transport headers.
3. `src/transport/webtransport_connect.cpp` owns draft-profile settings and CONNECT validation; `cmake/patches/picoquic-webtransport-strict.patch` removes H3zero legacy acceptance.
4. `src/transport/webtransport_session.cpp` owns framing and close isolation; `webtransport_listener.cpp` exposes accepted sessions as `SessionTransport`.
5. The run manager, HTTP server, Dockerfile, and existing README own endpoint allocation, error status, packaging, and operator workflows.

Each task is a reviewer gate. Keep the old native backend buildable until Task 3 parity passes; then remove it from production targets. Quiche testing is optional and off for production Docker.

---

### Task 1: Pin picoquic and prove link isolation

**Files:** Create `cmake/Picoquic.cmake`, `tests/integration/picoquic_link_test.cpp`, `tests/integration/picoquic_dependency_contract.cmake`; modify `cmake/Dependencies.cmake`, `CMakeLists.txt`, `include/moq/interop/app/version.h`, `tests/unit/version_test.cpp`.

**Interfaces:** `moq-interop-picoquic` links `picoquic::picoquic-core` and `picoquic::picohttp-core`; build metadata exports `MOQ_INTEROP_PICOQUIC_REVISION` and `MOQ_INTEROP_PICOTLS_REVISION`. Pin picoquic `61fcd56ae0f069a1e98459e7a4965312a5190943` and its configured-default picotls `bfa67875982afc4c24f21e146cef4747fa189c2f`; verify both resolved Git HEADs at configure time.

```cmake
set(MOQ_INTEROP_PICOQUIC_REVISION "61fcd56ae0f069a1e98459e7a4965312a5190943")
set(MOQ_INTEROP_PICOTLS_REVISION "bfa67875982afc4c24f21e146cef4747fa189c2f")
```

- [ ] **Write failing tests:** `version_test.cpp` asserts the two exact revisions and no `quiche` key; `picoquic_link_test.cpp` calls `picoquic_create`, `picoquic_free`, and an H3zero public symbol; the dependency contract compares pinned and fetched HEADs and rejects mismatch.
- [ ] **Verify red:** `cmake -S . -B build -DMOQ_INTEROP_BUILD_TESTS=ON` then `cmake --build build --target moq-interop-unit-tests`; expected missing metadata/link target.
- [ ] **Implement:** Set `project(... LANGUAGES C CXX)`; declare the exact picoquic Git tag above; set `PICOQUIC_FETCH_PTLS=ON`, `PICOQUIC_FETCH_PTLS_TAG` to the exact picotls revision, `BUILD_HTTP=ON`, `BUILD_LOGLIB=ON`, and picoquic demo/test options OFF before `FetchContent_MakeAvailable`. Add revision definitions to `moq-interop-app`. Keep quiche temporarily for native parity.
- [ ] **Verify green:** `cmake -S . -B build -DMOQ_INTEROP_BUILD_TESTS=ON`, `cmake --build build -j 4`, `ctest --test-dir build --output-on-failure`; expected both new contracts and existing tests pass. If pinned dependencies fail to compile, stop this feasibility gate and report the exact error rather than silently repinning.
- [ ] **Commit:** Stage only these files; `git commit -m "build: pin picoquic transport dependencies"`.

### Task 2: Native picoquic handshake, UDP dispatch, and bounded events

**Files:** Create `src/transport/picoquic_connection_internal.h`, `src/transport/picoquic_connection.cpp`, `src/transport/picoquic_native_listener.cpp`, `tests/transport/picoquic_native_listener_test.cpp`; modify `CMakeLists.txt`. Reference existing `native_quic_listener.h`, `session_transport.h`, and `src/transport/quiche_native_listener.cpp` without changing their public contract.

**Interfaces:** Keep `NativeQuicListener::create(NativeQuicListenerConfig) -> NativeQuicListenerCreateResult`, `bound_endpoint() -> const BoundEndpoint&`, and `poll(std::size_t) -> std::vector<TransportEvent>`. Private callbacks queue the existing established, close, timeout, error, and overflow event variants; no callback decodes MOQT.

```cpp
const auto result = NativeQuicListener::create(config);
ASSERT_NE(result.listener, nullptr);
EXPECT_EQ(result.listener->bound_endpoint().address, config.bind_address);
// After a peer connects with the run's exact ALPN:
EXPECT_EQ(std::get<ConnectionEstablishedEvent>(result.listener->poll(1).at(0)).alpn,
          config.expected_alpn);
```

- [ ] **Write failing tests:** With a QUIC peer offer `moqt-18`, `moqt-21`, then `moqt-16`; the first two yield exactly one `ConnectionEstablishedEvent` with matching ALPN/CIDs and nonzero datagram limit, the third none. Flood more than `config.max_events` callbacks and expect one `EventQueueOverflowEvent`; close a run and rebind its UDP port.
- [ ] **Verify red:** `ctest --test-dir build -R picoquic-native-listener --output-on-failure`; expected missing target or handshake failure.
- [ ] **Implement:** Configure certificate/key, exact ALPN, datagram transport parameter, idle timer, address validation, and socket from `NativeQuicListenerConfig`; map setup failures to its error enum. Copy only capped callback payloads into a capped queue. Poll receive/send/timer work with existing per-poll bounds. A temporary *build* switch may select the parity implementation; no runtime fallback.
- [ ] **Verify green:** Run focused CTest, existing native listener tests, wrong-ALPN/no-score assertion, and full CTest.
- [ ] **Commit:** Stage Task 2 files; `git commit -m "feat: accept native MoQT sessions with picoquic"`.

### Task 3: Complete SessionTransport parity and remove runtime quiche

**Files:** Modify `src/transport/picoquic_connection.cpp`, `src/transport/picoquic_native_listener.cpp`, `CMakeLists.txt`, `tests/transport/picoquic_native_listener_test.cpp`, `tests/e2e/draft18-native-moqxr.sh`; create `tests/transport/picoquic_connection_test.cpp`, `tests/e2e/draft21-native-moqxr.sh`. Retain `tests/support/quiche_client.cpp` and quiche link checks only as optional test tooling. Remove `src/transport/quiche_connection.cpp` and `quiche_native_listener.cpp` from production target only.

**Interfaces:** All eight `SessionTransport` operations retain signatures in `session_transport.h`; map picoquic outcomes to `TransportStatus` without treating `WouldBlock`, `Partial`, peer reset, or peer stop as success.

```cpp
const auto too_large = session.send_datagram(bytes_over_negotiated_limit);
EXPECT_EQ(too_large.status, TransportStatus::DatagramTooLarge);
EXPECT_EQ(too_large.accepted, 0U);
```

- [ ] **Write failing tests:** Exercise `open_bidi/open_uni`, partial and FIN writes, reset, stop, datagram at negotiated maximum and one byte above, local/peer close, idle timeout, and repeated `poll(1)` without loss. Compare draft-18 and draft-21 native scripted publisher traces by event class, requirement ID, and score, ignoring timing/CIDs.
- [ ] **Verify red:** `ctest --test-dir build -R 'picoquic-connection|draft(18|21)-native' --output-on-failure`; expected missing operations or parity mismatch.
- [ ] **Implement:** Translate stream IDs/callbacks exactly; cap events and datagrams before allocation; use RESET_STREAM_AT only when negotiated, otherwise ordinary RESET_STREAM; make close idempotent. Remove quiche sources and link from production targets. Gate quiche Rust build, tests, and independent peer behind `MOQ_INTEROP_BUILD_QUICHE_TEST_PEER` default OFF.
- [ ] **Verify green:** Focused tests, full CTest, both native E2E scripts, and `ldd build/moq-interop-runner` plus CMake link-command inspection; expected no quiche/BoringSSL/Rust production link and identical native evidence/scores. Differences fail this migration gate.
- [ ] **Commit:** Stage Task 3 files; `git commit -m "feat: complete picoquic native transport parity"`.

### Task 4: Enforce draft-specific WebTransport admission

**Files:** Create `cmake/patches/picoquic-webtransport-strict.patch`, `src/transport/webtransport_connect.h`, `src/transport/webtransport_connect.cpp`, `tests/transport/webtransport_connect_test.cpp`, `tests/integration/webtransport_h3_admission_test.cpp`; modify `cmake/Picoquic.cmake`, `CMakeLists.txt`.

**Interfaces:** Define `enum class WebTransportProfile { Draft18Wt15, Draft21Wt16 };` and `ConnectDecision validate_connect(const H3Request&, const PeerCapabilities&, const RunEndpoint&, WebTransportProfile);`. Define these private value types in `webtransport_connect.h`: `H3Request` holds method/protocol/scheme/authority/path and raw headers; `PeerCapabilities` holds relevant SETTINGS and QUIC transport-parameter flags; `RunEndpoint` holds required authority/path, origin policy, and exact selected MOQT protocol. `ConnectDecision` holds HTTP status, selected protocol, and bounded header evidence, never an accepted session without all required capabilities.

```cpp
const auto rejected = validate_connect(request_with_legacy_token, peer_capabilities,
                                       run_endpoint, WebTransportProfile::Draft21Wt16);
EXPECT_GE(rejected.http_status, 400);
EXPECT_FALSE(rejected.accepted());
```

- [ ] **Write failing tests:** For both profiles accept only exact `CONNECT`, `webtransport-h3`, `https`, authority/path, allowed Origin, and `WT-Available-Protocols` parsed as an RFC 8941 list of strings containing exact `moqt-18` or `moqt-21`; `WT-Protocol` must equal one offered value. Feed multiple offers, duplicate/missing/malformed fields, wrong draft/path/token, old settings, missing QUIC/H3 datagrams, missing RESET_STREAM_AT, and escaped error text. Assert HTTP error, no session, no MOQT score.
- [ ] **Verify red:** `ctest --test-dir build -R 'webtransport-connect|webtransport-h3-admission' --output-on-failure`; expected missing target or false acceptance.
- [ ] **Implement:** Read the local MoQT text files first and then each referenced WT-H3 revision. Parse the whole structured field; reject non-string members/trailing garbage. Apply a narrow H3zero patch before compilation to stop emitting or accepting legacy settings/tokens as sufficient; fail the build if it does not apply cleanly and assert patched source in dependency contract. Keep draft-specific differences explicit.
- [ ] **Verify green:** Focused tests, dependency contract, full CTest, and independent HTTP/3 peer wire-level settings/rejection assertions.
- [ ] **Commit:** Stage Task 4 files; `git commit -m "feat: enforce strict WebTransport admission profiles"`.

### Task 5: Map WebTransport sessions to bounded transport events

**Files:** Create `include/moq/interop/transport/webtransport_listener.h`, `src/transport/webtransport_listener.cpp`, `src/transport/webtransport_session.h`, `src/transport/webtransport_session.cpp`, `tests/transport/webtransport_session_test.cpp`, `tests/fuzz/webtransport_stream_fuzz.cpp`; modify `CMakeLists.txt`.

**Interfaces:** `WebTransportListener::create(WebTransportListenerConfig) -> WebTransportListenerCreateResult` follows the native create/bound-endpoint pattern. Each accepted CONNECT yields one `std::unique_ptr<SessionTransport>` keyed by QUIC connection and CONNECT stream ID. The adapter implements all `SessionTransport` methods; private `ingest_stream(StreamId, std::span<const std::byte>, bool)` and `ingest_datagram(std::span<const std::byte>)` return bounded parse results and never expose H3 framing to MOQT.

```cpp
adapter.ingest_stream(peer_stream_id, first_prefix_byte, false);
EXPECT_TRUE(adapter.poll(1).empty());
adapter.ingest_stream(peer_stream_id, remaining_prefix_and_payload, false);
EXPECT_EQ(std::get<StreamDataEvent>(adapter.poll(1).at(0)).data, expected_payload);
```

- [ ] **Write failing tests:** Feed bidi and uni streams byte-by-byte and coalesced; expect stream type/session-ID prefix stripped before `StreamDataEvent`. Feed wrong session ID, noncanonical/overlong/truncated varints, forbidden direction, cross-session HTTP datagram quarter-stream ID, maximum and oversize datagrams, reset/stop translation, graceful/abrupt close, and two concurrent sessions. Rejected input creates no MOQT event; closing one session leaves the other alive.
- [ ] **Verify red:** `ctest --test-dir build -R webtransport-session --output-on-failure`; expected missing adapter or failing assertions.
- [ ] **Implement:** Retain per-stream prefix state until complete; cap prefix/payload before append; key mapping by accepted CONNECT ID and QUIC connection; prefix outbound streams/datagrams once; translate reset/stop errors both ways; release only the selected session. Keep H3 and MOQT parsers separate.
- [ ] **Verify green:** Focused CTest, 60-second libFuzzer smoke with single-byte splits/malformed prefixes, ASan/UBSan CTest, and full CTest; expected no unbounded allocation or stale event after close.
- [ ] **Commit:** Stage Task 5 files; `git commit -m "feat: map WebTransport sessions into transport events"`.

### Task 6: Expose WebTransport runs, package, and black-box verify

**Files:** Modify `include/moq/interop/app/native_run_manager.h`, `src/app/native_run_manager.cpp`, `include/moq/interop/http/server.h`, `src/http/server.cpp`, `src/http/json.cpp`, `Dockerfile`, `compose.yaml`, `README.md`, `docs/moq-contribution-interop-runner-design.md`, `CMakeLists.txt`; create `tests/e2e/draft18-webtransport-smoke.sh`, `tests/e2e/draft21-webtransport-smoke.sh`, `tests/integration/webtransport_run_api_test.cpp`.

**Interfaces:** `POST /v1/runs` with `transport=webtransport` allocates an HTTPS URL/authority/path and exact `moqt-18` or `moqt-21` protocol; native retains its endpoint shape. Rename `NativeRunManager` to `RunManager` only if all callers migrate together. A transport error is transport evidence and failed/unsupported run state, never a failed publisher-MOQT requirement.

```json
{"draft":21,"transport":"webtransport"}
```

The response includes a concrete HTTPS publisher URL and selected `moqt-21`; requesting draft 18 instead selects `moqt-18`. A failed CONNECT must leave every publisher requirement unscored.

- [ ] **Write failing tests:** Start each draft/WebTransport combination, inspect endpoint/protocol, connect a literal-byte scripted publisher, and compare applicable requirement IDs/scores with native. Test wrong protocol, occupied port, port release, native-pass then WT-fail isolation, and evidence persistence after restart.
- [ ] **Verify red:** `ctest --test-dir build -R webtransport-run-api --output-on-failure` and both new E2E scripts; expected HTTP 422 unsupported and no WT endpoint.
- [ ] **Implement:** Route each run to native or WT listener without fallback; report HTTPS URL/path/authority/protocol and certificate trust instructions. Keep result schema/weights; add transport evidence. Remove Rust/quiche build stages and runtime artifacts from production Docker; retain optional quiche peer only in tests. Update existing README with exact local, Docker, manual-publisher, and adapter commands. Use `../moq-rs/moq-pub` only if it actually negotiates a target draft; otherwise report unsupported. Use moqxr as another fixture, not as an oracle.
- [ ] **Verify green:** `cmake --build build -j 4`, `ctest --test-dir build --output-on-failure`, both native and both WT E2E scripts, sanitizer/fuzz smoke, `bash scripts/container-build.sh build`, `docker run --rm moq-contribution-interop-runner:local --version`, and final image linked-library inspection. Run `git diff --check`; confirm native score parity and no quiche/Rust production dependency.
- [ ] **Commit and hand off:** Stage only Task 6 files; `git commit -m "feat: run draft 18 and 21 over WebTransport"`. Review the whole branch against this plan and the checked-in drafts; push `feature/interop-runner` after verification and report exact test outcomes/unsupported publisher combinations.
