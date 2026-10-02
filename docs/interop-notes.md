# Publisher Interoperability Notes

This document records what is known about running specific publishers against the
interop runner: how the bundled `moqxr` adapter maps scenarios to the publisher's
command line, what was observed in recent runs, and the standing rule for
reading any such observation. It is a log of facts about publisher builds, not a
statement of what MoQT requires. Observations here can go stale when either side
changes; every entry names the build it was made against.

## Standing rule

No implementation defines expected behavior. Expected behavior comes only from the
checked-in drafts, `docs/draft-ietf-moq-transport-18.txt` and
`docs/draft-ietf-moq-transport-21.txt`, through the requirement catalogs. A
publisher's quirk is never a reason to relax a scenario, bypass the QUIC DATAGRAM
requirement, or change an evaluator. Known publisher limitations appear as
ordinary `fail` or `not_run` rows. A result against one publisher build says
nothing about another. A `pass` row is an observation of specific wire evidence
in one run, not a conformance claim.

## The bundled moqxr adapter

`adapters/moqxr/run.sh` (with `adapters/moqxr/adapter.json`, which is descriptive
and is not read by the runner) drives the `openmoq-publisher` executable from the
sibling `moqxr` project. It needs `bash` and `jq`, reads the executable path from
the `MOQXR_BIN` environment variable, and only accepts the reference fixture:
namespace `media` (hex `6d65646961`) and track `vide_1` (hex `766964655f31`). Any
other namespace, track name, draft, transport or endpoint scheme is refused with
exit status 64 before moqxr starts.

How the request maps to moqxr options (these describe moqxr's command line, not
MoQT expectations):

| Request field or scenario | moqxr option |
|---|---|
| `fixture` | `--input` |
| `endpoint` | `--endpoint` (`moqt://...` for native QUIC, `https://...` for WebTransport) |
| `transport` `native_quic` / `webtransport` | `--transport raw` / `--transport webtransport` |
| `draft` | `--draft 18` or `--draft 21` |
| namespace (fixed) | `--namespace media` |
| `tls_ca` | `--ca` |
| `scenario_timeout_ms` | `--timeout`, rounded up to whole seconds |
| draft 18, all scenarios | `--forward 0` (await the runner's SUBSCRIBE) |
| draft 21, most scenarios | `--forward 1` (moqxr pushes its own PUBLISH) |
| draft 21, scenarios where the runner subscribes (overlapping subscriptions, forward and range-filter conjunction, fill, publish-skipped recovery, withheld acknowledgments, invalid and expired token) | `--forward 0` |
| draft 21 raw-probe scenarios where the runner acts as the subscriber (LARGEST_OBJECT, PUBLISH_DONE, redirects, notifications, padding, discovery, filters, GREASE, GOAWAY alternate URI, and similar) | `--forward 0 --paced` and `--timeout` increased by 3 seconds so the runner, not moqxr, ends the context |
| `publish-track-under-single-period-namespace`, `application-publish-track-in-session-namespace`, `publish-distinct-content-tracks-in-same-scope` | `--publish-catalog` |

`tests/e2e/moqxr-adapter-contract.sh` checks this mapping without starting any
network code, using a capture stub in place of the publisher; run it with
`bash tests/e2e/moqxr-adapter-contract.sh`. It is part of the default CTest suite
as `moqxr-adapter-contract`.

## QUIC DATAGRAM negotiation over native QUIC

Both drafts require QUIC DATAGRAM, and the runner closes a native QUIC session
that did not negotiate it before any MoQT bytes are scored (the close carries the
reason `QUIC DATAGRAM not negotiated`).

- `moqxr` 0.3.26-dev+g478d6c0.dirty (checked 2026-09-29 and 2026-10-01) did not
  negotiate DATAGRAM over raw QUIC for either draft, so native-QUIC runs ended at
  that gate. The same build negotiated it over WebTransport, so contribution runs
  against that build used `webtransport`.
- `moqxr` 0.4.1 (commit `9bda5c9`) negotiates QUIC DATAGRAM over native QUIC.
  Driven runs of the reference scenario reach SETUP and are scored over native
  QUIC for both drafts, as well as over WebTransport.

This is an interop observation about two publisher builds. It is not a validator
pass and not a reason to loosen the requirement.

## Results observed with moqxr 0.4.1

On 2026-10-02, against `openmoq-publisher 0.4.1 (commit 9bda5c9)` and the
`locmaf-publisher.mp4` fixture from the moqxr tests, `bash tests/e2e/moqxr-matrix.sh`
ran four driven runs, each with one reference scenario
(`subscribe-to-publisher-track` for draft 18, `d21-publisher-request-stream-placement`
for draft 21):

| Draft | Transport | Verdict | pass rows | fail rows |
|---|---|---|---:|---:|
| 18 | native QUIC | incomplete | 3 | 0 |
| 18 | WebTransport | incomplete | 5 | 0 |
| 21 | native QUIC | incomplete | 3 | 0 |
| 21 | WebTransport | incomplete | 5 | 0 |

`incomplete` is expected: these runs exercise one scenario each, and every other
applicable row stays `not_run`. The matrix script checks that the harness works
(health, process evidence, retained contract input, full requirement export) and
reports pass and fail counts without requiring the publisher to pass.

Opt-in black-box scripts in `tests/e2e/` take the runner binary, the moqxr
executable and the MP4 fixture as arguments and are not part of the default test
suite, because they need the external publisher:

| Script | Purpose |
|---|---|
| `moqxr-matrix.sh RUNNER MOQXR MP4` | The four driven runs above |
| `draft18-native-moqxr.sh RUNNER MOQXR MP4 [21]` | Observed-mode diagnostic over native QUIC; the optional fourth argument `21` runs the draft-21 PUBLISH-announcement profile with `--preannounce-tracks` |
| `draft18-webtransport-smoke.sh`, `draft21-webtransport-smoke.sh` | Observed-mode WebTransport smoke tests; they require a successful publisher exit, observed SETUP and at least one passing requirement |

`tests/e2e/draft18-native-moqxr.sh` and the WebTransport smoke scripts start a
loopback runner with temporary TLS material and need `openssl`, `curl` and `jq`.

## Limitations recorded for moqxr

These were recorded on 2026-10-01 against `0.3.26-dev+g478d6c0.dirty` and have not
been re-checked against 0.4.1. They describe why certain rows stay `not_run`
with that build; they do not describe MoQT behavior.

- It has no TRACK_STATUS support (a TRACK_STATUS with FIN ended the session with
  PROTOCOL_VIOLATION), no PUBLISH_STATE_NOTIFY, no padding, no authorization
  policy, no `MAX_FILTER_RANGES`, and does not follow a GOAWAY URI.
- It originates PUBLISH only for its catalog track and only on request, which is
  why the adapter adds `--publish-catalog` for the scenarios that observe a
  publisher-originated PUBLISH.
- Its reference fixture contains Groups 0 and 1 rather than a track with
  Group 7, Object 9, so the scenarios that need that Location do not score. See
  [scenario-reference.md](scenario-reference.md).

## Other publishers

A different publisher integrates through the same driver contract with no change
to runner code; see [publisher-harness-guide.md](publisher-harness-guide.md). The
sibling `moq-rs/moq-pub` project was recorded as supporting draft versions only
through 14 with ALPN `moq-00`, so it was not used as a draft-18 or draft-21
acceptance fixture.
