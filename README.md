# moq-contribution-interop-runner

A relay-side validator for Media over QUIC Transport (MoQT) contribution
publishers. It presents itself to a publisher as the receiving end of a relay,
drives controlled interactions (subscribing, fetching, sending malformed or
unusual input), records what the publisher puts on the wire, and scores that
evidence against the publisher-applicable requirements of
`draft-ietf-moq-transport-18`, `draft-ietf-moq-transport-21` and
`draft-ietf-moq-transport-22`.

- Each draft scored independently; native QUIC and WebTransport.
- Runs as a plain process or as a Docker container.
- Results are served over HTTP as JSON, TAP 14 and an HTML report, and stored in
  SQLite.
- Two ways to connect a publisher: start it yourself pointed at the runner
  (observed mode), or let the runner launch it through an adapter you provide
  (driven mode).
- The checked-in draft texts in `docs/` are the protocol authority. No
  implementation, including the example publishers used for testing, defines
  expected behavior.

The runner does not forward objects to subscribers and does not validate media
payloads; payload bytes are opaque.

## Status

- Every applicable, testable MUST/MUST NOT row of each draft has a bound scenario
  and evaluator: 173 of 173 for draft 18, 173 of 173 for draft 21 and 170 of 170
  for draft 22 (check with `build/moq-interop-audit --draft 18`, `--draft 21` and
  `--draft 22`). Draft 22 reuses draft 21's scenarios where the requirement did
  not change and adds its own where it did; its runs use `d22-` scenario IDs.
- Optional SHOULD/MAY coverage is low: 1 of 90 rows for draft 18, 1 of 97 for
  draft 21 and 3 of 97 for draft 22.
- A binding is not proof that a publisher passed. Most scenarios pass only on
  positive wire evidence, and a publisher that never produces the behavior leaves
  the row `NOT_RUN`. `incomplete` is the normal verdict of a run that selects a
  few scenarios.
- Some rows score only with operator-supplied fixtures, such as token
  credentials (`--invalid-auth-token`, `--expired-auth-token`,
  `--denied-authorization-token`); without them they stay `NOT_RUN`.
- Rows that cannot be observed on the wire are classified `not_testable` (or
  `not_applicable`) with a reason and a draft citation, and are excluded from
  scores.
- The HTTP API has no authentication. Keep it on a trusted network.

## Quick start

Requirements: Linux, CMake 3.24+, a C++20 compiler, OpenSSL and SQLite3
development packages, `git`, `perl`, `pkg-config`; the first configure downloads
pinned dependencies. Details are in [docs/building-and-running.md](docs/building-and-running.md).

```sh
cmake -S . -B build
cmake --build build -j4

# Test certificate with a subject alternative name for loopback.
mkdir -p work
openssl req -x509 -newkey rsa:2048 -nodes -keyout work/key.pem -out work/cert.pem \
  -subj /CN=localhost -days 7 -addext subjectAltName=DNS:localhost,IP:127.0.0.1

build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 4443 --publisher-port-end 4452 \
  --tls-cert "$PWD/work/cert.pem" --tls-key "$PWD/work/key.pem"
```

Create a first run in another terminal (observed mode, draft 18, native QUIC):

```sh
curl -sS -X POST http://127.0.0.1:8080/api/v1/runs -H 'Content-Type: application/json' -d '{
  "draft": 18, "transport": "native-quic", "mode": "observed",
  "scenarios": ["subscribe-to-publisher-track"], "timeout_ms": 8000,
  "track": {"namespace_hex": ["6d65646961"], "name_hex": "766964655f31"}
}'
```

The response contains `publisher_endpoint` (address, port and ALPN `moqt-18`).
Point your publisher at `moqt://ADDRESS:PORT/moq`, with the certificate trusted
and QUIC DATAGRAM enabled, then read the outcome:

```sh
curl -s http://127.0.0.1:8080/api/v1/runs/RUN_ID | jq '.run | {state, verdict, score}'
curl -s http://127.0.0.1:8080/results/RUN_ID.json | jq '.requirements[] | select(.outcome=="fail")'
```

