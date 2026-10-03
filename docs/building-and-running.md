# Building and Running the Runner

This document explains how to build `moq-interop-runner` from source, prepare TLS
material, start the service with every supported command-line flag, run it in
Docker or Docker Compose, and keep results across restarts. It covers the
runner process only. For creating runs see [http-api.md](http-api.md); for
connecting a publisher see [publisher-harness-guide.md](publisher-harness-guide.md).

## Build from source

Requirements: Linux, CMake 3.24 or newer, a C++20 compiler (the CI and the
Docker image use `g++`), `git`, `perl`, `pkg-config`, and the development
packages for OpenSSL and SQLite3.

```sh
sudo apt-get install -y cmake g++ git perl pkg-config libssl-dev libsqlite3-dev
cmake -S . -B build
cmake --build build -j4
```

The first configure step downloads pinned revisions of picoquic, picotls,
nlohmann/json, cpp-httplib and GoogleTest with CMake `FetchContent`, so it needs
network access. The pins are recorded in `cmake/Dependencies.cmake` and are
printed by `build/moq-interop-runner --version`.

The build produces two programs in `build/`:

| Program | Purpose |
|---|---|
| `build/moq-interop-runner` | The HTTP service and per-run QUIC/WebTransport listeners |
| `build/moq-interop-audit` | Static completeness gate and execution audit; see [scoring-and-audit.md](scoring-and-audit.md) |

Tests are built by default. Add `-DMOQ_INTEROP_BUILD_TESTS=OFF` to build only the
two programs (the Docker image does this). Running the tests needs `jq`,
`openssl` and `curl` on the host in addition to the build packages:

```sh
ctest --test-dir build -j2 --timeout 600 --output-on-failure
```

`cmake --install build --prefix /opt/moq-interop` installs both programs under
`bin/` and the draft texts and requirement catalogs under
`share/moq-interop/docs` and `share/moq-interop/requirements`. The runner's
default `--docs` and `--requirements` point at the source tree it was built
from, so an installed runner must be given the installed directories explicitly.

## TLS material

The runner needs a PEM certificate and private key for QUIC and WebTransport.
The publisher must trust that certificate. A self-signed certificate is enough
for local work; include a subject alternative name for the address the publisher
will connect to:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
  -subj /CN=localhost -days 7 \
  -addext subjectAltName=DNS:localhost,IP:127.0.0.1
```

Give the publisher `cert.pem` as its trust anchor (a self-signed certificate is
its own CA). In driven mode the runner passes a trust file to the adapter in the
request's `tls_ca` field; it defaults to the `--tls-cert` file and can be
overridden with `--driver-ca`. Without both `--tls-cert` and `--tls-key` the
service still serves stored results and inventories, but `POST /api/v1/runs`
returns HTTP 503 `publisher_listener_unavailable`.

## Starting the runner

```sh
build/moq-interop-runner --bind 127.0.0.1 --port 8080 \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start 4443 --publisher-port-end 4452 \
  --tls-cert cert.pem --tls-key key.pem
