# Publisher Harness Guide

This guide shows an engineer who has a MoQT publisher application how to build a
small harness (an adapter) for it and run it against the interop runner, so the
runner can start the publisher, point it at a test endpoint, and score what it
observes. It covers the two ways to connect a publisher, the end-to-end flow, the
driver contract, a complete worked adapter, how to unit-test the adapter without
the runner, running locally and in Docker Compose, what your publisher must
satisfy, how to read results, troubleshooting, and CI use. Working files live in
[`examples/harness/`](../examples/harness/). The runner itself is described in
[building-and-running.md](building-and-running.md), its API in
[http-api.md](http-api.md), and scoring in [scoring-and-audit.md](scoring-and-audit.md).

The runner plays the part of a relay-side peer: it accepts your publisher's
connection, subscribes and requests, and checks what comes back against drafts 18
and 21 (the text files in this directory). It does not forward objects to real
subscribers and it does not use any publisher implementation as a reference.

## 1. Choosing a mode: observed or driven

| | Observed mode | Driven mode |
|---|---|---|
| Who starts the publisher | You, by hand or from your own script | The runner, through an adapter executable you provide |
| Run request `mode` | `"observed"` | `"driven"` |
| Runner needs a driver flag | No | Yes: `--driver-executable` and `--driver-log-root` |
| Endpoint | Returned by `POST /api/v1/runs`; you connect to it | Computed by the runner and handed to the adapter |
| Multi-scenario runs | You reconnect each time a `context_ready` event names the next scenario | The runner restarts the adapter for each scenario context |
| Good for | First contact, debugging a single scenario, publishers that cannot be scripted | Repeatable runs, many scenarios, CI |
| Logs | Yours | Request file and stdout/stderr kept by the runner, with SHA-256 hashes in the events |

Start with observed mode to confirm that your publisher can connect at all, then
write an adapter and use driven mode for anything repeatable. Observed mode does
not authenticate which process connects.

## 2. End-to-end flow

### 2.1 Build and start the runner

Build as described in [building-and-running.md](building-and-running.md). Create a
certificate with a subject alternative name for the address your publisher will
use (the example uses loopback), and make sure the publisher trusts it:

```sh
mkdir -p work && cd work
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
  -subj /CN=localhost -days 7 -addext subjectAltName=DNS:localhost,IP:127.0.0.1
```

Start the runner with a port range for publishers:

```sh
../build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --database "$PWD/runs.sqlite3" \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 4443 --publisher-port-end 4452 \
  --tls-cert "$PWD/cert.pem" --tls-key "$PWD/key.pem"
```

- Each run holds one UDP port from the range, so the range size is the maximum
  number of simultaneous runs. A scenario that offers a replacement session
  (`receive-control-goaway-with-new-session-uri`,
  `d21-publisher-goaway-alternate-uri`) holds two, so the range must contain at
  least two ports. When no port is free the API returns 503
  `publisher_ports_exhausted`.
- `--publisher-advertise` is the address the runner returns to publishers; it must be
  reachable from the publisher process.
- Use absolute paths for `--tls-cert`, `--tls-key` and fixture paths. The runner
  passes the certificate path to the adapter unchanged, and a relative path only
  works if the adapter runs from the same directory.

### 2.2 Create a run and connect (observed mode)

The track fixture is the track the runner will request. `namespace_hex` is an
array of hex-encoded namespace fields and `name_hex` is the hex-encoded track
name; the reference setup uses namespace `media` (`6d65646961`) and track `vide_1`
(`766964655f31`). To use other names, hex-encode your own, for example
`printf '%s' mytrack | od -An -tx1 | tr -d ' \n'`.

```sh
cat > run.json <<'EOF'
{
  "draft": 18,
  "transport": "native-quic",
  "mode": "observed",
  "scenarios": ["subscribe-to-publisher-track"],
  "timeout_ms": 8000,
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}
}
EOF
curl -sS -X POST http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' -d @run.json | tee created.json
```

The response contains `publisher_endpoint`. Native QUIC returns `address`, `port`
and `alpn` (`moqt-18`, `moqt-21` or `moqt-22`); connect to `moqt://ADDRESS:PORT/moq`.
WebTransport (`"transport": "webtransport"`) additionally returns `url`
(`https://ADDRESS:PORT/moq`), `path` and `protocol`; give the URL to your publisher.
Start your publisher now. The run ends when its scenario completes, the timeout
expires or you stop it.

For a run with several scenarios, the endpoint in `publisher_endpoint` is only the
first context's. Each `context_ready` event carries the endpoint URI to use for that
context in its `detail` (`endpoint=...`); connect to that, not to a URI you saved
earlier. The address and port stay the same, but over native QUIC a few contexts
change the rest of the URI on purpose, because the URI is the thing under test:

| Context | Endpoint in `context_ready` | What to do |
|---|---|---|
| Draft 18 `connect-with-empty-host-moqt-uri` | `moqt://:PORT/moq`, a URI with no host | A conforming publisher does not use it; the row can only fail if a connection arrives. Not connecting is expected, and it does not mean your publisher is broken |
| Draft 18 `connect-publisher-to-native-uri-with-query` | `moqt://HOST:PORT/moq?interop=1` | Connect with the query kept in the URI |
| Draft 21 native URI scenarios | `/moq`, `/moq?run=1` or `/moq?` as the scenario names | Connect with the exact path and query |

Over WebTransport the URL is always `https://ADDRESS:PORT/moq`.

### 2.3 Poll, then read the results

```sh
id=$(jq -r .run.id created.json)
curl -s http://127.0.0.1:8080/api/v1/runs/$id | jq '.run | {state, verdict, score}'
curl -s "http://127.0.0.1:8080/api/v1/runs/$id/events?limit=100" | jq -c '.items[] | [.sequence, .kind, .scenario_id]'
curl -s http://127.0.0.1:8080/results/$id.json -o result.json    # every catalog row and its evidence
curl -s http://127.0.0.1:8080/results/$id.tap                    # scenario-level TAP 14
# HTML report: open http://127.0.0.1:8080/results/$id in a browser
```

`state` is `active` until the run finalizes; then `verdict` is `pass`, `fail`,
`incomplete` or `error`. To select several scenarios in one run, list up to 100
raw-probe scenario IDs; the runner opens a fresh session for each. Scenario IDs
are listed in `/healthz` (see [http-api.md](http-api.md)) and explained in
[scenario-reference.md](scenario-reference.md).

`examples/harness/run-scenarios.sh` wraps the create, wait and export steps for either
mode:

```sh
examples/harness/run-scenarios.sh 18 native-quic observed subscribe-to-publisher-track
```

## 3. The driver contract

In driven mode the runner executes the adapter once per scenario context. The
adapter's job is to translate a neutral request into your publisher's command line
and start the publisher. The contract is versioned (`schema_version` 1) and defined
by [`adapters/contract.schema.json`](../adapters/contract.schema.json).

### 3.1 How the adapter is started

- The adapter is the file given to `--driver-executable` (an absolute path),
  followed by any `--driver-arg VALUE` arguments (repeatable). It is executed
  directly with an argument array; no shell is involved and no argument is
  interpreted. Make the file executable and give it a shebang.
- Two environment variables are set, overriding any inherited values with the
  same names. All other variables from the runner's environment are inherited.

  | Variable | Value |
  |---|---|
  | `MOQ_INTEROP_DRIVER_CONTRACT_VERSION` | `1` |
  | `MOQ_INTEROP_DRIVER_REQUEST_FILE` | Absolute path of the request JSON, `<log_dir>/request.json` |

- Standard output and standard error are redirected to files in `log_dir`
  (`stdout.bin`, `stderr.bin`); standard input is not redirected and must not be
  read. The adapter runs from the runner's working directory in its own process
  group. It starts with no open descriptors above standard error: the runner closes
  its own sockets and files in the child, so a long-lived publisher cannot keep a
  run's UDP port bound after the runner releases it.
- Check the contract version first and refuse any other value.

### 3.2 The request file

| Field | Type | Meaning |
|---|---|---|
| `schema_version` | `1` | Contract version |
| `run_id` | string | Run identifier, for example `run-18dabdcec655134a` |
| `scenario_id` | string | The scenario this context runs, as it was selected for the run: the same id as in the run's `config.scenarios` and on its events (a draft 22 run's ids start with `d22-`, also for scenarios the runner shares with draft 21). Adapters may use it to select options that make the publisher emit messages the scenario observes. They must not use it to change what is expected |
| `endpoint` | string | URI to connect to: `moqt://HOST:PORT/moq` for native QUIC, `https://HOST:PORT/moq` for WebTransport. Some scenarios use a different path or query (`/moq?run=1`, `/moq?`, `?interop=1`) or an empty host; pass the URI through unchanged |
| `draft` | `18`, `21`, `22` or `"moq-lite-06"` | Draft under test: the run's draft, whose ALPN the runner accepts. The bundled `adapters/moqxr` supports drafts 18, 21 and 22 (moqxr's `--draft 22`, native backend); `adapters/moq5` supports drafts 18 and 21 only and refuses a draft 22 request (exit 64, "draft 22 is not supported by this adapter"); `adapters/imquic` supports draft 22 only (exit 64 for 18 and 21). The string `"moq-lite-06"` (a moq-lite draft, never the number 106) is carried by the contract and `adapters/contract.schema.json` but no bundled adapter runs it: moqxr, moq5 and imquic refuse it (exit 64, naming "moq-lite-06") |
| `transport` | `"native_quic"` or `"webtransport"` | Note the underscore here; the HTTP API uses `native-quic` |
| `namespace_hex` | array of hex strings | Namespace fields as lowercase hex of opaque bytes (0 to 32 fields) |
| `track_name_hex` | hex string | Track name as lowercase hex, possibly empty |
| `fixture` | string | Path from `--driver-fixture`; empty if not configured |
| `tls_ca` | string | Trust file for the runner's certificate: `--driver-ca`, else the `--tls-cert` path |
| `log_dir` | string | Absolute directory for this context's request file and logs |
| `scenario_timeout_ms` | integer | The scenario's deadline (the run's `timeout_ms`) |
| `process_timeout_ms` | integer | Hard limit for the process: `scenario_timeout_ms` plus 1000 |

