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

## Findings from a full scenario sweep against moqxr 0.4.1

On 2026-10-02 every driven scenario was run against `openmoq-publisher 0.4.1
(commit 9bda5c9)` on both transports (about 760 runs). Neither the runner nor
moqxr was presumed correct: each failing row was decoded by hand against the
checked-in draft text and classified as a publisher deviation, a runner defect
(fixed), a draft ambiguity, or a harness artifact. The classifications below
describe this build only.

Two harness artifacts explained most of the first sweep's failures and run-level
errors, and are now fixed:

- moqxr announces its namespace with PUBLISH_NAMESPACE and waits for a reply
  before it reads any other stream. The raw probes now answer a publisher's
  parameter-free PUBLISH_NAMESPACE by default (see
  [scenario-reference.md](scenario-reference.md)), so the publisher stays alive
  until the stimulus arrives. Before this, a timeout close with code 0 was scored
  against the publisher.
- In its default push mode moqxr follows the announcement with its own PUBLISH
  requests, which the runner-as-subscriber probes do not answer. The bundled
  adapter runs moqxr with `--forward 0 --paced` for those probes.

Effect on the sweep (rows with at least one scored result, final run on the merged
tree): draft 18 went from 34 passing and 49 failing rows to 56 and 30; draft 21 from
29 passing and 39 failing to 42 and 31. Runs ending in a run-level error fell from
225 to 60; the remaining ones are scenarios whose stimulus moqxr cannot serve (it
implements no FETCH, only serves namespace `media`, does not advertise
`MAX_REQUEST_UPDATES` or a token cache size, and exits when its own PUBLISH requests
go unanswered). Failing rows rose in the second half of the work on purpose: a
liveness follow-up (below) now turns 14 rows that used to stay unscored into proven
failures.

Two rows that passed in the first sweep, `D18-10-MUST-008` (unknown control message
type) and `D18-3-4-MUST-001` (unknown unidirectional stream type), fail now. Their
evaluator accepts any session close, and in the first sweep moqxr closed the session
by itself after its unanswered announcement timed out, which counted as the required
close. With the announcement answered, moqxr stays up, logs that it is skipping the
unhandled control message, and serves a follow-up request, so the failure is real.
An evaluator that accepts any close can still be satisfied by a publisher's unrelated
close; closes that precede the delivered stimulus should not count, and that
tightening is not done yet.

### Deviations from the drafts confirmed with wire evidence

