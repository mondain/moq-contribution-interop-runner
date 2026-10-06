# HTTP API

This document describes every route served by `moq-interop-runner`: the request
that creates a run, the run lifecycle, observed and driven mode, the events
that record what the runner saw, the JSON, TAP and HTML exports, the
completeness endpoint, and the error codes. All JSON bodies carry
`"schema_version": 1`. The API has no authentication; keep it on a trusted
network. Start the runner as described in
[building-and-running.md](building-and-running.md).

The examples below assume the runner listens on `127.0.0.1:8080`.

## Routes

| Method and path | Purpose |
|---|---|
| `GET /healthz` | Database readiness and the list of executable profiles |
| `GET /api/v1/drafts` | Per-draft catalog summary: source digest, row counts, applicability and testability counts, and `runnable`. Draft 22 appears as an optional third entry (`complete: true`, `runnable: false`) when the service loaded its catalog |
| `GET /api/v1/requirements?draft=18\|21\|22` | Requirement catalog rows with draft line citations (paginated) |
| `POST /api/v1/runs` | Create a run |
| `GET /api/v1/runs` | List runs, newest first (paginated) |
| `GET /api/v1/runs/{id}` | One run with its score and per-requirement outcomes |
| `GET /api/v1/runs/{id}/events` | Evidence events of a run (paginated) |
| `POST /api/v1/runs/{id}/stop` | Stop an active run |
| `GET /results` | HTML run list plus a completeness summary |
| `GET /results/{id}` | HTML requirement-by-requirement report |
| `GET /results/{id}.json` | Full JSON export: every catalog row, outcome and evidence |
| `GET /results/{id}.tap` | Scenario-level TAP 14 export |
| `GET /results/completeness.json` | Live completeness inventory across stored runs |

Collection endpoints accept `limit` (1 to 100, default 50) and `offset` and
return `pagination` with `limit`, `offset`, `total` and `next_offset` (null on
the last page). `POST /stop` must carry a body header; with `curl` use
`-X POST -d ''`, because an empty POST without `Content-Length` is rejected with
HTTP 400.

## Creating a run

```sh
curl -sS -X POST http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' -d @run.json
```

```json
{
  "draft": 18,
  "transport": "native-quic",
  "mode": "observed",
  "scenarios": ["subscribe-to-publisher-track"],
  "timeout_ms": 8000,
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}
}
```

| Field | Rules |
|---|---|
| `draft` | `18` or `21`. Drafts are scored independently. `22` is accepted as a known draft but is refused with `422 draft_not_runnable` before any scenario is validated, and no run is created (draft 22 is not runnable through the API yet). Any other number is `400 invalid_run_config`. |
| `transport` | `native-quic` or `webtransport` (hyphen here; the driver contract uses `native_quic`). |
| `mode` | `observed` (you start the publisher) or `driven` (the runner starts it through the adapter). |
| `scenarios` | 1 to 100 distinct nonempty scenario IDs. Several IDs are allowed only for raw-probe scenarios; the original typed scenarios take exactly one per run, and mixing the two returns 422. |
| `timeout_ms` | 2 to 3600000. For multi-scenario runs it applies to each context. |
| `track` | Optional for receiver-error probes in observed mode, required for most scenarios and always required in driven mode. `namespace_hex` is an array of 0 to 32 nonempty hex strings; `name_hex` is the possibly empty hex Track Name. The decoded namespace plus name is limited to 4096 bytes. Hex preserves arbitrary bytes. |
| `publisher_capabilities` | Optional object declaring what the publisher does not implement; see below. Absent means the publisher is fully capable. |

### Declaring publisher capabilities

The drafts let an endpoint that is not a relay implement only the subset of MOQT
it needs (draft 18 Section 4, draft 21 Section 1.5), so a live publisher with no
cache may legitimately have no FETCH. A run can say so:

```json
{"publisher_capabilities": {"fetch": false}}
```

`fetch` is the only capability so far and must be a boolean. Unknown names, other
value types and a non-object are rejected with 400 `invalid_publisher_capabilities`.
`{}` declares nothing.

The runner can also carry a startup default, `--publisher-no-fetch`, so a driven
setup declares it once. The effective declaration of a run is the value in its
request when it gives one, otherwise the startup default, otherwise capable. An
explicit `{"fetch": true}` therefore overrides `--publisher-no-fetch` for that
run, and `{"fetch": false}` works without the flag. `GET /healthz` reports the
startup default as `publisher_capability_defaults`.

When `fetch` is `false`:

- A run whose selected scenarios all need FETCH is refused with 422
  `scenario_requires_publisher_capability`; the message names the first scenario
  and the capability. No run, listener or publisher process is created.
- In a run that mixes both kinds, the scenarios that need FETCH are skipped: no
  listener context and no publisher process is started for them, and each gets a
  `context_skipped` event with the detail `publisher declared no FETCH support`.
  The selection is still recorded as requested.
- A catalog row whose named scenarios all need FETCH is reported `not_applicable`
  for the run, with the reason, and leaves the required, weighted and coverage
  denominators. A row that also names a scenario that does not need FETCH is not
  touched and keeps the rule that every named scenario must run. Skipped
  scenarios therefore never turn a run into `error` or `fail`.

The effective declaration is stored with the run: `config.publisher_capabilities`
in the run record, a `publisher_capabilities` event (`detail` `fetch=false` or
`fetch=true`) as the first event of every run, and the exports below. The
`executable_profiles` entries in `/healthz` carry `requires_fetch` for each
scenario; [scenario-reference.md](scenario-reference.md) lists them.

List the executable scenario IDs for a profile from `/healthz`:

```sh
curl -s http://127.0.0.1:8080/healthz | jq -r \
  '.executable_profiles[] | select(.draft==18 and .transport=="native-quic" and .mode=="observed") | .scenario' | sort -u
```

A successful create returns HTTP 201:

```json
{
  "schema_version": 1,
  "run": {"id": "run-18dabdcec655134a", "state": "active", "verdict": null, "...": "..."},
  "publisher_endpoint": {"address": "127.0.0.1", "port": 19901, "alpn": "moqt-18"}
}
```

For `webtransport` the endpoint also carries `url` (for example
`https://127.0.0.1:19901/moq`), `path` (`/moq`) and `protocol` (`moqt-18` or
`moqt-21`; `moqt-22` is reserved for draft 22 and is never offered while draft 22 is
not runnable), and `alpn` is `h3`. For native QUIC the publisher connects to
`address:port` with ALPN `moqt-18` or `moqt-21` (`moqt-22` once draft 22 is runnable),
using the URI `moqt://address:port/moq`.

## Run lifecycle

1. `POST /api/v1/runs` reserves one UDP port from the configured range and starts
   a listener. `state` is `active` and `verdict` is `null`.
2. In observed mode the runner waits for a publisher to connect; in driven mode
   it starts the adapter immediately. Each scenario context uses a fresh session.
3. Evidence is appended to the event log while the run is active.
4. When every context finishes, the timeout expires or `POST .../stop` is called,
   the run is evaluated once against the full catalog and becomes `finalized`
   with a `verdict` (`pass`, `fail`, `incomplete` or `error`), a `score`, and one
   `outcomes` entry per requirement observation. A finalized run never changes.

A run that has not finalized can be ended with `POST /api/v1/runs/{id}/stop`. The
response is the run record. A stopped typed run (one of the original
single-scenario controllers) is finalized `incomplete`; a stopped raw-probe run
is finalized `error`. Either way the collected evidence is kept, and the
interrupted interaction is not counted as a publisher failure. Stopping a
finalized run returns 409 `run_finalized`.

### Observed and driven mode

In observed mode you start the publisher yourself and point it at
`publisher_endpoint`. For a multi-scenario run, read the events and reconnect with
a fresh session each time a `context_ready` event names the next scenario, using the
endpoint URI in that event's `detail`: the address and port are constant, but over
native QUIC a few contexts change the path, add a query, or (draft 18
`connect-with-empty-host-moqt-uri`) leave the host empty, because the URI itself is
under test. See [publisher-harness-guide.md](publisher-harness-guide.md#22-create-a-run-and-connect-observed-mode).
The runner does not authenticate which process connects.

In driven mode the runner launches the executable configured with
`--driver-executable` once per context, writes a JSON request file and passes it
through the environment; see
[publisher-harness-guide.md](publisher-harness-guide.md) for the contract. The
request names the run's draft and the scenario id as it was selected for the
run, the same id the run's events carry (for a draft 22 run a `d22-` id, also
for a scenario the runner executes with its draft 21 implementation). If
the adapter exits before the publisher connects the run ends `error` with the
logs retained. Once the publisher is connected, scores come from MoQT
observations, not from the process exit status. `driven` requires `track` and a
runner started with a driver executable; otherwise the request is rejected.

## Run records

`GET /api/v1/runs/{id}` returns:

| Field | Meaning |
|---|---|
| `id`, `state`, `created_at_unix_ns`, `finalized_at_unix_ns` | Identity and timing |
| `config` | The accepted run configuration (hex track fixture included) |
| `build` | Runner version, source revision and dependency revisions |
| `verdict` | `pass`, `fail`, `incomplete`, `error` or `null` while active |
| `score` | `required`, `weighted` and `coverage`, each `{earned, possible}`; see [scoring-and-audit.md](scoring-and-audit.md) |
| `scoring_profile` | `standards`, or `compatibility` when an unknown-alias compatibility code is configured |
| `run_error_reason` | When `verdict` is `error`, a sentence saying why (the first `error_reasons` entry); `null` otherwise |
| `error_reasons` | `{scenario_id, kind, detail}` for every `harness_error`, `run_aborted` and `run_stopped` event of the run |
| `truncated_contexts` | `{scenario_id, detail}` for every context whose evidence hit a recording limit (`context_event_limit`). Such a context is not scored and does not make the verdict `error` |
| `outcomes` | `{requirement_id, state}` per observation; states are `pass`, `fail`, `not_run`, `not_testable`, `not_applicable` |
| `events` | `{total, href}` for the event log |

`GET /api/v1/runs` returns summaries (`id`, `config`, `state`, timestamps,
`verdict`, `score`) without outcomes.

## Events

`GET /api/v1/runs/{id}/events?limit=100&offset=0` returns items with `sequence`,
`monotonic_time_ns`, `wall_time_unix_ns`, `kind`, `detail`, `connection_id`,
`stream_id`, `request_id`, `scenario_id` and `requirement_id`. Kinds seen in
normal runs include:

| Kind | Meaning |
|---|---|
| `context_ready` | A scenario context is ready; `detail` gives the endpoint URI to connect to |
| `transport_established`, `session_evidence`, `peer_stream_classified` | Transport and stream observations |
| `local_setup_observed`, `peer_setup_received` | SETUP exchange, with option types |
| `request_observed`, `initial_response_observed`, `object_observed` | Decoded protocol messages and objects |
| `publisher_process` | Driven mode: the adapter process result (`status`, `exit_code`, `term_signal`, and `stdout_log` and `stderr_log` with `path`, `bytes`, `sha256`) |
| `publisher_exit_after_refusal` | The publisher exited after the runner refused or rejected it; not a harness fault |
| `harness_error` | The context became unusable (publisher process failed, transport rejected a write, and so on); `detail` is the specific reason. It ends the run as `error` |
| `run_aborted` | Added after a `harness_error` when later contexts of the run were not run; `detail` names the reason and the contexts not run |
| `run_stopped` | The run was stopped on request before every selected context finished |
| `context_event_limit` | The publisher sent more than one context records (4096 recorded transport events or 4 MiB of stream data; adjacent chunks of one publisher data stream count as one event). Later events were not recorded, this context is not scored (its rows stay `not_run`), and the run goes on with the next context, so it is `incomplete`, not `error` |
| `publisher_capabilities` | First event of every run: the effective declaration, `fetch=true` or `fetch=false`. It belongs to the run, so it has no `scenario_id` |
| `context_skipped` | The scenario was not started because the publisher declared it does not implement a capability it needs; `detail` is `publisher declared no FETCH support` |
| `runner_recovery` | Added at restart to a run that was interrupted |

Events tied to a scored requirement carry its `requirement_id`; the JSON export
lists them per requirement in `evidence_sequences`.

## Exports

- `GET /results/{id}.json` returns `run`, `draft_source_sha256`, `requirements`
  (every catalog row with `outcome`, `observations`, `weight`, `required`,
  `score_eligible`, `scenarios`, `evaluators`, `source` citation and
  `evidence_sequences`) and `evidence` (the full event list). A row's `outcome`
  aggregates its observations: `pass`, `fail`, `not_run`, `not_testable`,
  `not_applicable`, `error` for an inconsistent set, or `null` when the run
  recorded nothing for it. A scored row declared not applicable for the run
  (see the publisher capabilities above) is `not_applicable` and carries a
  `not_applicable_reason`, `null` for every other row. The document also has
  `publisher_capabilities` and `skipped_scenarios` (`scenario_id`, `reason`).
- `GET /results/{id}.tap` is TAP 14, one test point per selected scenario. A
  point is `ok` only when every applicable row bound to that scenario passed
  (or the scenario has none and is marked `# SKIP`). Failed, incomplete and
  errored scenarios are `not ok`; the YAML block gives `result` (`pass`,
  `fail`, `incomplete`, `skip`, `error`), counts and `scoring_profile`. A
  scenario skipped by a capability declaration is `ok ... # SKIP publisher
  declared no FETCH support` (its YAML block adds `skip_reason`), and a comment
  line after the plan records `publisher_capabilities fetch=false`.
  Because most runs leave rows `not_run`, TAP points are often `not ok` for
  scenarios that are merely incomplete.