For a draft 22 request, the bundled moqxr adapter gives a scenario shared with draft 21
(`d22-X` paired with `d21-X`) exactly the moqxr options of `d21-X`, with `--draft 22`,
except for the draft 22 overrides listed after the table below.
The draft 22 own scenarios and the two unscored probes have no draft 21 twin; their
options are listed in `adapters/moqxr/run.sh` and pinned by
`tests/golden/moqxr-cmdlines-d22.txt` (timeout+3 is the scenario timeout plus 3 seconds).
With `--forward 1`, moqxr sends its own PUBLISH and blocks until it is answered, so every
probe in which the runner is the subscriber and does not answer that PUBLISH gets
`--forward 0 --paced`. Only `publisher-location-filter-parameter` answers it and keeps
`--forward 1`. Two shared probes in which the runner is the subscriber are exceptions and keep
their `d21-` twin's `--forward 1`: `d22-unknown-request-stream-message` and
`d22-unknown-datagram-type`. Pacing them lets the stimulus reach moqxr, but in the dig of
2026-10-06 against moqxr `1883b9f` it changed no verdict (see
[the moqxr punch list](moqxr-punch-list.md#status-against-moqxr-1883b9f)). The own draft 22
scenarios:

| `d22-` scenario | moqxr options | Closest draft 21 scenario |
|---|---|---|
| `subscribe-bounded-location-range` | `--forward 0 --paced`, timeout+3 | `d21-update-subscription-location-range` |
| `update-subscription-location-range` | `--forward 0 --paced`, timeout+3 | `d21-update-subscription-location-range` |
| `fetch-bounded-location-range` | `--forward 0 --paced`, timeout+3 | `d21-fetch-datagram-preference` |
| `discover-original-publisher-namespaces` | `--forward 0 --paced`, timeout+3 | `d21-namespace-discovery-authorization` |
| `publisher-location-filter-parameter` | `--forward 1` | `d21-publisher-parameter-serialization` |
| `request-stream-before-peer-setup` | `--forward 0 --paced`, timeout+3 | `d21-successful-subscribe-response` |
| `location-filter-end-group-overflow` | `--forward 0 --paced`, timeout+3 | `d21-location-filter-end-group-overflow` |
| `fill-location-filter-end-group-overflow` | `--forward 0 --paced`, timeout+3 | `d21-location-filter-end-group-overflow` |
| `location-filter-unknown-type` (unscored) | `--forward 0 --paced`, timeout+3 | `d21-location-filter-end-group-overflow` |
| `location-filter-absolute-origin` (unscored) | `--forward 0 --paced`, timeout+3 | `d21-successful-subscribe-response` |

Draft 22 overrides of the draft 21 option lists: these shared scenarios run with
`--forward 0 --paced`, timeout+3 at draft 22, while their `d21-` twin keeps `--forward 1` at
draft 21 (draft 21 command lines are frozen). In each the runner is the subscriber and never
answers moqxr's own PUBLISH; with `--forward 1` moqxr blocks on it and closes with code 0 about
2 seconds later, inside the reaction window, which the runner reads as the reaction to the
stimulus. The draft 22 moqxr sweep of 2026-10-06 showed every one reaching its stimulus when
paced. `tests/e2e/moqxr-adapter-cmdlines.sh` enumerates the same ids as the only exceptions to
its twin-equality check:

| `d22-` scenario | Draft 22 moqxr options | `d21-` twin at draft 21 |
|---|---|---|
| `subscribe-empty-namespace-field` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `subscribe-33-namespace-fields` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `subscribe-tracks-oversized-namespace` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `subscribe-oversized-full-track-name` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `request-undecodable-authorization-token` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `request-token-cache-overflow` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `request-alias-registration-with-default-zero-cache` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `fill-forbidden-nested-authorization` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `fill-forbidden-track-property-filter` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `fill-recursive-parameter` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `fill-invalid-group-order` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `unknown-unidirectional-stream-type` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `unknown-control-message` | `--forward 0 --paced`, timeout+3 | `--forward 1` |
| `successful-subscribe-object-delivery` | `--forward 0 --paced`, timeout+3 | `--forward 1` |

#### The imquic adapter (draft 22)

`adapters/imquic/run.sh` drives imquic's example publisher (`examples/moq-pub.c`, built as
`imquic-moq-pub`), named by `IMQUIC_PUB_BIN`, at draft 22 only. It passes `-M 22 -n media
-N vide_1 -d 4`, translates the endpoint into `-r HOST -R PORT` plus `-q` (native QUIC) or
`-w -H PATH` (WebTransport; the path and any query become the HTTP/3 `:path` unchanged, `/` if
empty; IPv6 literals lose their brackets), and runs the publisher as
`timeout --foreground --preserve-status -k 2 -s TERM <timeout+3>` with its output in
`<log_dir>/publisher.log`. imquic's raw QUIC client sends no PATH or AUTHORITY SETUP
option, so the `moqt://` path is dropped and a `moqt://` query is refused (exit 64), as are
an empty host, a missing or out-of-range port, user information, a fragment and an IPv6
zone. moq-pub reads no fixture (it publishes a clock: one Object per second, one Group per
minute) and verifies no certificate, so `fixture` and `tls_ca` are not used.

The adapter does not `exec` the publisher; it stays alive as a small supervisor. The runner
stops a driver with SIGTERM to its process group and SIGKILLs the group 100 ms later, which
it records as a driver failure (run `error`, `term_signal` 9), and moq-pub needs about
40-160 ms after SIGTERM to send PUBLISH_DONE and PUBLISH_NAMESPACE_DONE and close. So the
adapter starts `timeout` and moq-pub in the background (same process group), and on SIGTERM,
SIGINT or SIGHUP exits 0 at once without forwarding anything: moq-pub was already sent the
group's SIGTERM (and the one `timeout` relays, so two in all, of which it observes one or two,
because pending standard signals coalesce) and finishes on its own.
moq-pub also bumps its stop counter on connection loss, GOAWAY and a refused PUBLISH or
PUBLISH_NAMESPACE, and a signal that takes the counter past two makes it exit(1) without
cleanup: that happens in about 5 percent of runs. It is not a regression (the former
`exec timeout` adapter did the same in 43 of 186 runs of the first imquic sweep) and changed
no verdict. Without a signal it waits and exits with the publisher's status as
`--preserve-status` reports it, also when the deadline fired. The trade-off: after a stop,
moq-pub briefly outlives the adapter without the runner's SIGKILL backstop; it stays bounded
by `timeout -k 2` (SIGKILL at most 2 s after the signal). When you run the adapter by hand, note
that a background child of a shell without job control starts with SIGINT and SIGQUIT ignored:
Ctrl-C ends only the adapter, and moq-pub runs on to its `timeout` deadline. The runner uses
SIGTERM, so this affects interactive use only.

The per-scenario choice is publish-first (`-X`: PUBLISH right after SETUP; every SUBSCRIBE
is refused with DUPLICATE_SUBSCRIPTION, code 0x19, which draft 22 does not define) or
announce-and-wait (no `-X`: PUBLISH_NAMESPACE, then a SUBSCRIBE is accepted and Objects flow
if it carries FORWARD=1; once delivery has started, a further SUBSCRIBE is refused with
DUPLICATE_SUBSCRIPTION, while SUBSCRIBEs without FORWARD=1 start nothing and later ones are
still accepted). In either mode a refused PUBLISH or PUBLISH_NAMESPACE ends the session. It is
derived from the moqxr adapter: moqxr `--forward 1` gives `-X`, `--forward 0` (paced or not)
gives none, and the own scenarios and probes follow the moqxr table above. Where imquic's
modes differ from moqxr's, these shared scenarios deviate from the derivation:

| `d22-` scenario | imquic | moqxr | Why |
|---|---|---|---|
| `complete-subgroup-fin`, `subgroup-start-location-fin` | no `-X` | `--forward 1` | the runner subscribes and judges Subgroup FINs |
| `object-datagram-flags` | no `-X`, `-D datagram` | `--forward 1` | the runner subscribes and judges Object datagrams |
| `original-publisher-opens-new-subgroup`, `publish-track-with-mandatory-property`, `subscribe-single-subgroup` | no `-X` | `--forward 1` | the runner subscribes and judges the Objects |
| `subscribe-accepted` | no `-X` | `--forward 1` | scores the SUBSCRIBE_OK branch (`subscribe-rejected` keeps `-X`) |
| `request-update-overrun`, `request-update-independent-streams` | no `-X` | `--forward 1` | REQUEST_UPDATEs on the runner's own subscriptions |
| `publish-namespace-redirect-nonempty-track-name`, `publisher-namespace-routing-announcement` | no `-X` | `--forward 1` | need the publisher's PUBLISH_NAMESPACE |
| `setup-key-value-type-overflow`, `setup-key-value-declared-length-overflow` | no `-X` | `--forward 1` | the probe's liveness SUBSCRIBE must be accepted; with `-X` moq-pub refuses it (REQUEST_ERROR 0x19) and the row stays unscored |
| `setup-register-default-zero-cache` | no `-X` | `--forward 1` | keeps its liveness SUBSCRIBE from being refused by `-X` (harmless either way); its row `D22-9-1-4-MUST-NOT-318` stays unscored because it is bound to two scenarios and the sibling `setup-register-exceeds-token-cache` needs a MAX_AUTH_TOKEN_CACHE_SIZE of at least 1, which imquic does not announce |
| `publish-update-ok-with-track-properties` | `-X` | `--forward 0 --paced` | its first write answers the publisher's PUBLISH |
| `publish-established-subscriber-sends-publish-state-notify` | `-X` | `--forward 0 --paced` | answers the publisher's PUBLISH, then sends PUBLISH_STATE_NOTIFY on it |
| `subscribe-tracks-publish-skipped-then-capacity-recovers` | `-X` | `--forward 0` | sends its SUBSCRIBE_TRACKS only after the publisher's PUBLISH |