```

The service prints `HTTP service listening on ADDRESS:PORT` and runs until it
receives SIGINT or SIGTERM. Invalid options print the usage text and exit with
status 2.

### Command-line flags

Flags below match `build/moq-interop-runner --help`.

| Flag | Default | Meaning |
|---|---|---|
| `--bind ADDRESS` | `127.0.0.1` | HTTP bind address |
| `--port PORT` | `8080` | HTTP port |
| `--database PATH` | `interop-runs.sqlite3` in the working directory | SQLite database |
| `--docs PATH` | `docs/` of the source tree | Directory holding the checked-in draft text files |
| `--requirements PATH` | `requirements/` of the source tree | Requirement catalogs and draft digests |
| `--publisher-bind ADDRESS` | `127.0.0.1` | UDP bind address for the per-run QUIC/WebTransport listeners |
| `--publisher-advertise ADDRESS` | the bound address | Host or address returned to publishers in `publisher_endpoint` and in driver request URIs |
| `--publisher-port-start PORT` | `4443` | First UDP port of the publisher range |
| `--publisher-port-end PORT` | `4452` | Last UDP port of the publisher range |
| `--publisher-origin ORIGIN` | none | Allowed WebTransport `Origin`; repeatable |
| `--require-publisher-origin` | off | Require an `Origin` header on WebTransport CONNECT; needs at least one `--publisher-origin` |
| `--tls-cert PATH`, `--tls-key PATH` | none | PEM certificate and key; supply both or neither |
| `--driver-executable PATH` | none | Absolute path of the publisher adapter; enables driven mode |
| `--driver-arg VALUE` | none | Extra argument passed to the adapter; repeatable |
| `--driver-fixture PATH` | none | Media or object fixture path handed to the adapter |
| `--driver-ca PATH` | the `--tls-cert` file | Trust file handed to the adapter |
| `--driver-log-root PATH` | none | Directory for per-run adapter request files and logs |
| `--unknown-auth-token-alias-compat-code CODE` | unset | REQUEST_ERROR code (decimal or `0x` hex) accepted for an unknown authorization token alias; see [scenario-reference.md](scenario-reference.md) |
| `--denied-authorization-token VALUE` | unset | Token-type-0 credential (1 to 1024 bytes) that the publisher's policy is configured to refuse |
| `--invalid-auth-token TYPE:HEX` | unset | Well-formed but invalid credential for a Token Type the publisher understands |
| `--expired-auth-token TYPE:HEX` | unset | Expired credential, same contract |
| `--publisher-no-fetch` | off | Declare, for every run that does not say otherwise, that the publisher does not implement FETCH. A run's own `publisher_capabilities` wins over it; see below |
| `--version` | | Print build identity and dependency revisions |
| `--help` | | Print usage |

Rules enforced at startup:

- `--tls-cert` and `--tls-key` must be given together.
- `--publisher-port-start` must not exceed `--publisher-port-end`.
- `--driver-executable` and `--driver-log-root` must be given together, and the
  executable path must be absolute. If `--driver-executable` is omitted, the
  environment variables `MOQ_INTEROP_DRIVER_EXECUTABLE`,
  `MOQ_INTEROP_DRIVER_FIXTURE` and `MOQ_INTEROP_DRIVER_LOG_ROOT` supply the
  executable, fixture and log root; Compose uses this.
- The number of simultaneous active runs equals the size of the publisher port
  range. Each run holds one UDP port; a run that offers a replacement session
  holds a second one. When no port is free, `POST /api/v1/runs` returns 503
  `publisher_ports_exhausted`.

`--publisher-no-fetch` is the startup default for the publisher capability
declaration. The declaration of a run is, in order: the `publisher_capabilities`
object in the `POST /api/v1/runs` body when it names `fetch`; otherwise the
startup default (`fetch: false` with the flag, `true` without it). A run can
therefore opt back in with `{"publisher_capabilities": {"fetch": true}}`, or out
without the flag with `{"fetch": false}`. A live publisher with no cache, such as
the bundled moqxr setup, starts the runner once with the flag and every run
skips the scenarios that need FETCH; see
[http-api.md](http-api.md#declaring-publisher-capabilities). The flag is not
stored anywhere except in the effective declaration each run records.

Operator-supplied credentials (`--invalid-auth-token`, `--expired-auth-token`,
`--denied-authorization-token`, `--unknown-auth-token-alias-compat-code`) change
which rows can be scored; see [scenario-reference.md](scenario-reference.md).

The HTTP API has no authentication. Keep `--bind` on loopback or a trusted
network. The adapter executable and its arguments are chosen by the operator at
startup and can never be supplied through the API.

## Persistence and recovery

Results live in the SQLite database named by `--database`. Finalized outcomes
are immutable; a rerun creates a new run.

- On SIGINT or SIGTERM the service stops active runs and finalizes them (a typed
  single-scenario run as `incomplete`, a raw-probe run as `error`).
- If the process is killed before it can finalize (for example SIGKILL or a
  container removal), the next startup marks each interrupted run `error`, adds
  a `runner_recovery` event to it and prints `Recovered N interrupted runs as
  ERROR`. Already finalized results are unchanged.
- Driven-mode adapter logs are files under `--driver-log-root`, not in the
  database. Keep both on persistent storage if results must survive container
  replacement.

## Docker image

The `Dockerfile` builds a pinned Debian image with the runner, the audit
program, the draft texts and catalogs, and the bundled moqxr adapter at
`/usr/local/lib/moq-interop/moqxr/run.sh`. It runs as UID 10001, exposes
HTTP port 8080, and its default command binds the HTTP service to `0.0.0.0`
with the database at `/var/lib/moq-interop/runs.sqlite3`. The runtime image
includes `bash`, `curl` and `jq`, which the bundled adapter and the health check
use.

`scripts/container-build.sh build` builds the image from a clean committed tree
and labels it with the exact Git revision. It refuses a dirty worktree or a
conflicting `MOQ_INTEROP_SOURCE_REVISION`. `scripts/container-build.sh config`
prints the resolved Compose configuration.

The image compiles with two parallel jobs by default. On a larger machine set
`MOQ_INTEROP_BUILD_JOBS` (for example `MOQ_INTEROP_BUILD_JOBS=$(nproc) scripts/container-build.sh build`),
or pass `--build-arg BUILD_JOBS=N` to a plain `docker build`. The job count does not
change the image contents.

## Docker Compose

Prepare a directory with `cert.pem` and `key.pem` readable by UID 10001, build
the image, and start the service. Compose requires the two build variables to be
set even when it only starts an existing image:

```sh
export MOQ_INTEROP_SOURCE_REVISION="$(git rev-parse HEAD)"
export SOURCE_DATE_EPOCH=1790467200
scripts/container-build.sh build
MOQ_INTEROP_TLS_DIR="$PWD/tls" MOQ_INTEROP_PUBLISHER_HOST=192.0.2.10 \
  docker compose up
