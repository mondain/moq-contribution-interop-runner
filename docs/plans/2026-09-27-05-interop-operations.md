# Publisher Integration and Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the validator usable with arbitrary publishers, add black-box `moqxr` coverage, finish result exports and the accessible report, and document local, Docker, and CI operation.

**Architecture:** A versioned process contract separates publisher launch/configuration from protocol expectations. The HTTP and export layers read immutable stored results; adapters contribute logs and identity only.

**Tech Stack:** C++20, JSON, TAP 14, SQLite3, Docker Compose, CTest, shell adapters

**Spec:** `docs/moq-contribution-interop-runner-design.md`

## Global Constraints

- Adapters never supply expected protocol behavior or alter scores.
- `moqxr` remains an external binary/container and no source is imported.
- Manual observed mode must work without any adapter.
- HTML uses high-contrast colors plus text labels and works without JavaScript.
- The HTTP service is unauthenticated and documented for trusted-network use only.

## Review Focus

- A publisher exits before connecting: the run must be `ERROR`, retain logs, and release its port.
- Adapter stdout contains invalid UTF-8 or HTML: JSON and HTML must remain valid and escaped.
- A client disconnects while events are paginated: immutable ordering and cursors must remain stable.
- TAP names containing punctuation must not corrupt the plan or diagnostics.
- Container shutdown during a run must preserve finalized transactions and recover an active run explicitly.

---

### Task 1: Versioned publisher-driver contract

**Files:** Create `include/moq/interop/app/publisher_driver.h`, `src/app/publisher_driver.cpp`, `adapters/contract.schema.json`, `tests/unit/publisher_driver_test.cpp`, and `tests/support/fake_driver.sh`.

**Interfaces:** Produces `PublisherDriver::start(const DriverRequest&)`, `poll`, `stop`, and `DriverResult`; request fields include endpoint, draft, transport, run/scenario IDs, namespace, tracks, fixture, TLS material, deadlines, and log directory.

```cpp
struct DriverRequest { RunId run_id; std::string scenario_id; std::string endpoint; DraftVersion draft; TransportKind transport; std::filesystem::path fixture; std::filesystem::path log_dir; };
class PublisherDriver { public: DriverHandle start(const DriverRequest&); DriverStatus poll(DriverHandle); DriverResult stop(DriverHandle); };
```

- [ ] Write tests for full environment/JSON serialization, missing executable, early exit, timeout, SIGTERM then SIGKILL escalation, invalid UTF-8 log bytes, and log SHA-256 metadata.
- [ ] Confirm focused tests fail, implement bounded process control without a shell command string, and rerun tests.
- [ ] Commit with `git commit -m "feat: define publisher driver contract"`.

### Task 2: `moqxr` black-box adapter and Compose matrix

**Files:** Create `adapters/moqxr/run.sh`, `adapters/moqxr/adapter.json`, `tests/e2e/moqxr-matrix.sh`; modify `compose.yaml`.

**Interfaces:** Consumes the driver contract and a mounted `openmoq-publisher`; produces publisher logs and exit metadata only.

- [ ] Write adapter contract tests for draft 18/21, native/WebTransport selection, input fixture, TLS trust, unsupported combinations, and paths containing spaces.
- [ ] Run contract tests before the adapter exists and confirm failure.
- [ ] Implement argument-array construction for the documented `moqxr` CLI without reading its internal source during execution.
- [ ] Run the supported matrix; assert only harness health, completed runs, and persisted row-level results, not predeclared publisher passes.
- [ ] Commit with `git commit -m "test: add moqxr black-box interop matrix"`.

### Task 3: Complete API, JSON, and TAP exports

**Files:** Create `include/moq/interop/http/result_schema.h`, `src/http/tap.cpp`, `tests/integration/result_export_test.cpp`; modify `src/http/json.cpp` and `src/http/server.cpp`.

**Interfaces:** Produces schema-versioned complete JSON and scenario-level TAP 14 at `/results/{id}.json` and `/results/{id}.tap`.

```cpp
nlohmann::json serialize_result(const RunRecord&, std::string_view schema_version);
std::string serialize_tap14(const RunRecord&);
```

- [ ] Add golden responses for pass, fail, incomplete, error, zero denominator, every row state, evidence pagination, invalid run ID, and TAP escaping/YAML diagnostics.
- [ ] Confirm focused tests fail, implement stable field ordering and valid TAP plan/count rules, then compare exact fixtures.
- [ ] Commit with `git commit -m "feat: export complete interop results"`.

### Task 4: Accessible HTML report

**Files:** Create `src/http/report_styles.cpp` and `tests/integration/report_accessibility_test.cpp`; modify `src/http/report.cpp`.

**Interfaces:** Produces server-rendered run lists, summaries, filters, requirement tables, and expandable evidence without JavaScript.

- [ ] Test dark-text/light-background contrast, visible status words, keyboard-operable details, table headers, score labels, filter query validation, and malicious publisher/requirement strings.
- [ ] Confirm tests fail, implement semantic HTML and a single escaped rendering path, then validate representative output with an HTML parser.
- [ ] Commit with `git commit -m "feat: render accessible interop reports"`.

### Task 5: Operator and contributor documentation

**Files:** Modify `README.md`; create `docs/building.md`, `docs/operations.md`, `docs/http-api.md`, `docs/scoring.md`, `docs/publisher-driver.md`, `docs/testing.md`, and `docs/troubleshooting.md`.

**Interfaces:** Documents exact local/Docker commands, ports, trusted-network warning, manual observed mode, driven mode, adapter integration, result interpretation, tests, fuzzing, and common failures.

- [ ] Add a documentation smoke test in `tests/e2e/documented-commands.sh` that extracts and runs the quick-start build, container health, run creation, result fetch, and shutdown commands.
- [ ] Run it against the pre-documentation README and confirm the required markers/commands are absent.
- [ ] Write concise runnable documentation using the checked-in drafts for every protocol reference and high-contrast diagrams only where prose is insufficient.
- [ ] Run link checking, the documented-command smoke test, and the supported Compose matrix.
- [ ] Commit with `git commit -m "docs: document interop runner operation"`.

### Task 6: CI and operational shutdown/recovery

**Files:** Create `.github/workflows/ci.yml`, `tests/integration/recovery_test.cpp`, and `tests/e2e/container-shutdown.sh`; modify `src/app/main.cpp` and `src/storage/sqlite_run_store.cpp`.

**Interfaces:** Produces bounded CI jobs for build/test, sanitizers, fuzz smoke, Docker smoke, and explicit recovery of interrupted active runs.

- [ ] Test SIGTERM finalization, forced-kill recovery to `ERROR`, port release, WAL recovery, and repeated container restart.
- [ ] Confirm recovery tests fail, implement signal-safe shutdown notification plus startup reconciliation in normal application context.
- [ ] Run all CI commands locally where available and commit with `git commit -m "ci: verify validator and container workflows"`.