`tests/e2e/imquic-adapter-cmdlines.sh` pins every command line in
`tests/golden/imquic-cmdlines-d22.txt` and checks the derivation and these exceptions;
`tests/e2e/imquic-adapter-contract.sh` checks validation, endpoint translation, the
`timeout` wrapper and the supervisor's shutdown (adapter exit within the 100 ms grace, the
publisher's cleanup and the `-k 2` bound).

Limits of this adapter and publisher: namespace fields and the track name must be printable
ASCII without spaces (the adapter accepts only the reference `media` / `vide_1`); moq-pub
registers no FETCH handler, so start the runner with `--publisher-no-fetch` (or declare
`"publisher_capabilities": {"fetch": false}`); it has no SUBSCRIBE_NAMESPACE or
SUBSCRIBE_TRACKS handler of its own (the library answers NOT_SUPPORTED); and the adapter
passes no emission option other than `-D datagram` (no `-P` padding, no `-f` / `-F` prior
group or object gap, no `-x` Object properties), so the rows that need them stay unscored. The draft 22 sweep
against imquic and its triage are in
[interop-notes.md](interop-notes.md#draft-22-sweep-against-imquic-6836173); the imquic findings
are in [imquic-punch-list.md](imquic-punch-list.md).

How to build, run and test against each bundled peer: [adapters/moqxr/README.md](../adapters/moqxr/README.md) and [adapters/imquic/README.md](../adapters/imquic/README.md).

A real request file from a run:

```json
{
  "draft": 18,
  "endpoint": "moqt://127.0.0.1:19901/moq",
  "fixture": "/path/to/locmaf-publisher.mp4",
  "log_dir": "/path/to/driver-logs/run-18dabdcec655134a",
  "namespace_hex": ["6d65646961"],
  "process_timeout_ms": 9000,
  "run_id": "run-18dabdcec655134a",
  "scenario_id": "subscribe-to-publisher-track",
  "scenario_timeout_ms": 8000,
  "schema_version": 1,
  "tls_ca": "/path/to/cert.pem",
  "track_name_hex": "766964655f31",
  "transport": "native_quic"
}
```

Validate every field before use, refuse anything you cannot map, and decode hex
carefully: names are opaque bytes and may contain NUL or other non-printable
bytes.

### 3.3 Exit codes, lifetime and signals

- Exit 64 (or any nonzero code) with a message on stderr when the adapter itself
  rejects the request, as the bundled adapters do for malformed or unsupported
  input. If the process exits before the publisher connected, the run ends in
  `error` with the logs retained; this is how a request your adapter cannot
  serve surfaces.
- Prefer `exec` so the publisher replaces the adapter and receives signals
  directly. The adapter's own exit status is the publisher's.
- Once the publisher has connected, scores come from MoQT observations on the wire,
  not from the process exit status. A raw-probe context that is still waiting when
  its publisher process fails (nonzero exit, signal, timeout) is recorded as a
  harness error and cannot supply a pass, unless the runner caused the exit
  (it refused the session, closed it as the scenario requires, or the scenario
  treats the publisher giving up as evidence). Exit with status 0 on your own
  deadline or end of work.
- The runner ends the process when a context finishes: it sends SIGTERM to the
  adapter's process group and SIGKILL after a 100 ms grace period. Handle SIGTERM
  promptly. Termination by SIGKILL is counted as a process failure.
- Do not run longer than `process_timeout_ms`. Set your publisher's own deadline at
  or just after `scenario_timeout_ms` (the examples round up to whole seconds) so
  that an idle publisher is ended by the runner, not by an early exit.

### 3.4 Logs and retention

For a typed single-scenario run the files are in `<log-root>/<run-id>/`. For a
multi-scenario run each context has `<log-root>/<run-id>/<ordinal>-<scenario-id>/`.
Each directory holds `request.json`, `stdout.bin` and `stderr.bin` (created with
mode 0600 and never overwritten). When the context ends, a `publisher_process`
event is added to the run with `status` (`exited`, `signaled`, `timed_out`,
`stopped` or `error`), `exit_code`, `term_signal`, and for each log its `path`,
`bytes` and `sha256`:

```sh
curl -s "http://127.0.0.1:8080/api/v1/runs/$id/events?limit=100" \
  | jq '.items[] | select(.kind=="publisher_process") | .detail | fromjson'
```

Keep the log root on persistent storage if you need the logs after the runner
restarts.

### 3.5 What an adapter must never do

An adapter is a translator, not a judge. It must not decide or communicate what MoQT
behavior is expected, must not produce or alter scores or requirement outcomes, and
must not change the publisher's behavior to hide or force a result (for example by
suppressing a message because a scenario would score it). Options that only make a
publisher emit the messages a scenario observes, such as "announce your track", are
fine. The adapter is chosen by the operator at startup and cannot be supplied
through the HTTP API.

## 4. A worked adapter

The example drives a fictional publisher, `acme-pub`, with this command line:

```text
acme-pub --input FILE --connect URL --transport quic|wt --draft 18|21
         --namespace NAME[/NAME...] --track NAME --ca FILE
         --exit-after SECONDS --mode serve|announce
```

`serve` waits for the relay to subscribe; `announce` sends the publisher's own
PUBLISH. The adapter is `examples/harness/adapter.sh` (bash and `jq`):