`/results/RUN_ID` is the HTML report and `/results/RUN_ID.tap` the TAP export. To
have the runner launch your publisher for you, follow the
[harness guide](docs/publisher-harness-guide.md).

## Documentation

| Document | What it covers |
|---|---|
| [docs/publisher-harness-guide.md](docs/publisher-harness-guide.md) | Build a harness for your publisher and run it against the runner: observed and driven mode, the driver contract, a worked adapter, unit-testing it, Docker Compose, publisher requirements, reading results, troubleshooting, CI |
| [docs/building-and-running.md](docs/building-and-running.md) | Build from source, TLS material, every runner flag, Docker and Compose, persistence and recovery |
| [docs/http-api.md](docs/http-api.md) | Routes, run request and response JSON, run lifecycle, events, JSON/TAP/HTML exports, completeness endpoint, error codes |
| [docs/scoring-and-audit.md](docs/scoring-and-audit.md) | Outcome states, weights, verdicts, scores, `moq-interop-audit`, release audit, sanitizer and fuzz scripts |
| [docs/scenario-reference.md](docs/scenario-reference.md) | Per-family fixture contracts, operator credentials, port and transport requirements, what `NOT_RUN` means |
| [docs/interop-notes.md](docs/interop-notes.md) | Publisher compatibility notes: the bundled moqxr adapter, observed results, the standing rule on expected behavior |
| [docs/moq-contribution-interop-runner-design.md](docs/moq-contribution-interop-runner-design.md) | Design: goals, architecture, requirement catalog, scoring model, verification strategy |
| [docs/draft-ietf-moq-transport-18.txt](docs/draft-ietf-moq-transport-18.txt), [-21.txt](docs/draft-ietf-moq-transport-21.txt), [-22.txt](docs/draft-ietf-moq-transport-22.txt) | The protocol authority (checked in, digests recorded in `requirements/draft-digests.json`) |
| [docs/plans/](docs/plans/) | Historical implementation plans |

## Repository layout

| Path | Contents |
|---|---|
| `src/`, `include/moq/interop/` | Runner source, grouped by `app`, `http`, `requirements`, `scenarios`, `session`, `storage`, `transport`, `wire` |
| `requirements/` | Requirement catalogs (`draft18.json`, `draft21.json` and `draft22.json`), the draft 21 to 22 delta audit, schema and draft digests |
| `adapters/` | Driver contract schema and the bundled `moqxr` adapter |
| `examples/harness/` | Worked example adapter (bash and Python), capture-stub test, run helper |
| `docs/` | Documentation and the draft texts |
| `tests/` | Unit, golden, protocol, integration, end-to-end and fuzz tests |
| `scripts/` | `container-build.sh`, which builds the pinned image from a clean tree |
| `Dockerfile`, `compose.yaml` | Container image and Compose service |

## Tests

```sh
ctest --test-dir build -j2 --timeout 600 --output-on-failure
bash tests/e2e/moqxr-adapter-contract.sh              # adapter mapping, no network
bash examples/harness/test-adapter.sh                 # example adapter, no network
build/moq-interop-audit --draft 18                    # static completeness gate (also --draft 21)
```

The default suite needs `jq`, `openssl` and `curl`. Checks against an external
publisher (`tests/e2e/moqxr-matrix.sh` and the smoke scripts), Docker checks, and
the sanitizer, fuzz and release-audit scripts are opt-in; see
[docs/interop-notes.md](docs/interop-notes.md) and
[docs/scoring-and-audit.md](docs/scoring-and-audit.md).

## License and contributing

Licensed under the Apache License 2.0; see [LICENSE](LICENSE). Issues and pull
requests are welcome on the project repository. When reporting that the runner
scored a row incorrectly, cite the requirement ID and the draft lines it
quotes: the checked-in drafts decide, not the runner and not any publisher.