- `GET /results/{id}` is the HTML report. It accepts `strength` (`MUST`,
  `MUST NOT`, `SHOULD`, `SHOULD NOT`, `MAY`), `outcome` (`pass`, `fail`,
  `not_run`, `not_testable`, `not_applicable`, `unobserved`, `error`), `section`
  and `scenario` query filters. Invalid filters return 400
  `invalid_report_filter`. A run that declared no FETCH shows a "Publisher
  capabilities" section naming the skipped scenarios, and each not applicable
  row states its reason in text.
- `GET /results` lists up to 100 runs and summarizes completeness by draft and
  transport.

## Requirements and the completeness endpoint

`GET /api/v1/requirements?draft=18` (or `21`, or `22` when the service loaded the draft 22 catalog) lists catalog rows with `id`, `strength`,
`source` (`section`, `first_line`, `last_line` in the checked-in draft text),
`actor`, `summary`, `applicability`, `testability`, `scenarios`, `evaluators`
and `rationale`. The line numbers refer to `docs/draft-ietf-moq-transport-18.txt`
`-21.txt` or `-22.txt`.

`GET /results/completeness.json` returns `schema_version`, `source_revision` and
`drafts`. Each draft entry has `source_sha256`, `catalog_rows`,
`required_covered`/`required_total`, `optional_covered`/`optional_total`,
`evaluator_complete`, static `findings`, `classified_residuals` (rows that are
`not_testable`, `not_applicable` or informative, with reasons and citations), and
`transports`. Each transport entry (`native-quic`, `webtransport`) reports
`run_count`, `scored_rows`, `observed_requirement_count` (requirements with an
evidence-backed pass or fail), `not_run_count` with the `not_run` rows,
`execution_consistent` and `execution_findings`. It reflects only the runs
stored in the current database. A pass or fail observation is not proof of
conformance; inspect the run's evidence.

`GET /healthz` returns `status`, `database.ready`, `supported_drafts` (only runnable drafts, so `[18, 21]`; draft 22 is never listed there or in
`executable_profiles`), the
`validator` build identity, `publisher_capability_defaults` and
`executable_profiles`: one entry per `draft`, `transport`, `mode` and `scenario`
with a `configured` flag that is true only when the TLS material (and, for
`driven`, an adapter) is set, and a `requires_fetch` flag.

## Error codes

Errors have the form `{"error": {"status", "code", "message"}, "schema_version": 1}`.

| HTTP | `code` | Cause |
|---|---|---|
| 400 | `invalid_json` | Body is not valid JSON |
| 400 | `invalid_run_config` | Missing or invalid field, bad hex, out-of-range `timeout_ms`, scenario needs `track`, driven without `track`, or a track the scenario cannot use |
| 400 | `invalid_publisher_capabilities` | `publisher_capabilities` is not an object, names an unknown capability, or gives a non-boolean value |
| 400 | `missing_draft`, `unsupported_draft` | `draft` query parameter absent, or not 18 or 21 (or 22 when the draft 22 catalog is loaded; the message says which) |
| 400 | `invalid_pagination` | `limit` or `offset` invalid |
| 400 | `invalid_report_filter` | HTML report filter invalid |
| 404 | `run_not_found`, `not_found` | Unknown run or path |
| 409 | `run_finalized` | Stop requested for a finalized run |
| 409 | `run_not_active` | The run is not active in this process |
| 422 | `scenario_requires_publisher_capability` | Every selected scenario needs a capability the run declares the publisher does not implement (today only FETCH); the message names the first scenario and the capability |
| 422 | `draft_not_runnable` | The run request names draft 22. The draft is known and has a requirement catalog, but is not runnable through the API yet. Returned before scenario validation; no run is created |
| 422 | `unsupported_run_config` | Unknown scenario, mixed typed and raw scenarios, driven mode without an adapter, or a draft the listener does not support. The message names the offending scenario and the reason. Unsupported scenarios are never silently scored |
| 500 | `internal_error` | Unexpected failure |
| 503 | `publisher_listener_unavailable` | No TLS material configured, or the listener could not start |
| 503 | `publisher_ports_exhausted` | Every port in the range is in use (replacement-session scenarios need two free ports) |
| 503 | `database_not_ready` | Health check could not read the database |