```bash
#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'acme adapter: %s\n' "$1" >&2
    exit 64
}

[[ "${MOQ_INTEROP_DRIVER_CONTRACT_VERSION:-}" == 1 ]] ||
    fail 'unsupported driver contract version'
request_file=${MOQ_INTEROP_DRIVER_REQUEST_FILE:-}
[[ -n "$request_file" && -r "$request_file" ]] || fail 'request file is unavailable'
# ACME_PUB_BIN overrides the default: an executable named acme-pub next to this
# script, which is where a Docker Compose setup puts it (/opt/publisher).
publisher_bin=${ACME_PUB_BIN:-$(dirname "${BASH_SOURCE[0]}")/acme-pub}
[[ -x "$publisher_bin" ]] || fail "publisher is not executable: $publisher_bin"

# Reject anything this adapter cannot map instead of guessing.
jq -e '
    .schema_version == 1 and
    (.draft == 18 or .draft == 21) and
    (.transport == "native_quic" or .transport == "webtransport") and
    (.endpoint | type == "string" and length > 0) and
    (.fixture | type == "string" and length > 0) and
    (.tls_ca | type == "string" and length > 0) and
    (.scenario_timeout_ms | type == "number" and . >= 1) and
    (.namespace_hex | type == "array" and length >= 1) and
    (.track_name_hex | type == "string")
' "$request_file" >/dev/null || fail 'unsupported or malformed request'

draft=$(jq -r '.draft' "$request_file")
transport=$(jq -r '.transport' "$request_file")
endpoint=$(jq -r '.endpoint' "$request_file")
fixture=$(jq -r '.fixture' "$request_file")
ca_cert=$(jq -r '.tls_ca' "$request_file")
scenario_id=$(jq -r '.scenario_id' "$request_file")
timeout_ms=$(jq -r '.scenario_timeout_ms' "$request_file")

[[ -r "$fixture" ]] || fail 'fixture is unreadable'
[[ -r "$ca_cert" ]] || fail 'TLS CA is unreadable'

# The runner hands out moqt:// for native QUIC and https:// for WebTransport.
case "$transport" in
    native_quic)  [[ "$endpoint" == moqt://* ]] || fail 'native QUIC requires a moqt:// endpoint'
                  acme_transport=quic ;;
    webtransport) [[ "$endpoint" == https://* ]] || fail 'WebTransport requires an https:// endpoint'
                  acme_transport=wt ;;
esac

# Namespace fields and track name arrive as lowercase hex of opaque bytes.
# acme-pub accepts printable names only, so refuse anything else.
hex_to_text() {
    local hex=$1 text= index
    # Printable ASCII only (0x21-0x7e). Checking the hex first also keeps a
    # NUL byte from vanishing inside the command substitution below.
    [[ "$hex" =~ ^(2[1-9a-f]|[3-6][0-9a-f]|7[0-9a-e])+$ ]] ||
        fail 'namespace or track name is not printable ASCII'
    for ((index = 0; index < ${#hex}; index += 2)); do
        text+=$(printf "\\x${hex:index:2}")
    done
    [[ "$text" =~ ^[A-Za-z0-9._-]+$ ]] || fail 'namespace or track name is not a plain name'
    printf '%s' "$text"
}
namespace=
while IFS= read -r field_hex; do
    namespace+="${namespace:+/}$(hex_to_text "$field_hex")"
done < <(jq -r '.namespace_hex[]' "$request_file")
track=$(hex_to_text "$(jq -r '.track_name_hex' "$request_file")")

# acme-pub counts whole seconds; stay alive until the runner's own deadline.
timeout_seconds=$(((timeout_ms + 999) / 1000))

# "serve" waits for the relay to subscribe, "announce" sends the publisher's own
# PUBLISH. The mode only makes acme-pub emit the messages a scenario observes;
# it never describes what the runner should expect.
mode=serve
case "$scenario_id" in
    publish-track-under-single-period-namespace|\
    application-publish-track-in-session-namespace|\
    publish-distinct-content-tracks-in-same-scope|\
    d21-publisher-request-stream-placement)
        mode=announce
        ;;
esac

args=(--input "$fixture" --connect "$endpoint" --transport "$acme_transport"
      --draft "$draft" --namespace "$namespace" --track "$track"
      --ca "$ca_cert" --exit-after "$timeout_seconds" --mode "$mode")

exec "$publisher_bin" "${args[@]}"
```

`adapter.json` next to it is a descriptive manifest in the same shape as
`adapters/moqxr/adapter.json`. The runner does not read it; it documents the
adapter for people and tools:

```json
{
  "adapter_id": "acme-example",
  "driver_contract_version": 1,
  "entrypoint": "adapter.sh",
  "publisher_binary_environment": "ACME_PUB_BIN",
  "supported_drafts": [18, 21],
  "supported_transports": ["native_quic", "webtransport"],
  "protocol_expectations": "none; logs and exit status are diagnostic only"
}
```

A Python variant, `examples/harness/adapter.py`, implements the same translation
with `json` and `os.execv` and no shell. Its core is:

```python
request = json.load(open(request_file, encoding="utf-8"))
args = [publisher, "--input", request["fixture"], "--connect", request["endpoint"],
        "--transport", acme_transport, "--draft", str(request["draft"]),
        "--namespace", namespace, "--track", track, "--ca", request["tls_ca"],
        "--exit-after", str(seconds), "--mode", mode]
os.execv(publisher, args)
```

To try the whole example without a real `acme-pub`, `examples/harness/acme-pub-standin.sh`
is a stand-in that translates `acme-pub` flags into the flags of the sibling `moqxr`
publisher (selected with `MOQXR_BIN`) and execs it. It exists only so the example
can be exercised end to end; your own publisher replaces it.

## 5. Unit-testing the adapter without the runner

Test the adapter by replacing the publisher with a capture stub that prints each
argument it receives inside angle brackets, then assert on the output. This is the
pattern of `tests/e2e/moqxr-adapter-contract.sh` and
`tests/support/capture_publisher.sh`:

```bash
#!/usr/bin/env bash
printf '<%s>\n' "$@"
```

The test builds a request file with `jq`, sets the two environment variables and the
publisher path, runs the adapter, and checks the argument vector. Paths with spaces
make quoting mistakes visible. It also feeds the adapter requests it must refuse
(wrong draft, mismatched endpoint scheme, binary names, missing trust file, missing
contract version) and asserts a nonzero exit. The core of the pattern:

```bash
output=$(ACME_PUB_BIN="$test_dir/publisher binary" \
    MOQ_INTEROP_DRIVER_CONTRACT_VERSION=1 \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter")
[[ "$output" == *"<$test_dir/fixture with spaces.mp4>"* ]]
[[ "$output" == *"<--draft>"* && "$output" == *"<21>"* ]]

if ACME_PUB_BIN="$test_dir/publisher binary" \
    MOQ_INTEROP_DRIVER_REQUEST_FILE="$test_dir/request.json" "$adapter" >/dev/null 2>&1; then
    echo 'missing contract version unexpectedly accepted' >&2; exit 1
fi
```

