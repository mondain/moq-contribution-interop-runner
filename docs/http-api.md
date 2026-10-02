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
| `GET /api/v1/drafts` | Per-draft catalog summary: source digest, row counts, applicability and testability counts |
| `GET /api/v1/requirements?draft=18\|21` | Requirement catalog rows with draft line citations (paginated) |
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
| `draft` | `18` or `21`. Drafts are scored independently. |
| `transport` | `native-quic` or `webtransport` (hyphen here; the driver contract uses `native_quic`). |
| `mode` | `observed` (you start the publisher) or `driven` (the runner starts it through the adapter). |
| `scenarios` | 1 to 100 distinct nonempty scenario IDs. Several IDs are allowed only for raw-probe scenarios; the original typed scenarios take exactly one per run, and mixing the two returns 422. |
| `timeout_ms` | 2 to 3600000. For multi-scenario runs it applies to each context. |
| `track` | Optional for receiver-error probes in observed mode, required for most scenarios and always required in driven mode. `namespace_hex` is an array of 0 to 32 nonempty hex strings; `name_hex` is the possibly empty hex Track Name. The decoded namespace plus name is limited to 4096 bytes. Hex preserves arbitrary bytes. |

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
`moqt-21`), and `alpn` is `h3`. For native QUIC the publisher connects to
`address:port` with ALPN `moqt-18` or `moqt-21`, using the URI
`moqt://address:port/moq`.

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
`publisher_endpoint`. For a multi-scenario run, read the events and reconnect to
the same endpoint with a fresh session each time a `context_ready` event names
the next scenario. The runner does not authenticate which process connects.

In driven mode the runner launches the executable configured with
`--driver-executable` once per context, writes a JSON request file and passes it
through the environment; see
[publisher-harness-guide.md](publisher-harness-guide.md) for the contract. If
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
| `harness_error` | The publisher process failed in a way that makes the context unusable |
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
  recorded nothing for it.
- `GET /results/{id}.tap` is TAP 14, one test point per selected scenario. A
  point is `ok` only when every applicable row bound to that scenario passed
  (or the scenario has none and is marked `# SKIP`). Failed, incomplete and
  errored scenarios are `not ok`; the YAML block gives `result` (`pass`,
  `fail`, `incomplete`, `skip`, `error`), counts and `scoring_profile`.
  Because most runs leave rows `not_run`, TAP points are often `not ok` for
  scenarios that are merely incomplete.
- `GET /results/{id}` is the HTML report. It accepts `strength` (`MUST`,
  `MUST NOT`, `SHOULD`, `SHOULD NOT`, `MAY`), `outcome` (`pass`, `fail`,
  `not_run`, `not_testable`, `not_applicable`, `unobserved`, `error`), `section`
  and `scenario` query filters. Invalid filters return 400
  `invalid_report_filter`.
- `GET /results` lists up to 100 runs and summarizes completeness by draft and
  transport.

## Requirements and the completeness endpoint

`GET /api/v1/requirements?draft=18` lists catalog rows with `id`, `strength`,
`source` (`section`, `first_line`, `last_line` in the checked-in draft text),
`actor`, `summary`, `applicability`, `testability`, `scenarios`, `evaluators`
and `rationale`. The line numbers refer to `docs/draft-ietf-moq-transport-18.txt`
or `-21.txt`.

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

`GET /healthz` returns `status`, `database.ready`, `supported_drafts`, the
`validator` build identity and `executable_profiles`: one entry per
`draft`, `transport`, `mode` and `scenario` with a `configured` flag that is true
only when the TLS material (and, for `driven`, an adapter) is set.

## Error codes

Errors have the form `{"error": {"status", "code", "message"}, "schema_version": 1}`.

| HTTP | `code` | Cause |
|---|---|---|
| 400 | `invalid_json` | Body is not valid JSON |
| 400 | `invalid_run_config` | Missing or invalid field, bad hex, out-of-range `timeout_ms`, scenario needs `track`, driven without `track`, or a track the scenario cannot use |
| 400 | `missing_draft`, `unsupported_draft` | `draft` query parameter absent or not 18 or 21 |
| 400 | `invalid_pagination` | `limit` or `offset` invalid |
| 400 | `invalid_report_filter` | HTML report filter invalid |
| 404 | `run_not_found`, `not_found` | Unknown run or path |
| 409 | `run_finalized` | Stop requested for a finalized run |
| 409 | `run_not_active` | The run is not active in this process |
| 422 | `unsupported_run_config` | Unknown scenario, mixed typed and raw scenarios, driven mode without an adapter, or a draft the listener does not support. Unsupported scenarios are never silently scored |
| 500 | `internal_error` | Unexpected failure |
| 503 | `publisher_listener_unavailable` | No TLS material configured, or the listener could not start |
| 503 | `publisher_ports_exhausted` | Every port in the range is in use (replacement-session scenarios need two free ports) |
| 503 | `database_not_ready` | Health check could not read the database |
