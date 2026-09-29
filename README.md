# moq-contribution-interop-runner

Publisher-focused MoQT interoperability runner. The checked-in draft text in
`docs/` is the protocol authority. The requirement inventories cover drafts 18
and 21. The draft-18 native-QUIC scenarios subscribe to a configured track,
optionally subscribe again to verify rejection with `DUPLICATE_SUBSCRIPTION`,
or issue a standalone FETCH and check for exactly one response. Draft-21 native-QUIC
profiles accept a publisher's PUBLISH for a
configured track and send an empty REQUEST_OK. WebTransport and
the remaining publisher requirements are cataloged but not executable yet.
Requests for unsupported scenarios return HTTP 422; they are never silently
scored as conformant.

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

For the draft-18 duplicate-subscription check, set `scenarios` to
`["subscribe-again-to-established-publisher-track"]`. The runner waits for
`SUBSCRIBE_OK` to the first request, sends a second SUBSCRIBE for the identical
track, and scores `D18-5-1-MUST-004` from the response code. If the first
subscription is not established, that conditional requirement remains
`NOT_RUN` rather than becoming a publisher failure.

For the draft-18 FETCH response check, use
`["fetch-publisher-track-range"]`. The runner sends a standalone FETCH for
the configured track from Location `{0, 0}` to `{0, 1}` and scores
`D18-5-2-MUST-001` when exactly one `FETCH_OK` or `REQUEST_ERROR` is
observed. The track fixture names the request target; it does not assert that
the publisher has already published an Object. This profile does not score
the response code, range validity, or FETCH object delivery.

For draft 21, set `draft` to `21` and select exactly one of
`d21-publisher-request-stream-placement`, `d21-setup-unknown-options`, or
`d21-setup-duplicate-unknown-options`, `d21-server-sends-authority`, or
`d21-server-sends-path` in `scenarios`; the endpoint advertises ALPN `moqt-21`.
The two unknown-option SETUP profiles send the draft-21 reserved GREASE option
type `0x9D` once or twice, then require a valid PUBLISH and REQUEST_OK before
scoring receiver requirements `D21-9-1-MUST-287`, `-288`, and (for duplicates)
`-290` as passes. A close or timeout without that exchange stays `NOT_RUN`,
not a publisher failure. The draft-21 runner waits for both SETUP messages, including when
PUBLISH arrives before SETUP completes, and records the observed PUBLISH and
REQUEST_OK. This is a publisher-announcement test, not a subscriber-serving
relay or a full draft-21 conformance test. Unexercised catalog lines remain
`NOT_RUN`, so a valid run against the full catalog remains incomplete. An
invalid first message on a publisher-opened request stream is recorded and
fails `D21-6-3-MUST-NOT-141`; a different permitted but unsupported opener is
not reported as a publisher failure.

The AUTHORITY and PATH profiles deliberately send an otherwise well-formed
server SETUP with one role-forbidden option. They score
`D21-9-1-1-MUST-293` or `D21-9-1-2-MUST-300` only after the local SETUP and
the publisher client's application close are observed. The expected close codes
are `INVALID_AUTHORITY` (`0x19`) and `INVALID_PATH` (`0x8`); a different
application close code fails the selected requirement, while an unobserved or
transport-level close remains `NOT_RUN`. A completed publication after the
forbidden option, without the required close, also fails the requirement.

`namespace_hex` is an ordered array of 0–32 nonempty hex-encoded namespace
fields; `name_hex` is the possibly empty hex-encoded Track Name. The decoded
full name is limited to 4,096 bytes. Hex encoding preserves arbitrary bytes,
including NUL, without imposing a text canonicalization on publishers.

Create a run with `curl -sS -X POST http://127.0.0.1:8080/api/v1/runs \
  -H 'Content-Type: application/json' -d @run.json`, where `run.json` contains
the JSON above. HTTP 201 returns the run ID and `publisher_endpoint` with
`address`, `port`, and the selected draft's ALPN. Configure the publisher to connect to
that endpoint, then retrieve `GET /api/v1/runs/{id}` or
`GET /api/v1/runs/{id}/events`. `GET /results` is the HTML summary, and
`GET /api/v1/requirements?draft=18` or `draft=21` lists catalog entries.
`GET /healthz` distinguishes the two inventoried drafts from the narrow
executable profiles and whether their listeners are configured.
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

An opt-in black-box check against an external publisher is available:

```sh
bash tests/e2e/draft18-native-moqxr.sh \
  "$PWD/build/moq-interop-runner" \
  "/path/to/openmoq-publisher" \
  "/path/to/moqxr/tests/fixtures/locmaf-publisher.mp4"
```

Pass `21` as a fourth argument to exercise the draft-21 PUBLISH-announcement
profile with `moqxr --preannounce-tracks`; omitting it selects draft 18. The
script checks the returned ALPN and prints publisher and runner logs on failure.
In a 2026-09-29 test with `moqxr` build `g478d6c0.dirty`, the picoquic client
did not negotiate QUIC DATAGRAM, so the draft-21 attempt ended at the
transport gate before any PUBLISH could be scored. This is an interop
observation, not a validator pass or a reason to bypass that draft requirement.

The script starts a loopback runner with temporary TLS material, asks the
publisher to connect, prints the run verdict and any scored requirements, and
removes its temporary files. It requires `openssl`, `curl`, and `jq`; it is not
part of the default CTest suite because `moqxr` and media input are external.
This fixture publishes the `media` namespace and `vide_1` track expected by
the script.
If the publisher omits QUIC DATAGRAM negotiation, the runner rejects the
session as required by draft 18 section 3.1, records the close, and leaves
publisher behavior unscored rather than marking a pass.