`examples/harness/test-adapter.sh` is the complete version for the example adapter; it
takes the adapter path as an optional argument so it also tests `adapter.py`:

```sh
bash examples/harness/test-adapter.sh
bash examples/harness/test-adapter.sh "$PWD/examples/harness/adapter.py"
```

## 6. Running the adapter against the runner

### 6.1 Locally

This is the same shape as `tests/e2e/driven-moqxr.sh`. Run it from the repository
root, with the certificate from section 2.1 (`work/cert.pem`, which has a subject
alternative name for 127.0.0.1) and the example stand-in as the "publisher":

```sh
export ACME_PUB_BIN="$PWD/examples/harness/acme-pub-standin.sh"
export MOQXR_BIN=/path/to/moqxr/build/openmoq-publisher

build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --database "$PWD/work/runs.sqlite3" \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 4443 --publisher-port-end 4452 \
  --tls-cert "$PWD/work/cert.pem" --tls-key "$PWD/work/key.pem" \
  --driver-executable "$PWD/examples/harness/adapter.sh" \
  --driver-fixture /path/to/moqxr/tests/fixtures/locmaf-publisher.mp4 \
  --driver-log-root "$PWD/work/driver-logs"
```

In another terminal:

```sh
examples/harness/run-scenarios.sh 18 native-quic driven subscribe-to-publisher-track
examples/harness/run-scenarios.sh 21 webtransport driven d21-publisher-request-stream-placement
```

The flags:

| Flag | Role |
|---|---|
| `--driver-executable` | Absolute path of the adapter |
| `--driver-arg` | Optional arguments passed to the adapter (repeatable) |
| `--driver-fixture` | Becomes `fixture` in the request |
| `--driver-ca` | Becomes `tls_ca`; defaults to the `--tls-cert` file |
| `--driver-log-root` | Parent directory for per-run request files and logs; required with `--driver-executable` |
| `--publisher-advertise` | Host in the URI the adapter receives |

Replace the stand-in with your publisher by setting `ACME_PUB_BIN` (or the variable
your adapter reads) and adapting the flag mapping. The environment of the runner is
inherited by the adapter, which is how that variable reaches it. The driven-mode
check scripts are `tests/e2e/driven-moqxr.sh` (one draft and transport) and
`tests/e2e/moqxr-matrix.sh` (the adapter contract, then drafts 18, 21 and 22 over native
QUIC and WebTransport: six pairs). Each pair runs a single scenario
(`subscribe-to-publisher-track` for draft 18, `d21-publisher-request-stream-placement`
for draft 21, `d22-publisher-request-stream-placement` for draft 22), so the matrix is a
smoke test of the harness, not a conformance sweep.

### 6.2 In Docker Compose

Compose runs the adapter inside the runner container, so the publisher, its fixture
and the adapter must be present in that container. Put them in a directory that is
mounted read-only at `/opt/publisher`:

```sh
mkdir -p publisher tls
cp examples/harness/adapter.sh publisher/
cp /path/to/your-publisher publisher/acme-pub        # the adapter's default lookup
cp /path/to/fixture.mp4 publisher/fixture.mp4
chmod -R a+rX publisher tls                          # container UID 10001 must read them (test keys only)
cp work/cert.pem work/key.pem tls/

export MOQ_INTEROP_SOURCE_REVISION="$(git rev-parse HEAD)"
export SOURCE_DATE_EPOCH=1790467200
scripts/container-build.sh build
MOQ_INTEROP_TLS_DIR="$PWD/tls" \
MOQ_INTEROP_PUBLISHER_DIR="$PWD/publisher" \
MOQ_INTEROP_PUBLISHER_HOST=127.0.0.1 \
MOQ_INTEROP_DRIVER_EXECUTABLE=/opt/publisher/adapter.sh \
MOQ_INTEROP_DRIVER_FIXTURE=/opt/publisher/fixture.mp4 \
  docker compose up
```

The container image provides `bash`, `curl` and `jq` but no Python, so use a bash
adapter or ship a static binary. Your publisher must run in that container (a
Debian bookworm userland); a binary linked against a newer system library may not.
Because `compose.yaml` passes only a fixed set of variables into the container,
let the adapter find the publisher by path (as the example does) rather than
through a new environment variable, or extend the Compose file. In driven mode the
publisher runs inside the container, so `127.0.0.1` is correct for
`MOQ_INTEROP_PUBLISHER_HOST`; for observed mode from another host set it to an
address that host can reach and publish the UDP range. Driver logs are kept in the
`validator-results` volume under `driver-logs/<run-id>`.

## 7. Requirements your publisher must meet

- **QUIC DATAGRAM is required** (draft 18 section 3.1, and draft 21 section 6.2 for
  native QUIC and HTTP/3 DATAGRAM for WebTransport). A native QUIC client that does
  not negotiate it is refused before SETUP: the runner closes the connection with an
  application error and the reason `QUIC DATAGRAM not negotiated`. The run then holds
  only a `local_close` event, no SETUP and no scored behavior; a typed driven run
  ends `error` ("publisher exited before connecting"), and in raw-probe runs the context
  records `publisher_exit_after_refusal` and its rows stay `not_run`. Enable QUIC
  datagrams (a nonzero `max_datagram_frame_size` transport parameter) in your
  publisher's QUIC stack.
- **Exact ALPN.** Native QUIC uses `moqt-18` for draft 18, `moqt-21` for draft 21 and
  `moqt-22` for draft 22.
  There is no fallback to another draft; a mismatch is rejected and recorded.
- **WebTransport profile.** The runner admits only a strict profile: HTTP/3 (ALPN
  `h3`) with the WebTransport-capable HTTP/3 settings, HTTP/3 and QUIC DATAGRAM
  and RESET_STREAM_AT negotiated, an extended CONNECT request with the
  `webtransport-h3` protocol, scheme `https`, the authority and path from the
  returned URL (`/moq`), and `WT-Available-Protocols` as a Structured Fields list of
  strings containing the exact `moqt-18`, `moqt-21` or `moqt-22` (the runner selects
  it in `WT-Protocol`). Draft 18 references `draft-ietf-webtrans-http3-15`, and
  drafts 21 and 22 `draft-ietf-webtrans-http3-16`. Legacy WebTransport settings or protocol tokens
  are rejected before any MoQT bytes are scored. A client that sends `Origin` must
  use an origin the operator listed with `--publisher-origin`.
