# Draft Completeness Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish evidence-backed completion gates for draft 18 and 21 and publish honest residual `NOT_TESTABLE`, `NOT_APPLICABLE`, and `NOT_RUN` inventories.

**Architecture:** A release audit joins source-keyword coverage, catalog classification, scenario/evaluator registration, observed execution coverage, and verification artifacts. It never converts absence of evidence into a pass.

**Tech Stack:** C++20 audit CLI, JSON, CTest, libFuzzer, sanitizers, Docker Compose

**Spec:** `docs/moq-contribution-interop-runner-design.md`

## Global Constraints

- A draft is complete only when every applicable externally testable required statement has an evaluator.
- `NOT_TESTABLE`, `NOT_APPLICABLE`, and `NOT_RUN` rows remain visible with reasons.
- No score or release claim combines draft versions or transports.
- Verification output records exact commands, revisions, draft digests, and failures.
- Known publisher defects remain publisher results, not validator exceptions.

## Review Focus

- A catalog row names a deleted test: the release audit must fail before packaging.
- A scenario passes without emitting evidence for its evaluator: the row must remain `NOT_RUN`.
- A test is flaky across repeated runs: the audit must surface both outcomes and fail determinism.
- Fuzz or sanitizer jobs time out: completion must report them as incomplete, not passing.
- One unsupported publisher transport must not erase valid results for another transport.

---

### Task 1: Static completeness auditor

**Files:** Create `include/moq/interop/requirements/completeness.h`, `src/requirements/completeness.cpp`, `src/app/audit_main.cpp`, and `tests/unit/completeness_test.cpp`; modify `CMakeLists.txt`.

**Interfaces:** Produces `moq-interop-audit --draft 18|21 --format json` and `CompletenessReport audit_completeness(RequirementCatalog, ScenarioRegistry, EvaluatorRegistry)`.

```cpp
struct CompletenessReport { DraftVersion draft; std::vector<AuditFinding> findings; bool complete() const; };
CompletenessReport audit_completeness(const RequirementCatalog&, const ScenarioRegistry&, const EvaluatorRegistry&);
```

- [ ] Write tests for missing scenarios/evaluators, orphan registrations, absent evidence schema, duplicate source clauses, unclassified keywords, and fully complete synthetic catalogs.
- [ ] Confirm tests fail, implement deterministic sorted findings with nonzero exit on any required gap, and rerun.
- [ ] Commit with `git commit -m "feat: audit static draft completeness"`.

### Task 2: Dynamic evidence and determinism audit

**Files:** Create `src/requirements/execution_audit.cpp`, `tests/integration/execution_audit_test.cpp`, and `tests/e2e/repeatability.sh`.

**Interfaces:** Produces an execution report joining scenario completion, evaluator evidence, row outcomes, transport, and repeated-run consistency.

- [ ] Test missing evaluator evidence, contradictory repetitions, stable pagination/order, partial transport support, interrupted fuzz runs, and three identical scripted-publisher repetitions.
- [ ] Confirm focused tests fail, implement evidence joins and canonical result hashing that excludes timestamps/run IDs, then rerun.
- [ ] Commit with `git commit -m "feat: audit result evidence and repeatability"`.

### Task 3: Full verification matrix

**Files:** Create `tests/e2e/release-audit.sh` and `.github/workflows/release-audit.yml`; modify `compose.yaml`.

**Interfaces:** Runs unit, golden, protocol, integration, sanitizer, fuzz-smoke, four scripted Docker transports/drafts, and supported `moqxr` black-box combinations; writes a JSON audit artifact.

- [ ] Make the script deliberately fail when any required command is skipped, any test fails, catalog digests drift, a required row lacks an evaluator, or a claimed-supported matrix entry is absent.
- [ ] Run the script before workflow wiring and confirm it reports the exact missing matrix stages.
- [ ] Wire bounded jobs with retained logs/results, run the complete matrix, and inspect every nonpass row rather than weakening expectations.
- [ ] Commit with `git commit -m "test: enforce draft completeness gates"`.

### Task 4: Publish the completeness record

**Files:** Create `docs/completeness.md`; modify `README.md`, `docs/testing.md`, and `src/http/report.cpp`.

**Interfaces:** Exposes per-draft/per-transport catalog counts, evaluator coverage, executed coverage, verification commands, and residual classifications in docs and `/results`.

- [ ] Add tests requiring every reported count to match `moq-interop-audit` JSON and every residual row to include a reason and draft citation.
- [ ] Confirm the test fails before the document/report section exists.
- [ ] Generate counts from audit output, write the human explanation without claiming unexecuted behavior, and link downloadable audit JSON.
- [ ] Run `git diff --check`, the full release audit, and documentation smoke tests.
- [ ] Commit with `git commit -m "docs: publish draft completeness evidence"`.
