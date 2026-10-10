# Scoring and Audit

This document explains how the runner turns evidence into outcomes, weights,
verdicts and scores, how to read the numbers in a result, and how the
`moq-interop-audit` program, the release audit script and the sanitizer and fuzz
scripts check the runner itself. The design rationale is in
[moq-contribution-interop-runner-design.md](moq-contribution-interop-runner-design.md).
The checked-in draft text files in this directory are the only source of
expected behavior; no publisher implementation defines it.

## Outcome states

Every catalog row of the selected draft ends a run in exactly one state per
observation:

| State | Meaning |
|---|---|
| `pass` | The evidence from every required scenario context satisfies the requirement |
| `fail` | Observed evidence contradicts the requirement. One contradicting observation dominates passing ones |
| `not_run` | The row is testable, but its scenario contexts did not run, did not complete, or the publisher never produced the behavior needed to judge it |
| `not_testable` | The behavior cannot be observed or expressed at the protocol boundary; the catalog records a reason and a draft citation |
| `not_applicable` | The statement does not apply to a contribution publisher (or is informative) |

`not_run` is not a failure. It usually means "the runner had no way to see this
behavior from your publisher", for example because the publisher never sends a
message the row is about, or an operator-supplied fixture was absent. Rows that
are `not_testable` or `not_applicable` stay visible in the report and are
excluded from every score.

### Failing on continued service