- **URIs.** Native QUIC: `moqt://host:port/moq`. WebTransport: `https://host:port/moq`.
  A publisher given the wrong scheme for the run's transport cannot connect.
- **The track fixture.** The runner requests the track named in the run's `track`.
  Your publisher must publish that namespace and track name. The reference setup uses
  namespace `media` and track `vide_1`; to change it, change `track` in the run
  request (and `--namespace`/`--track` in the adapter), keeping the three in sync.
  Some scenarios also expect specific content in the track (for example Group 7,
  Object 9); see [scenario-reference.md](scenario-reference.md).
- **Publisher-initiated behavior.** The runner subscribes, fetches and sends requests;
  it can only react to what your publisher sends. Behaviors your publisher never
  produces (for example a GOAWAY, PUBLISH_STATE_NOTIFY or TRACK_STATUS query) leave
  those rows `not_run`, which is not a failure. Use the adapter's `scenario_id` to
  start the publisher in a mode that produces the messages a scenario observes.
- **Credentials and other operator fixtures.** Rows about authorization tokens score
  only when the operator starts the runner with `--invalid-auth-token TYPE:HEX`,
  `--expired-auth-token TYPE:HEX`, `--denied-authorization-token VALUE` or
  `--unknown-auth-token-alias-compat-code CODE`, and your publisher is configured to
  match. Without them those rows stay `not_run`. The details are in
  [scenario-reference.md](scenario-reference.md).

### Declaring what your publisher does not implement

The drafts allow an endpoint that is not a relay to implement only the part of MOQT
it needs, and a limited endpoint SHOULD answer a message it does not support with
NOT_SUPPORTED instead of ignoring it (draft 18 Section 4, draft 21 Section 1.5).
A live publisher with no cache or history, such as moqxr, normally has no FETCH. Not
implementing FETCH is not a conformance failure, so tell the runner instead of
letting the scenarios that start with a FETCH (23 in draft 18, 22 in draft 21, 23 in
draft 22) end in run-level errors:

```sh
# once, when starting the runner
build/moq-interop-runner ... --publisher-no-fetch

# or per run
curl -sS -X POST http://127.0.0.1:8080/api/v1/runs -H 'Content-Type: application/json' -d '{
  "draft": 18, "transport": "native-quic", "mode": "driven", "timeout_ms": 8000,
  "scenarios": ["receive-unknown-message-type", "cancel-fetch-request-with-open-data-stream"],
  "publisher_capabilities": {"fetch": false},
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}}'
```

A value in the run wins over the flag (`{"fetch": true}` re-enables FETCH scenarios
for one run on a runner started with `--publisher-no-fetch`). What you see:

- A selection that contains only FETCH scenarios is refused with 422
  `scenario_requires_publisher_capability`. Remove the declaration, or select
  scenarios your publisher can run.
- In a mixed selection the FETCH scenarios are skipped: no context and no publisher
  process, one `context_skipped` event each (`publisher declared no FETCH support`),
  `# SKIP` in the TAP export. They do not turn the run into `error` or `fail`.
- Catalog rows whose every scenario needs FETCH are `not_applicable` and are left
  out of the required, weighted and coverage scores, so the denominators describe
  what your publisher can be asked. A row that also names a scenario that does not
  need FETCH stays `not_run`; the runner never hides a scenario a row needs. The
  JSON export and the HTML report give the reason for every `not_applicable` row.
- Scenarios not tagged `requires_fetch` still run in full, including ones that
  exercise behavior you may not implement for other reasons; those show up as
  ordinary `fail` or `not_run` rows.

