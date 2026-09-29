# moq-contribution-interop-runner

Publisher-focused MoQT interoperability runner. The checked-in draft text in
`docs/` is the protocol authority. The requirement inventories cover drafts 18
and 21. The executable scenario currently covers one draft-18 native-QUIC
publisher interaction: subscribe to a configured track and evaluate its initial
response. Draft 21, WebTransport, and the remaining publisher requirements are
cataloged but not executable yet. Requests for them return HTTP 422; they are
never silently scored as conformant.

Build and test on Linux with `cmake -S . -B build`,
`cmake --build build -j4`, and `ctest --test-dir build --output-on-failure`.
Runtime options are listed by `build/moq-interop-runner --help`.

To run the executable scenario, provide a PEM certificate and private key:

```sh
build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --publisher-bind 127.0.0.1 --publisher-port-start 4443 \
  --publisher-port-end 4452 --tls-cert cert.pem --tls-key key.pem
```

Without both TLS files, the HTTP inventory and stored-result endpoints remain
available, but POST `/api/v1/runs` returns HTTP 503 for executable scenarios.
The certificate must be trusted by the publisher being tested. The service
binds one UDP port per active run, up to the configured port range. It does not
forward to subscribers.

The HTTP run configuration accepts an optional opaque-byte track fixture:

```json
{
  "draft": 18,
  "transport": "native-quic",
  "mode": "observed",
  "scenarios": ["subscribe-to-publisher-track"],
  "timeout_ms": 1000,
  "track": {"namespace_hex": ["6e"], "name_hex": "78"}
}
```

`namespace_hex` is an ordered array of 0–32 nonempty hex-encoded namespace
fields; `name_hex` is the possibly empty hex-encoded Track Name. The decoded
full name is limited to 4,096 bytes. Hex encoding preserves arbitrary bytes,
including NUL, without imposing a text canonicalization on publishers.

Create a run with `curl -sS -X POST http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' -d @run.json`, where `run.json` contains
the JSON above. HTTP 201 returns the run ID and `publisher_endpoint` with
`address`, `port`, and ALPN `moqt-18`. Configure the publisher to connect to
that endpoint, then retrieve `GET /api/v1/runs/{id}` or
`GET /api/v1/runs/{id}/events`. `GET /results` is the HTML summary, and
`GET /api/v1/requirements?draft=18` or `draft=21` lists catalog entries.
`POST /api/v1/runs/{id}/stop` ends an active run and finalizes it as incomplete;
it does not count the interrupted interaction as a publisher failure.
Scoring distinguishes required MUST/MUST NOT, weighted recommendations, and
coverage; unexecuted requirements remain visible rather than counting as
passes. An incomplete or harness-failed run is not a publisher failure.

For Docker Compose, put `cert.pem` and `key.pem` in a directory readable by
container UID 10001, set `MOQ_INTEROP_TLS_DIR` to that directory, and set
`MOQ_INTEROP_PUBLISHER_HOST` to the externally reachable address. Use
`MOQ_INTEROP_UDP_BIND`, `MOQ_INTEROP_UDP_START`, and `MOQ_INTEROP_UDP_END` to
match the published UDP range. `scripts/container-build.sh build` creates the
image from a clean committed tree; `docker compose up` starts the HTTP and UDP
listeners. The HTTP endpoint is bound to localhost by default. Keep it on a
trusted network because the API has no authentication yet.