| Behavior of moqxr 0.4.1 | Draft requirement | Rows |
|---|---|---|
| Closes the session when a SETUP repeats an unknown option | Receivers MUST allow duplicates of unknown Setup Options (draft 18 lines 3541-3548; draft 21 lines 3478-3481) | D18-10-3-MUST-003, D18-14-MUST-001, D18-14-MUST-008, D18-14-MUST-NOT-001, D18-15-4-MUST-001, D21-13-MUST-593, D21-13-MUST-NOT-594 |
| Answers a malformed namespace or track name (zero-length field, 33 fields, over 4096 bytes) with REQUEST_ERROR and closes with code 0 | Close the session with PROTOCOL_VIOLATION (draft 18 lines 997-1022; draft 21 section 8.7) | D18-2-4-1-MUST-002 to -005, D21-8-7-MUST-251 to -254 |
| Uses REQUEST_ERROR code 0x02 for "does not exist" | DOES_NOT_EXIST is 0x10; 0x02 is TIMEOUT (draft 18 lines 6848 and 6860; draft 21 lines 7657 and 7677) | D18-3-2-1-MUST-002, D18-3-2-2-MUST-001, D18-3-2-2-MUST-002, D21-2-4-2-MUST-031, D21-6-5-MUST-170 to -172 |
| SUBSCRIBE_NAMESPACE has no 32-field limit, although SUBSCRIBE_TRACKS does | Reject a prefix with more than 32 fields (draft 18 lines 4787-4788; draft 21 lines 4512-4513) | D18-10-18-MUST-001, D21-9-15-MUST-383 |
| Treats the AUTHORIZATION TOKEN parameter as opaque and accepts it | Reject malformed tokens and cache overflow (draft 18 lines 3160-3161 and 3221; draft 21 lines 3262-3263 and 3319) | D18-10-2-2-MUST-005, D18-10-2-2-MUST-011, D21-8-9-MUST-267, D21-8-9-MUST-277 |
| Accepts a second SUBSCRIBE to the same track | DUPLICATE_SUBSCRIPTION (draft 18 lines 1979-1982) | D18-5-1-MUST-004 |
| Accepts an identical, an empty (ancestor) and a second SUBSCRIBE_NAMESPACE prefix with REQUEST_OK; only a descendant prefix got REQUEST_ERROR 0x02 | REQUEST_ERROR PREFIX_OVERLAP (0x30) for a prefix sharing a common prefix with an established SUBSCRIBE_NAMESPACE (draft 18 lines 4803-4807; draft 21 lines 4528-4532) | D18-10-18-MUST-003, D21-9-15-MUST-385 |
| Never sets the FIRST_OBJECT bit (0x40); its subgroup streams open with type 0x38 | The Original Publisher MUST set FIRST_OBJECT when it opens a new Subgroup (draft 18 lines 901-905 and 5303-5305) | D18-2-2-MUST-001 |
| Takes a REQUEST_ERROR with a 1025-byte Reason Phrase as an ordinary failure, prints it and closes with application code 0 | Close the session with PROTOCOL_VIOLATION when the reason phrase length exceeds 1024 (draft 21 lines 3030-3034) | D21-8-5-MUST-248 |
| A REQUEST_ERROR redirect with a non-empty track name is not rejected | PROTOCOL_VIOLATION (draft 18 lines 3835-3836; draft 21 lines 3797-3798) | D18-10-6-1-MUST-005, D21-9-4-1-MUST-341 |
| As a client it accepts AUTHORITY or PATH in a server SETUP | Close with INVALID_AUTHORITY or INVALID_PATH (draft 21 lines 3490-3510) | D21-9-1-1-MUST-293, D21-9-1-1-MUST-294, D21-9-1-2-MUST-300, D21-9-1-2-MUST-301 |
| Skips nested FILL_PARAMETERS contents and does not validate them | PROTOCOL_VIOLATION for invalid group order, an overflowing filter, or forbidden nested parameters (draft 21 lines 4956-5187) | D21-9-20-9-MUST-429, D21-9-20-10-MUST-432, D21-9-20-16-MUST-447 |
| REQUEST_UPDATE_OK carries no LARGEST_OBJECT | Include LARGEST_OBJECT (draft 21 lines 5243-5246) | D21-9-20-18-MUST-456 |
| Rejects a Range Filter with code 0x1 (UNAUTHORIZED) when MAX_FILTER_RANGES is unadvertised, which means zero | INVALID_FILTER (draft 21 lines 1226 and 3602-3607) | D21-3-3-2-MUST-065, D21-9-1-6-MUST-315 |
| Ignores control-stream GOAWAY frames (logged as unhandled) | Duplicate or oversized GOAWAY, and a 1-byte GOAWAY body, require PROTOCOL_VIOLATION (draft 21 lines 3440 and 3679-3709) | D21-9-2-MUST-327, D21-9-2-MUST-331, D21-9-MUST-285 |
| Accepts Track Properties in a REQUEST_OK and a responder-side REQUEST_UPDATE | PROTOCOL_VIOLATION (draft 21 lines 3763-3764 and 3854) | D21-9-3-MUST-337, D21-9-5-MUST-344 |

Some of these currently score as `not_run` instead of `fail` because moqxr stays
silent where the draft requires a close (for example the GOAWAY rows, the
server-SETUP rows and the token rows). Silence alone is not proof of a violation,
so the runner leaves them unscored; the evidence above comes from probes that
followed the violating input with a valid request and observed that moqxr kept
serving. A liveness check that scores this automatically has not been implemented.

### Rows proven by a liveness follow-up