The declaration is stored with the run (`config.publisher_capabilities` and a
`publisher_capabilities` event), so a result says what it was run against. `fetch`
is the only capability so far. To see which scenarios are affected, read
`requires_fetch` in `GET /healthz`, or the list in
[scenario-reference.md](scenario-reference.md#scenarios-that-need-fetch).

### 7.1 moq-lite-06 publishers (L1d)

These notes describe what an adapter for a moq-lite-06 publisher (for example the moq CLI) must provide. moq-lite-06
runs are not startable through the HTTP API yet (L1e flips that); the expectations below are what the scenarios
already assume.

- **The publisher under test is the client.** It dials the runner; the runner is the server and the subscriber. The
  runner sends its own SETUP stream first (except in the violation probes) and does not offer QUIC DATAGRAM
  or require it: a moq-lite-06 listener accepts a peer that did not negotiate datagrams. The ALPN is `moq-lite-06`
  for native QUIC; a publisher that offers another ALPN ends the run with a harness error (verdict `error`).
- **Endpoint forms are provisional.** Native QUIC is `moql://host:port` and WebTransport is `https://host:port/moq`.
  Both are fixed in L1e against the moq CLI and may change; do not hard-code them in a published adapter.
- **Capabilities some scenarios need from the adapter:**
  - `l06-setup-client-path` needs the publisher to be given a session URL with a path and a query (unreserved
    characters only, for example `/moq?token=l1d`) and to put them in its SETUP Path (native QUIC; the path and query
    appended) or send no Path (WebTransport). Without them rows 120, 124 and 125 are `not_run`.
  - `l06-announce-lifecycle` needs the publisher to end a broadcast and start it again within one session. Without that,
    row 152 is `not_run`.
  - The unknown-code and reserved-code probes (`l06-errors-unknown-reset-code`, `l06-errors-reserved-reset-code`) run
    about 9 s and 6 s. The publisher needs a media source that keeps producing for that long; a source that ends
    inside the probe makes the publisher close the session with NO_ERROR, which those rows judge as a Fail.
  - Ten scenarios need the track fixture (broadcast path and track name); the adapter must publish exactly that
    broadcast and track.
- **Results are staged.** A moq-lite-06 run covers 30 testable rows of a catalog with 75 still-unreviewed rows, so its
  verdict is `incomplete` or `fail`, never `pass`; unreviewed rows are listed as not tested.

## 8. Interpreting results

| Outcome | Meaning |
|---|---|
| `pass` | The wire evidence satisfies the requirement in this run |
| `fail` | The wire evidence contradicts the requirement |
| `not_run` | The row is testable but the scenario did not run, did not complete, or the publisher did not produce the behavior |
| `not_testable` | The runner cannot observe this behavior; a reason and draft citation are recorded |
| `not_applicable` | The statement does not apply to a contribution publisher, or (a scored row) every scenario it names needs a capability this run declared your publisher does not implement; the export says which |

The run verdict is `pass`, `fail` (any applicable MUST or MUST NOT failed),
`incomplete` (nothing failed but some applicable rows are `not_run`) or `error` (the
runner could not run or preserve the test, for example the publisher exited before
connecting). **`incomplete` is the normal result**: each scenario exercises a slice
of the catalog, so a run that selects a few scenarios leaves most rows `not_run`.
`incomplete` and `error` are not publisher failures. A passing row is evidence from
one run, not a conformance claim.

To read a failure:

1. Find the failing rows: `jq '.requirements[] | select(.outcome=="fail")' result.json`
   or the HTML report with `?outcome=fail`. Each row has the requirement ID, its
   `summary`, `scenarios` and `source` (section, first and last line in
   `docs/draft-ietf-moq-transport-18.txt` or `-21.txt`).
2. Read the cited draft lines. They are the authority.
3. Follow `evidence_sequences` to the events. `GET /api/v1/runs/{id}/events` shows,
   in order, the transport events, decoded messages, stimulus the runner sent
   (`raw_probe_stimulus`), the bytes the runner received and the close codes, with
   timestamps. The request and response bytes are in the event `detail`.
4. Compare the evidence with the draft text. Check the `publisher_process` event
   and the retained `stderr.bin` to see what your publisher reported.

If you believe the runner is wrong, the drafts decide, not the runner and not any
implementation. Report it with the requirement ID, the run's JSON export, the draft
file and the lines you read, and what you believe the draft requires.

## 9. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Run ends `error`; events show `publisher_process` with `status` `exited` and no `peer_setup_received` | The publisher (or the adapter, exit 64) exited before connecting. Read `stderr.bin` in the run's log directory; run the adapter by hand with a saved `request.json` |
| `error` verdict, `harness_error` event | The `detail` of the event (also `run_error_reason` in the run JSON and the report) says why: the publisher process failed while the context was running, it did not stop within the 100 ms grace after SIGTERM (handle SIGTERM and exit 0 on your own deadline), or the transport rejected a runner write |
| A context has a `context_event_limit` event and its rows stay `not_run`; the verdict is `incomplete` | The publisher sent more than one context records (4096 events or 4 MiB of stream data) before the scenario finished, typically media at a real bitrate while the probe was still waiting. The context is not scored and the run continues. Send less media while a probe is open, or run fewer, shorter scenarios |
| Only a `local_close` event; publisher logs a close with `QUIC DATAGRAM not negotiated` | DATAGRAM is not negotiated. Enable QUIC datagrams in the publisher's QUIC stack |
| TLS or certificate errors in the publisher log | The publisher does not trust `cert.pem`, or the certificate has no subject alternative name for the address used. Regenerate with `-addext subjectAltName=...` and pass the file as the trust anchor (`tls_ca`) |
| `tls_ca` or fixture path "unreadable" in the adapter | The runner was started with a relative `--tls-cert` or `--driver-fixture` and the adapter runs elsewhere. Use absolute paths |
| 503 `publisher_ports_exhausted` | All ports in `--publisher-port-start`..`--publisher-port-end` are in use, or the scenario needs two. Widen the range; finish or stop (`POST /api/v1/runs/{id}/stop -d ''`) active runs |
| 503 `publisher_listener_unavailable` | The runner has no `--tls-cert`/`--tls-key` |
| 422 `unsupported_run_config` | Unknown scenario ID, typed and raw scenarios mixed in one run, or `mode` `driven` without `--driver-executable` |
| 422 `scenario_requires_publisher_capability` | Every selected scenario needs FETCH and the run (or `--publisher-no-fetch`) declares the publisher has none. See "Declaring what your publisher does not implement" |
| 400 `invalid_publisher_capabilities` | `publisher_capabilities` is not an object, names something other than `fetch`, or `fetch` is not a boolean |
| 400 `invalid_run_config` | The scenario needs `track`; `driven` always needs `track`; `timeout_ms` below 2 or above 3600000; bad hex in the track |
| Publisher cannot connect to the endpoint | Wrong scheme for the transport: native QUIC is `moqt://host:port/moq`, WebTransport is `https://host:port/moq`. Or `--publisher-advertise` is not reachable from the publisher (use the container network address, not 127.0.0.1) |
| WebTransport CONNECT rejected | The client does not meet the strict profile above, or sent an `Origin` the operator did not allow with `--publisher-origin` |
| Context times out; rows `not_run` | The publisher was too slow to connect or respond. Raise `timeout_ms` (it applies to each context) when the machine is loaded; runs against a container or on a busy CI host need more headroom |
| Many rows `not_run` | Expected. See "What NOT_RUN means" in [scenario-reference.md](scenario-reference.md); the publisher may never send the messages those rows observe |
| Runner exits with status 2 at start | A flag combination is invalid (see [building-and-running.md](building-and-running.md)); the message names it |

## 10. CI usage

The runner has no pass/fail exit status of its own; a CI job starts it, creates runs
through the API, waits for them to finalize, and decides from the results.
`examples/harness/run-scenarios.sh` does this and exits 0 for `pass` or `incomplete`,
1 for `fail` and 2 for `error`, an invalid request or a run that did not finalize;
set `FAIL_ON_INCOMPLETE=1` to treat `incomplete` as a failure.

```sh
OUT_DIR=ci-results TIMEOUT_MS=15000 \
  examples/harness/run-scenarios.sh 21 webtransport driven d21-publisher-request-stream-placement
```

- Give every context a generous `timeout_ms`; CI hosts are slower than laptops.
- The script saves `results/<run-id>.json` and `.tap`. In a CI system that reads
  TAP, remember that a scenario whose rows are not all `pass` is `not ok`, so an
  `incomplete` scenario appears as `not ok`; use the JSON verdict to decide.
- Gate on specific rows with `jq`, for example to fail the job when any MUST row
  failed: `jq -e '[.requirements[] | select(.outcome=="fail" and .required)] | length == 0' results/RUN.json`.
- Retain as artifacts: the SQLite database (`--database`), the driver log root
  (`--driver-log-root`; request file, `stdout.bin`, `stderr.bin` for each context)
  and the exported JSON and TAP files. Compare repeated runs with
  `build/moq-interop-audit --draft 18 --database runs.sqlite3` only when the
  publisher binary and fixture are identical (see [scoring-and-audit.md](scoring-and-audit.md)).
- Use a fresh database or a unique run directory per job, a port range that does not
  collide with other jobs on the host, and stop the runner at the end of the job.