```

Compose variables:

| Variable | Default | Meaning |
|---|---|---|
| `MOQ_INTEROP_IMAGE` | `moq-contribution-interop-runner:local` | Image to run |
| `MOQ_INTEROP_HTTP_BIND`, `MOQ_INTEROP_HTTP_PORT` | `127.0.0.1`, `8080` | Host address and port for the HTTP API |
| `MOQ_INTEROP_UDP_BIND` | `127.0.0.1` | Host address for the published UDP range |
| `MOQ_INTEROP_UDP_START`, `MOQ_INTEROP_UDP_END` | `4443`, `4452` | UDP range, used both on the host and as the runner's publisher port range |
| `MOQ_INTEROP_PUBLISHER_HOST` | `127.0.0.1` | Value of `--publisher-advertise`; must be reachable from the publisher process (`127.0.0.1` only works inside the same network namespace) |
| `MOQ_INTEROP_TLS_DIR` | `./tls` | Mounted read-only at `/run/moq-interop-tls`; must hold `cert.pem` and `key.pem` |
| `MOQ_INTEROP_PUBLISHER_DIR` | `./publisher` | Mounted read-only at `/opt/publisher`; put your publisher, fixture and custom adapter here |
| `MOQ_INTEROP_DRIVER_EXECUTABLE` | empty | In-container adapter path; leave unset for observed mode only |
| `MOQ_INTEROP_DRIVER_FIXTURE` | `/opt/publisher/fixture.mp4` | In-container fixture path |
| `MOQXR_BIN` | `/opt/publisher/openmoq-publisher` | Publisher path read by the bundled moqxr adapter |

Compose runs the container read-only with a 16 MiB `/tmp`, publishes the UDP
range, stores the database and driver logs in the `validator-results` volume
(`/var/lib/moq-interop`, logs under `driver-logs/<run-id>`), and restarts the
container unless stopped. Compose does not pass `--invalid-auth-token` or the
other credential flags. To use them, supply a Compose override file that sets
`command:`; an override replaces the whole list, so repeat the flags from
`compose.yaml` and append the new ones.

## Running the test suite

See the "Tests" section of the [README](../README.md) for the test commands, and
[scoring-and-audit.md](scoring-and-audit.md) for the sanitizer, fuzz and release
audit scripts.