For probes whose draft rule is an unconditional "MUST close the session", the runner
sends a valid SUBSCRIBE for the configured track 500 ms after the violating input.
If the publisher serves it with a SUBSCRIBE_OK and never closes the session, the row
fails on wire evidence; silence, a refusal or any close leaves the previous outcome
unchanged. Against moqxr 0.4.1 this turned 14 rows (26 scenario and transport pairs) from unscored into FAIL:
`D18-10-4-MUST-002`, `-005`, `-007` (control-stream GOAWAY),
`D18-10-3-1-1-MUST-001`, `-002` and `D18-10-3-1-2-MUST-001`, `-002` (server SETUP
AUTHORITY and PATH), `D18-1-4-3-MUST-003`, `D18-10-MUST-008`, `D18-3-4-MUST-001`,
`D21-9-2-MUST-327`, `D21-9-2-MUST-331`, `D21-9-MUST-285` and `D21-8-3-MUST-233`.
The 500 ms bound is a time bound, not proof of delivery order: QUIC does not order
data across streams, so a publisher that leaves one stream unread for longer while
serving another could be wrongly failed. See [scoring-and-audit.md](scoring-and-audit.md).

### Not adjudicated or not scoreable

- `D18-10-18-MUST-004` and `D18-10-19-MUST-004` (authorization of a discovery request)
  score only when `--denied-authorization-token` names a credential the publisher's
  policy refuses; moqxr has no policy that refuses a token, so they stay unscored
  for it. They previously scored FAIL on the unproven assumption that the publisher
  denied the built-in `interop-denied` value.
- `D21-9-20-19-MUST-460`: the scenario now starts from a SUBSCRIBE_TRACKS with
  FORWARD 0, so the earlier false FAIL is gone. moqxr appears to ignore a
  REQUEST_UPDATE on a SUBSCRIBE_TRACKS stream, so the row stays unscored.
- `D18-10-12-2-MUST-004` is ambiguous: moqxr implements no FETCH and answers every
  FETCH with REQUEST_ERROR 0x1 and a close, and the draft's MUST arguably applies
  only to publishers that implement FETCH.
- `D18-11-3-1-MUST-002` and `-003` (datagram types) pass over native QUIC and
  stay unscored over WebTransport, because moqxr validates publisher datagrams
  only on the native path and so never closes on WebTransport.
- Token rows (`D18-10-2-2-MUST-008` and `-010`, `D21-8-9-MUST-270` and `-273`)
  need an operator-supplied credential of a token type the publisher
  understands, and moqxr 0.4.1 understands none. Its `--auth-profile`,
  `--auth-token-file`, `--auth-token-type` and `--auth-dpop-*` options only make
  moqxr present credentials as a client. For tokens it receives, it checks the
  Token structure (Alias Type and field framing) and then ignores Token Type and
  Value; it keeps no token cache and has no validation configuration. Run with
  `--invalid-auth-token 16:deadbeef --expired-auth-token 16:cafebabe` (16 is the
  type its docs use): `D18-10-2-2-MUST-008` scored FAIL because moqxr answered
  REQUEST_OK to a SUBSCRIBE_NAMESPACE carrying the credential, but that is not
  evidence of a violation, since the attestation that the publisher understands
  the type is false. `D18-10-2-2-MUST-010` stayed `not_run` (the registration was
  accepted, so the credential was not expired for the publisher). In the
  draft 21 rows moqxr never answered the TRACK_STATUS and the session ended
  (`-270` and `-273` stayed `not_run`). Do not supply these credentials for moqxr:
  the four rows are not scoreable against it and are reported as `not_run`.

## Limitations recorded for moqxr

These were recorded on 2026-10-01 against `0.3.26-dev+g478d6c0.dirty`. Parts of
them were re-observed against 0.4.1 (no `MAX_FILTER_RANGES`, GOAWAY on the control
stream not followed, no FETCH support, no PUBLISH_STATE_NOTIFY or padding); the
rest have not been re-checked. They describe why certain rows stay `not_run`
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