A close probe is scored from what the publisher did after input that required it
to close the session: a close with the required code passes, a close with another
code fails, and silence is `not_run`. For the scenarios that opt in (see
[scenario-reference.md](scenario-reference.md#close-probes-and-the-liveness-follow-up)),
silence can also fail: the runner sends a valid SUBSCRIBE for the track fixture
afterwards, and a SUBSCRIBE_OK on it with no close of any kind, within the delay
and a grace period after the answer, is wire evidence that the publisher did not
close. The rule only ever turns an unscored row into `fail`. It never produces a
`pass`, never overrides a close, and a refusal or an unanswered follow-up leaves
the row `not_run`. The residual risk is timing: the transport gives no
acknowledgement signal, so "the publisher had the input" is bounded by a 500 ms
delay, not proven.

## Weights and verdicts

| Strength | Weight | Effect |
|---|---:|---|
| MUST, MUST NOT | 10 | A `fail` makes the run verdict `fail` |
| SHOULD, SHOULD NOT | 3 | Counted in the weighted score; a `fail` does not change the verdict |
| MAY | 1 | Counted in the weighted score and coverage |

The run verdict is:

- `fail` when any applicable, testable MUST or MUST NOT row failed;
- otherwise `incomplete` when any applicable, testable row is still `not_run`;
- otherwise `pass`;
- `error` when the runner could not establish or preserve a valid run: a publisher
  that exits before connecting in driven mode, a listener or process failure, a
  stopped raw-probe run, or an inconsistent outcome set.

`incomplete` is the normal result today. Each scenario exercises a slice of the
catalog, and a run that names a few scenarios leaves most rows `not_run`. A
harness failure or `incomplete` run is not a publisher failure.

## Scores

Each run records three ratios, each as `{earned, possible}`:

| Score | `earned` | `possible` |
|---|---|---|
| `required` | Weight of passed MUST and MUST NOT rows | Weight of all applicable, testable MUST and MUST NOT rows |
| `weighted` | Weight of all passed applicable, testable rows | Weight of all applicable, testable rows |
| `coverage` | Weight of rows that were executed (`pass` or `fail`) | Weight of all applicable, testable rows |

With the current catalogs, `required.possible` is 1730 for each draft (173 rows
times 10), and `weighted.possible` and `coverage.possible` were 1910 for
draft 18 and 1917 for draft 21 in a recent run. A fresh run against a publisher
typically shows small earned values and a large gap: for example a single
`subscribe-to-publisher-track` run reported `required` 30 of 1730 with verdict
`incomplete`. Do not read the earned/possible ratio as a percentage of
conformance; read it together with coverage and the list of `not_run` rows.

A row's outcome in the JSON export aggregates its observations: any `fail`
wins, then `pass`; a single `not_run` stays `not_run`. A raw-probe row bound to
several scenario contexts needs every named context to complete before it can
pass; a run that exercises only one of them leaves the row incomplete. Outcomes
from separate runs are never merged.

## Static gate and live evidence

Two different claims must not be confused:

- The static gate (`moq-interop-audit`) proves that the catalog and the code
  agree: every applicable, testable MUST/MUST NOT row names a registered scenario
  and evaluator, and the source-keyword audit has classified every normative
  keyword in the draft text. Its current result is 173 of 173 required rows bound
  for each of draft 18 and draft 21, with optional (SHOULD/MAY) coverage of 1 of
  90 and 1 of 97, and 170 of 170 for draft 22 with optional coverage of 3 of 97.
- Live evidence is what a particular run observed from a particular publisher. A
  binding is not proof that a publisher passed; many scenarios can only pass on
  positive wire evidence and stay `not_run` when the publisher never produces the
  behavior. Some rows score only with operator-supplied fixtures, such as token
  credentials; see [scenario-reference.md](scenario-reference.md).

## moq-interop-audit

```sh
build/moq-interop-audit --draft 18
build/moq-interop-audit --draft 21 --format json
build/moq-interop-audit --draft 18 --database /path/to/runs.sqlite3
```

Options: `--draft 18|21|22` (required; with `--database` only the stored runs of that draft are audited), `--format text|json` (default `text`),
`--docs DIR`, `--requirements DIR` and `--database PATH` (an existing run
database). Without `--docs` and `--requirements` it uses the source tree, or
`/usr/share/moq-interop` when that is absent (as in the Docker image).

Text output:

```text
Draft 18 source 9e6b32cb7797c151e9e127374c1291af3ed546b2d453cd5bbb15946977eeeeb6
Required executable coverage: 173/173
Optional executable coverage: 1/90
Source-keyword audit: complete
Static gate: PASS (89 findings)
```

The findings are non-blocking notices about unbound optional rows. JSON output
adds `source_revision`, `static_complete`, `source_audit`, `executable_coverage`,
sorted per-requirement `findings` (each with `code`, `requirement_id`, `detail`
and `blocking`), and `classified_residuals`: every `not_testable`,
`not_applicable` and informative row with its reason, section and first line in
the draft.

With `--database`, the audit also checks stored execution evidence and adds
`execution_audit` (`consistent`, `run_count`, `scored_rows`, `findings`, and a
canonical SHA-256 per run). Findings include `missing_evaluator_evidence` (a
passed row lacks its declared evidence), `stored_score_mismatch`,
`unfinished_run`, `run_error`, `nondeterministic_result` (repeats of the same
configuration disagree) and `scored_without_binding`. The canonical hash
excludes run ID, timestamps and evidence arrival order; draft, transport,
track, timeout, any compatibility mapping and the validator revision are part of
the comparison group. Publisher identity is not stored, so compare repetitions
only when the publisher binary and fixture are the same. Run the audit after
the service has stopped creating runs. A zero-run audit can be consistent but
proves no behavior.

Exit status: 0 when the static gate passes (and the execution audit, if
requested, is consistent), 1 when the gate or the execution audit has findings,
2 for a usage or load error.

## Release audit script

`tests/e2e/release-audit.sh` produces and validates a release evidence
artifact. It has two modes:

```sh
# Run every stage and write the artifact; the output directory must not exist.
bash tests/e2e/release-audit.sh run /tmp/moq-interop-release-audit \
  "$PWD/build/moq-interop-audit" "moq-interop-runner:$(git rev-parse --short HEAD)" \
  "$PWD/build/moq-interop-picoquic-peer" \
  /path/to/openmoq-publisher /path/to/locmaf-publisher.mp4

# Validate an existing artifact against the checked-in digests and revision.
bash tests/e2e/release-audit.sh check /tmp/moq-interop-release-audit/release-audit.json
```

`run` accepts either only the output directory and audit program, or those two
plus all four optional arguments (Docker image, native test peer, publisher
executable, MP4 fixture). The image must be built from the exact current commit;
a mismatched revision label is rejected. Omitting the optional arguments records
the Docker and publisher stages as `missing`, which fails the gate.

Required stages, all of which must be `pass` for `check` to succeed:
`native_suite` (ctest), `asan_ubsan`, `fuzz_smoke`, `docker_d18_native`,
`docker_d18_webtransport`, `docker_d21_native`, `docker_d21_webtransport`,
`moqxr_d18_webtransport`, `moqxr_d21_webtransport`, `moqxr_d22_native`,
`moqxr_d22_webtransport`, `audit_d18`, `audit_d21` and `audit_d22`. The draft 18 and 21
moqxr stages run `tests/e2e/repeatability.sh`; the draft 22 stages run the
`tests/e2e/moqxr-matrix.sh --pair 22` pair on each transport. There are no Docker stages
for draft 22: the container image carries no draft 22 peer. The moqxr draft 22 stages are smoke tests: each runs one reference scenario and passes when the run finishes with publisher evidence, whatever the verdict; they do not assert that any row passes. The artifact (`release-audit.json`) also stores the source revision,
the draft source digests, the static gate result for each draft, the external
publisher version, executable SHA-256 and fixture SHA-256, plus logs, Docker
run results and the repeat databases. `check` rejects revision drift, digest
drift, missing or duplicate stages, and a missing publisher identity. The
manually dispatched `Draft release audit` GitHub workflow builds a pinned
publisher and a source-matched image and runs the same gate. Local runs require
Clang with libFuzzer, Docker and `timeout`.

## Sanitizer and fuzz scripts

```sh
bash tests/e2e/sanitizer-smoke.sh   # clang build with ASan and UBSan in build-asan/
bash tests/e2e/fuzz-smoke.sh        # libFuzzer targets in build-fuzz/, bounded runs
```

`sanitizer-smoke.sh` builds and runs the focused tests `publisher-driver`,
`run-store`, `execution-audit`, `webtransport-run-api` and `result-export` under
address and undefined-behavior sanitizers. `fuzz-smoke.sh` builds the cursor,
draft-18 message, draft-18 object and WebTransport stream fuzz targets and runs
each for 500 executions with a 30 second limit. Both need Clang.

### moq-lite-06 audit

```sh
build/moq-interop-audit --draft moq-lite-06 [--format text|json] [--database PATH]
```

`--draft moq-lite-06` audits the moq-lite-06 catalog, which is complete since L2c (`complete: true`, all 212 rows
reviewed). Forty evaluators are bound to the 27 scenarios (44 bindings), so the static audit checks the coverage of the
required rows by those bindings (36 of 36 required and 4 of 4 optional Applicable and Testable rows) as it does for the
other drafts, and its verdict line reads `COMPLETE: catalog complete`. The planned scenario and evaluator ids follow
`l06-<area>-<name>`, with the areas `session`, `setup`, `announce`, `subscribe`, `group`, `frame`, `errors`, `track`,
`fetch`, `probe`, `goaway` and `datagram`. Other spellings (`106`, `moq-lite-05`) are refused. With `--database PATH` the
stored moq-lite-06 runs of that database are also audited (the execution audit of the MoQ Transport drafts with the lite
bindings: each stored score is recomputed with `score()`, and every scored row must be bound to a selected scenario and,
when it passed, carry that binding's declared evidence); the text output adds an
`Execution audit: consistent|findings (N runs, M scored rows, K findings)` line before the verdict and JSON an
`execution_audit` object (`null` without `--database`).

```text
Draft moq-lite-06 source <sha256>
Rows: <n>
Reviewed: <n>
Unreviewed: <n>
Unreviewed required (MUST/MUST NOT): <n>
Required applicable testable (reviewed rows): <n>
Planned scenarios: <n>
Source-keyword audit: complete
Findings: <n>
  [blocking] <code> <requirement_id>: <detail>
COMPLETE: catalog complete
```

The planned scenarios are the distinct scenario ids named by any row. JSON output carries the fields `schema_version`,
`draft`, `source_sha256`, `rows`, `reviewed`, `unreviewed`, `unreviewed_required`, `required_applicable_testable`,
`planned_scenarios`, `staged` (false), `complete` (true), `verdict`, `source_audit`, `findings`, `execution_audit`. It is
a separate shape from the other drafts: there is no `source_revision`, `static_complete` or `executable_coverage`. Exit
status is 0 unless the source-keyword audit fails or a finding is blocking or the execution audit has findings, 1 in
that case, and 2 for an argument or loader error (including a `--database` path that does not exist). A copy of the
catalog with `complete: false` is audited with the staged rules as before (`STAGED: incomplete catalog (not a pass)`).

Runs made through the HTTP API are scored as the drafts' are: `score()` gives `fail` when a required row failed,
`incomplete` while a scored row stays `not_run`, `error` for a harness error, and `pass` when every scored row is
judged or not applicable and none failed. The run record, the exports and the completeness entry carry no `staged` field.
See [http-api.md](http-api.md#moq-lite-06-runs).

**Not applicable for this peer (L2c).** A scored row whose rule is out of the publisher's reach is `not_applicable` and
leaves the required, weighted and coverage denominators, so it does not keep a run from passing. Eight evaluators carry a
predicate (`lite_applicability_registry`): the evaluator found nothing to judge and the transcript shows why it is
not the publisher's defect. Row 075 (a publisher that advertised no Probe capability resets the Probe Stream) is not
applicable to a publisher that advertised Report or Increase (row 072 judges that one); row 105 (a datagram body of at most
1200 bytes) to a publisher that sent no datagram (datagrams are a permission); rows 077 (no new streams after a GOAWAY)
and 186 (a second GOAWAY closes the session) to a publisher that ends the session on the first GOAWAY, which the draft
allows; rows 120 and 124 (the Path equals `/moq?token=l1d`, a Path is sent) and 126 (a server's SETUP Path closes the
session) to a WebTransport run, and row 125 (no Path on a WebTransport session) to a native QUIC run. A verdict of true
or false is never replaced, a run that was cut short (harness failure, event limit, timeout) is never marked, and a row
with any failing context is `fail` whatever its other contexts were.

What `not_run` still means for a bound row (the run is then `incomplete` unless a required row failed): row 152 (a retired
announce id is not reused) needs a publisher that retracts the broadcast inside the observation window, which the moq CLI
does not (the reference publisher does with `--retract-after-polls`); row `L06-4-4-MUST-027` takes its session half from
any of its five scenarios and its stream half from `l06-errors-code-space`, so it settles only in a run that holds all five
and is `not_run` in a single-scenario run; when a WebTransport client's SETUP carries Path the runner closes the session
for it (draft 7.3.2, a receiver MUST close) and every other row of that session is `not_run` except 111 and 125. A Fail by
the absence of a close (rows 107, 126, 131, 179 and 186, whose bindings declare `peer_close`) is counted as observed on the
completeness page for the moq-lite-06 catalog; a Pass still needs the evidence its binding declares.
