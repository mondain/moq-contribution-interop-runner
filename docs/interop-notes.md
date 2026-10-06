# Publisher Interoperability Notes

This document records what is known about running specific publishers against the
interop runner: how the bundled `moqxr` adapter maps scenarios to the publisher's
command line, what was observed in recent runs, and the standing rule for
reading any such observation. It is a log of facts about publisher builds, not a
statement of what MoQT requires. Observations here can go stale when either side
changes; every entry names the build it was made against.

## Standing rule

No implementation defines expected behavior. Expected behavior comes only from the
checked-in drafts, `docs/draft-ietf-moq-transport-18.txt`,
`docs/draft-ietf-moq-transport-21.txt` and `docs/draft-ietf-moq-transport-22.txt`, through
the requirement catalogs. A
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
| `draft` | `--draft 18`, `--draft 21` or `--draft 22` |
| namespace (fixed) | `--namespace media` |
| `tls_ca` | `--ca` |
| `scenario_timeout_ms` | `--timeout`, rounded up to whole seconds |
| draft 18, all scenarios | `--forward 0` (await the runner's SUBSCRIBE) |
| draft 21, most scenarios | `--forward 1` (moqxr pushes its own PUBLISH) |
| draft 21, scenarios where the runner subscribes (overlapping subscriptions, forward and range-filter conjunction, fill, publish-skipped recovery, withheld acknowledgments, invalid and expired token) | `--forward 0` |
| draft 21 raw-probe scenarios where the runner acts as the subscriber (LARGEST_OBJECT, PUBLISH_DONE, redirects, notifications, padding, discovery, filters, GREASE, GOAWAY alternate URI, and similar) | `--forward 0 --paced` and `--timeout` increased by 3 seconds so the runner, not moqxr, ends the context |
| draft 22, scenarios shared with draft 21 | the `d21-` twin's options with `--draft 22`, except 14 probes that run `--forward 0 --paced` at draft 22 only (listed in [the harness guide](publisher-harness-guide.md)) |
| draft 22, own scenarios and the two unscored probes | `--forward 0 --paced` except `d22-publisher-location-filter-parameter` (`--forward 1`); see the harness guide |
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

## Draft 22 sweep against moqxr 4b615f4

This is one peer at one revision on one date. It says nothing about another moqxr
revision or another publisher, and a `pass` row is an observation of wire evidence in
one run, not a conformance claim.

| Item | Value |
|---|---|
| Peer | `openmoq-publisher` (moqxr) at `4b615f4874d67653035642c80a3d454db759859e` ("Add moqt draft 22", 2026-10-06), `--version` `0.4.2-dev`; built in a scratch copy with picoquic `01e124e` and picotls `f06553b`, Release, target `openmoq-publisher` |
| Fixture | moqxr's `tests/fixtures/locmaf-publisher.mp4` (Groups 0 and 1), namespace `media`, track `vide_1` |
| Runner | this repository at `5ca525e` (runner binary built from `6b28cca`; later commits change only tests), bundled `adapters/moqxr/run.sh` |
| Date | 2026-10-06 (final sweep 13:10 to 13:32 -07:00) |

Method (the same as the draft 18 and 21 sweeps below): every executable scenario is run
as its own driven run on both transports, with the bundled moqxr adapter as the driver
executable and the publisher declared cache-less. Four runners worked in parallel,
each started as

```sh
MOQXR_BIN=<moqxr>/build/openmoq-publisher build/moq-interop-runner --bind 127.0.0.1 \
  --port <P> --database <dir>/runs.sqlite3 --docs docs --requirements requirements \
  --publisher-bind 127.0.0.1 --publisher-advertise 127.0.0.1 \
  --publisher-port-start <P+1> --publisher-port-end <P+4> --tls-cert cert.pem --tls-key key.pem \
  --driver-executable adapters/moqxr/run.sh --driver-fixture locmaf-publisher.mp4 \
  --driver-log-root <dir>/logs --publisher-no-fetch
```

and given one `POST /api/v1/runs` per scenario (`"mode": "driven"`, `"timeout_ms": 12000`,
the track above in hex). The draft 22 id set is `tests/golden/executable-ids-d22.txt`
(221) plus the two unscored probes; the draft 21 baseline uses
`tests/golden/executable-ids-d21.txt` (222). A requirement row that names several
scenarios needs all of them in one run, so every row still `not_run` or `fail` after the
single runs whose scenarios were all runnable got one more run with exactly those
scenarios (row-completion groups). A row counts as `fail` if any run failed it, else
`pass` if any run passed it, else `not_run`. `moq-interop-audit --draft 21|22` was run
on every database.

Runs (refused: HTTP refusal, nothing started; every single-scenario run is `incomplete`,
`fail` or `error` by construction):

| Sweep | ids | refused 400 | refused 422 | runs | incomplete | fail | error | group runs (incomplete / fail / error) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| draft 21 native QUIC (baseline) | 222 | 6 | 28 | 188 | 165 | 15 | 8 | 43 (33 / 6 / 4) |
| draft 21 WebTransport | 222 | 6 | 28 | 188 | 164 | 15 | 9 | 43 (33 / 6 / 4) |
| draft 22 native QUIC | 223 | 6 | 29 | 188 | 175 | 6 | 7 | 41 (31 / 6 / 4) |
| draft 22 WebTransport | 223 | 6 | 29 | 188 | 175 | 6 | 7 | 41 (31 / 6 / 4) |

Requirement rows touched by the sweep:

| Sweep | rows | pass | fail | not_run |
|---|---:|---:|---:|---:|
| draft 21 native QUIC (baseline) | 148 | 57 | 12 | 79 |
| draft 21 WebTransport | 145 | 57 | 12 | 76 |
| draft 22 native QUIC | 147 | 69 | 5 | 73 |
| draft 22 WebTransport | 144 | 69 | 5 | 70 |

- Refusals are the same on every sweep: 6 scenarios need a reserved-namespace fixture
  the adapter does not accept (400 `invalid_run_config`), 22 FETCH scenarios (23 at
  draft 22, with `d22-fetch-bounded-location-range`) are refused for the no-FETCH
  declaration (422 `scenario_requires_publisher_capability`), and 6 transport-specific
  scenarios are refused on the other transport (422 `unsupported_run_config`).
- The unscored probes both recorded `pass` on both transports:
  `d22-location-filter-unknown-type` (REQUEST_ERROR, then a PROTOCOL_VIOLATION close) and
  `d22-location-filter-absolute-origin` (SUBSCRIBE_OK and Objects).
- WebTransport works at drafts 21 and 22 with this build. It scores like native QUIC
  except for the transport-specific rows: the four WebTransport SETUP rows pass on
  WebTransport only; `D22-9-1-1-MUST-307`, `D22-9-1-2-MUST-314` and `-315`, scored only by
  the `d22-native-publisher-*` scenarios, pass on native QUIC and are not touched on
  WebTransport; and `D22-11-MUST-488` (unknown datagram type) passes on native QUIC only
  (its WebTransport run is confounded, see below).
- Audit: `Required executable coverage: 170/170` (draft 22) and `173/173` (draft 21),
  `Static gate: PASS` on every database. The execution audit reports, at both drafts
  alike, one `run_error` and one `stored_score_mismatch` per `error` run and
  `scored_without_binding` for rows scored by the typed announcement scenario
  (`D22-6-3-MUST-NOT-155`, `D22-9-MUST-294`, `D22-9-1-MUST-NOT-300`, and on WebTransport
  the four WebTransport SETUP rows). That is pre-existing audit behavior, not a draft 22
  change.

Comparison. The draft 21 baseline equals the first sweep of the same day row for row (57
pass, 12 fail, 79 not_run on native QUIC); draft 21 is frozen, so its command lines and
probes were not changed. The first draft 22 sweep scored 58 pass, 10 fail, 79 not_run
on native QUIC. Between it and this one the runner changed for draft 22 only: 14 probes
run moqxr `--forward 0 --paced` (see [the harness guide](publisher-harness-guide.md)),
three probes send the run's namespace and track instead of fixed names, and the
SUBSCRIBE_TRACKS probes answer the publisher's PUBLISH. The effect, the same on both
transports: 8 false FAIL rows became passes (`D22-8-7-MUST-269` to `-272`,
`D22-8-9-MUST-279`, `-289`, `D22-9-20-15-MUST-432`, `D22-9-20-8-MUST-421`), 3 `not_run`
rows passed (`D22-6-4-1-MUST-167`, `D22-9-6-MUST-366`, `D22-4-2-MUST-111`), and 3
`not_run` rows became FAILs on moqxr's own behavior (`D22-9-MUST-295`,
`D22-9-2-MUST-339`, `D22-3-6-MUST-083`). No shared scenario passes at draft 21 and fails
at draft 22.

The draft 21 twins of the paced probes still run `--forward 1`. With it moqxr sends its
own PUBLISH, blocks waiting for the answer and closes with code 0 about 2 seconds later,
inside the reaction window, so the runner reads that close as the reaction to the
stimulus. The draft 21 FAILs `D21-8-7-MUST-251` to `-254`, `D21-8-9-MUST-267`, `-277`,
`D21-9-20-16-MUST-447`, `D21-9-20-9-MUST-429`, `D21-9-20-10-MUST-432` and
`D21-9-15-MUST-383` are adapter-flag artifacts, not moqxr defects: their draft 22 twins
pass. Two have no draft 22 twin: the draft 22 row for the overflowing filter that
`D21-9-20-10-MUST-432` covers is the own row `D22-9-20-9-MUST-424`, which passes, and
`D21-9-15-MUST-383` passed when the same draft 21 probe ran with paced flags. The
two remaining draft 21 FAILs, `D21-3-3-2-MUST-065` and `D21-9-1-6-MUST-315`, are moqxr
findings (M-11).

### Triage of the draft 22 non-pass rows

Every `fail` and `not_run` row of the draft 22 native QUIC sweep is in exactly one line
below: 5 fail and 73 not_run rows, 78 in all. Categories: (a) runner defect, (b) moqxr
defect, (c) expectation question, (d) not applicable to this peer. Totals: (a) 0 rows,
(b) 15 rows (5 fail, 10 not_run), (c) 3 rows, (d) 60 rows. Line numbers are in
`docs/draft-ietf-moq-transport-22.txt`. The 7 `error` runs are the update-overlap probes
(T22), `d22-publisher-goaway-alternate-uri` (T9), the two REQUEST_UPDATE overrun probes
(T13) and two SETUP token-registration probes (T12).

| T | Cat | Draft 22 rows (outcome) | Scenarios | Evidence | Draft 22 text | Draft 21 twin | Action |
|---|---|---|---|---|---|---|---|
| T1 | b | `D22-3-3-2-MUST-077`, `D22-9-1-6-MUST-326` (fail) | `d22-range-filter-with-zero-negotiated-limit`, `d22-range-filter-default-zero-limit` (and the row's other scenarios, which stall, see T11) | SUBSCRIBE with a Range Filter while moqxr advertised no MAX_FILTER_RANGES: moqxr answers REQUEST_ERROR code 0x1 (UNAUTHORIZED) "invalid SUBSCRIBE" (`05 0014 01 00 11 ...`) instead of INVALID_FILTER (0x36) | 1501-1505, 4039-4044; codes 7929-7975 | `D21-3-3-2-MUST-065`, `D21-9-1-6-MUST-315` fail | Punch list D22-01 (M-11 open) |
| T2 | b | `D22-9-MUST-295` (fail) | `d22-unknown-control-message` (+ `d22-unknown-request-stream-message`) | The runner writes control message type 0x7e (`7e 00 00`); moqxr keeps the session and answers the liveness SUBSCRIBE with SUBSCRIBE_OK and Objects | 3877-3878 | `D21-9-MUST-284` not_run (its twin runs `--forward 1`) | Punch list D22-02 (M-09 partly open) |
| T3 | b | `D22-9-2-MUST-339` (fail) | `d22-duplicate-request-goaway`, `d22-goaway-on-distinct-request-streams` | After moqxr accepts SUBSCRIBE_NAMESPACE for `media` (`07 0001 00`), two GOAWAYs (`10 0003 00 a7 10`) on that request stream draw no close and the liveness SUBSCRIBE is served: moqxr stops reading the request stream after accepting SUBSCRIBE_NAMESPACE. The distinct-streams negative control was answered (barrier REQUEST_ERROR 0x10) | 4113-4115 | `D21-9-2-MUST-328` not_run (fixed names) | Punch list D22-08 |
| T4 | b | `D22-3-6-MUST-083` (fail) | `d22-subscribe-tracks-overlap`, `d22-discovery-independent-overlap-spaces` | A second SUBSCRIBE_TRACKS for prefix `media` while the first is established is accepted with REQUEST_OK (`07 0001 00`), as is one for the empty prefix | 1716-1719 | `D21-9-18-MUST-393` not_run (no PUBLISH answer at draft 21) | Punch list D22-09 |
| T5 | b | `D22-9-5-1-MUST-357`, `-359`, `-360` (not_run) | `d22-failed-subscription-update-cleanup`, `d22-failed-subscribe-namespace-update-close`, `d22-failed-subscribe-tracks-update-close` | The REQUEST_UPDATE meant to fail (`02 0006 03 01 03 02 02 00`: AUTHORIZATION TOKEN USE_ALIAS 0, never registered) is accepted on a SUBSCRIBE (REQUEST_OK `07 0004 01 09 00 01`) and ignored on SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS (no reply, no FIN). The update never fails, so the cleanup these rows require never comes due; the rows stay unscored | 3708-3709 (unknown alias), 4395-4401 | `D21-9-5-1-MUST-346`, `-348`, `-349` not_run (fixed names at draft 21) | Punch list D22-10 (unscored observation) |
| T6 | b | `D22-11-5-1-MUST-541` (not_run) | `d22-inbound-padding-stream` | A padding stream (type 0x132B3E28) makes moqxr close with PROTOCOL_VIOLATION ("received unknown or malformed unidirectional stream type"); the row's evaluator scores only the liveness follow-up, so the close leaves it unscored | 2702, 6633-6647 | `D21-11-5-1-MUST-566` not_run (same close) | Punch list D22-03 |
| T7 | b | `D22-9-5-MUST-356`, `D22-9-5-1-MUST-363` (not_run) | `d22-single-request-update-response`, `d22-coalesced-successful-update-responses`, `d22-coalesced-failed-update-response` | After an accepted SUBSCRIBE, a valid REQUEST_UPDATE (`02 0004 03 01 20 64`; three coalesced) followed by a TRACK_STATUS with FIN on a new request stream: moqxr closes 0x3 "request stream closed before a complete message" (its `read_request_stream_message`) and sends no REQUEST_OK. Which of the two requests triggers the close is not settled | 4304-4308, 4406-4408 | `D21-9-5-MUST-345`, `D21-9-5-1-MUST-352` not_run (same close) | Punch list D22-05 (medium confidence) |
| T8 | b | `D22-4-2-MUST-110`, `D22-9-20-18-MUST-445`, `D22-9-5-MUST-355` (not_run) | `d22-discover-original-publisher-namespaces`; `d22-discovery-update-invalid-forward` (`d22-forward-value-two`, `-255` close 0x3 correctly); `d22-update-on-track-status`, `d22-responder-update-on-publish-namespace`, `d22-subscriber-update-on-publish` | Silence where the draft requires an answer or a close: no NAMESPACE for `media` after accepting SUBSCRIBE_NAMESPACE with the empty prefix; FORWARD 255 in a REQUEST_UPDATE on SUBSCRIBE_TRACKS ignored; a REQUEST_UPDATE on moqxr's own PUBLISH_NAMESPACE (after REQUEST_OK) ignored. Not evidence: `d22-update-on-track-status` is confounded by the open runner item below (fixed track "x"; moqxr did not answer even the TRACK_STATUS, which it otherwise answers with NOT_SUPPORTED, so it probably never read that stream), and in `d22-subscriber-update-on-publish` (a permitted case) moqxr reset its PUBLISH streams before the update, so nothing was observed. Silence is not proof, so the rows stay unscored | 1929-1931, 5613, 4302 | `D22-4-2-MUST-110` is a draft 22 row (no twin); `D21-9-20-19-MUST-460`, `D21-9-5-MUST-344` not_run | Punch list D22-06 (suspected) |
| T9 | b | `D22-9-2-MUST-340` (not_run) | `d22-publisher-goaway-alternate-uri` (error run) | GOAWAY with a New Session URI on the control stream: moqxr logs "received unknown or unsupported control-stream message" and closes 0x3 instead of migrating | 4129-4130 | `D21-9-2-MUST-329` not_run (same error) | Punch list D22-04 (M-19) |
| T10 | c | `D22-13-MUST-568`, `D22-13-MUST-NOT-569`, `D22-13-MUST-NOT-576` (not_run) | `d22-grease-request-error` (+ `-grease-setup-options`, `-auth-token-type`, `-stop-sending`) | The runner rejects moqxr's PUBLISH with GREASE code 0x9d (`05 0004 80 9d 00 00`); moqxr resets the stream and ends the session with code 0. It ends the session after any refused PUBLISH, so whether it closed because of the unknown code cannot be told; the NO_ERROR close is unscored. The other three GREASE scenarios show no close | 7025-7039 | `D21-13-MUST-593`, `-NOT-594`, `-NOT-601` not_run | Expectation question (punch list D22-C1) |
| T11 | d | `D22-8-6-MUST-267`, `D22-3-3-2-MUST-076`, `D22-9-20-12-MUST-425`, `D22-9-20-13-MUST-427`, `D22-9-20-14-MUST-429` (not_run) | range-filter delta overflow, duplicate range-filter key, priority filter, property-filter odd type | moqxr advertises no MAX_FILTER_RANGES (its SETUP carries only PATH and AUTHORITY); these probes wait for it, so the stimulus is never sent | 3535-3536, 1493-1494, 5421-5466 | twins not_run | None (optional capability) |
| T12 | d | `D22-8-9-MUST-276`, `-277`, `-280`, `-281`, `-282`, `-283`, `-285`, `D22-8-9-MUST-NOT-293`, `D22-9-1-4-MUST-319`, `D22-9-1-4-MUST-NOT-318` (not_run) | token registration and alias probes, `d22-setup-register-*` (2 error runs) | No token cache (no MAX_AUTH_TOKEN_CACHE_SIZE), no operator credential it understands, never registers tokens itself | 3664-3783, 4003-4004 | twins not_run | None (out of scope) |
| T13 | d | `D22-9-1-7-MUST-328`, `D22-9-1-7-MUST-NOT-327` (not_run) | `d22-request-update-overrun`, `-independent-streams` (2 error runs), `-unlimited`; `d22-publisher-update-credit-*` | No MAX_REQUEST_UPDATES advertised; moqxr never sends a REQUEST_UPDATE of its own | 4054-4055, 4068-4069 | twins not_run | None |
| T14 | d | `D22-11-5-MAY-536`, `-537`, `D22-11-5-1-MAY-539`, `-MUST-540`, `D22-11-5-2-MAY-544`, `-MUST-545` (not_run) | `d22-padding-stream-emission`, `d22-padding-datagram-emission` | moqxr sends no padding | 6624-6656 | twins not_run | None |
| T15 | d | `D22-9-10-MUST-380`, `-381`, `-383`, `D22-9-10-MUST-NOT-382` (not_run) | `d22-publish-state-notify-*` | No PUBLISH_STATE_NOTIFY | 4711-4726 | twins not_run | None |
| T16 | d | `D22-9-2-MUST-329` (not_run) | `d22-publisher-client-goaway-control`, `-request` | moqxr (the client) never sends GOAWAY | 4076-4078 | `D21-9-2-MUST-318` not_run | None |
| T17 | d | `D22-9-1-1-MUST-305`, `D22-9-1-1-MUST-NOT-303`, `D22-9-1-2-MUST-312`, `D22-9-1-2-MUST-NOT-310` (not_run on native QUIC only) | rows naming a `d22-webtransport-*` scenario | The WebTransport scenario is refused on native QUIC; all four pass on WebTransport | 3934-3955 | same split | None |
| T18 | d | `D22-3-3-1-MUST-NOT-069` (not_run) | `d22-subscribe-bounded-location-range`, `d22-update-subscription-location-range`, `d22-fetch-bounded-location-range` (refused) | The row also names a FETCH scenario. moqxr gives all concurrent subscriptions Track Alias 1 and no LARGEST_OBJECT (allowed, 1123-1133), so Objects cannot be attributed to one filter; every filter type 0x01 to 0x05 was accepted | 1461 | none (a draft 22 row) | None |
| T19 | d | `D22-10-7-MUST-474`, `-475`, `D22-11-2-1-MUST-502`, `D22-11-3-2-MUST-512`, `D22-2-2-MUST-022`, `D22-2-2-MUST-NOT-020`, `D22-3-7-MUST-088`, `D22-6-4-2-2-MUST-172` (not_run) | property filters, datagram flags, subgroup FIN and restart, mandatory property, publisher FIN | Fixture content: two short Groups, no Object properties, no datagrams, nothing at Group 7 | 5815-5816, 6138-6139, 6303, 819-822, 796-798, 1779, 2755-2756 | twins not_run | None (fixture) |
| T20 | d | `D22-11-3-2-MUST-513`, `-519`, `D22-3-1-2-MUST-047`, `D22-3-4-1-MUST-079`, `-080`, `-081`, `D22-3-6-3-MUST-NOT-086`, `D22-3-1-MUST-035`, `D22-3-1-2-MUST-NOT-049`, `D22-4-2-MUST-108`, `D22-4-2-MUST-NOT-112`, `D22-9-4-1-MUST-351`, `D22-9-20-9-MAY-422`, `D22-9-20-MUST-387`, `D22-9-3-MUST-348` (not_run) | subgroup resets, cancellation, fill, PUBLISH_SKIPPED, accepted and rejected pairs, withdrawal order, redirects, `d22-publisher-location-filter-parameter`, parameter serialization, track properties in replies | The condition never arises with this live, single-track publisher: it does not reset its short Groups, opens no fill stream (it delivers both Groups and PUBLISH_DONE at once), never refuses a valid request, never withdraws a namespace or redirects, sends PUBLISH with no parameters (so no LOCATION_FILTER for the MAY row) and no REQUEST_UPDATE for its PUBLISH | 6306, 6376, 1182-1191, 1096-1097, 1607-1615, 1769-1771, 1924-1927, 1947-1948, 4244-4246, 5275-5277, 5062-5063, 4211-4212 | twins not_run (`D22-9-20-9-MAY-422` is a draft 22 row) | None |
| T21 | d | `D22-3-6-MUST-084`, `D22-4-2-1-MUST-115` (not_run) | `d22-track-discovery-authorization`, `d22-namespace-discovery-authorization` | Need an authorization policy that refuses a credential; moqxr has none | 1721-1722, 1974-1975 | twins not_run | None |
| T22 | d | `D22-9-20-20-MUST-455`, `-456` (not_run) | `d22-namespace-prefix-update-overlap`, `d22-track-prefix-update-overlap`, `d22-discovery-update-independent-overlap-spaces` (error runs) | Need two prefixes accepted; moqxr refuses the second prefix `mediab` (DOES_NOT_EXIST "unsupported namespace prefix") and, on the SUBSCRIBE_TRACKS legs, exits | 5685-5686 | twins not_run | None (single-namespace publisher) |

WebTransport: the same 78 rows less the four of T17 (pass there), plus
`D22-11-MUST-488` (not_run on WebTransport, pass on native QUIC). The WebTransport run of
`d22-unknown-datagram-type` is confounded: the adapter runs it `--forward 1`, and moqxr
sent its PUBLISH, reset it and closed with code 0 ("timed out waiting for stream data"),
the own-PUBLISH timeout pattern, so its `not_run` is not a datagram observation. Over
native QUIC the same flags still drew close 0x3 "invalid MOQT datagram". The finding
rests on moqxr's source instead: the WebTransport client ignores incoming datagrams
(`webtransport_client.cpp` lines 666-669 return 0 for `picohttp_callback_post_datagram`)
while the native client validates them and closes with 0x3 (`picoquic_client.cpp` lines
581-589); lines 5955-5956 require a close; punch list D22-07, M-18 open. moqxr source lines are from the scratch copy of `4b615f4` that the sweep ran; the read-only checkout has since moved, so its line numbers differ in places.
Category (b) on WebTransport is 16 rows, that one by source.

`D22-3-4-1-MUST-079` (`d22-cancel-subscription-with-concurrent-fill-streams`) is
counted under T20 for this sweep: no fill stream appeared, so the cancellation was never
sent. The same scenario ended `error` once in the first sweep (native QUIC) and its
draft 21 twin ended `error` once in this one (WebTransport) with moqxr closing 0x3
"retained SUBSCRIBE stream closed with a truncated REQUEST_UPDATE" after it had answered
the update; see D22-05.

Open runner item (no row outcome depends on it): `d22-unknown-request-stream-message`
and `d22-update-on-track-status` still send a fixed request for namespace () and track
"x" (`03 0005 01 00 01 78 00`, `0d 0005 01 00 01 78 00`) on the draft 22 wire, and the
first still runs moqxr `--forward 1`. Against moqxr the request-stream half of
`D22-9-MUST-295` is therefore never exercised (moqxr refuses the unknown track or times
out on its own PUBLISH); the row's FAIL comes from the control-message scenario. It also
weakens the evidence for T8 / punch list D22-06: the TRACK_STATUS leg of
`D22-9-5-MUST-355` comes from `d22-update-on-track-status`, where moqxr answered nothing,
not even the TRACK_STATUS, so that leg says nothing about update handling. The draft 21
twins have the same fixed names (punch list, runner-side follow-ups). Likewise
`d22-unknown-datagram-type` still runs `--forward 1`, which confounds its WebTransport
run (above); a later adapter change could pace it as the other 14.

## Results observed with moqxr 0993cf7

The release audit pinned moqxr `0993cf7d537b0d7af56f87f7017b3fc14f39b614`
(`0.4.1-dev+g0993cf7`) until the draft 22 work moved the pin to
`4b615f4874d67653035642c80a3d454db759859e`, the revision of the draft 22 sweep. Against
`0993cf7`, three-run repeatability passed for draft 18 and
draft 21 over WebTransport, and the four-way driven matrix passed (draft 18 native 3,
WebTransport 5; draft 21 native 3, WebTransport 5; no failures).

A full 758-run sweep compared with the 0.4.1 sweep: draft 18 rows passing went from 57
to 81 and failing from 30 to 4; draft 21 rows passing went from 41 to 56 and failing from
26 to 12. Verdict counts: fail 106 to 38, error 56 to 24.

Rows that passed on 0.4.1 and no longer produce a passing verdict:

- `D18-10-12-MUST-001` now fails. moqxr answers an unknown FETCH Type with a request
  error instead of closing the session (punch list M-21).
- `D18-10-9-MUST-001`, `D18-11-2-1-MUST-001` and `D21-11-5-1-MUST-566` ended
  `incomplete` with no evidence: the stimulus was not accepted before the context timed
  out. They are not failures and are not scored; they are runner-side coverage to
  re-examine against this moqxr revision.

## Results observed with moqxr 0.4.1

> Historical: the sections below record what moqxr 0.4.1 did on 2026-10-02. Many of
> the deviations were fixed in `0993cf7`; the current status of each is in the table in
> [moqxr-punch-list.md](moqxr-punch-list.md) and the summary in the section above.

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
| `moqxr-matrix.sh RUNNER MOQXR MP4` | The adapter contract, then one driven run per draft (18, 21, 22) and transport: six pairs, each with a single reference scenario (`d22-publisher-request-stream-placement` for draft 22). It was four pairs (drafts 18 and 21) when the table above was recorded. A smoke test, not a sweep |
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
tree): draft 18 went from 34 passing and 49 failing rows to 57 and 30; draft 21 from
29 passing and 39 failing to 41 and 30. Runs ending in a run-level error fell from
225 to 56; the remaining ones are scenarios whose stimulus moqxr cannot serve (it
implements no FETCH, only serves namespace `media`, does not advertise
`MAX_REQUEST_UPDATES` or a token cache size, and exits when its own PUBLISH requests
go unanswered). The FETCH part of that is now handled by the capability declaration
described next, so those runs no longer have to end in an error. Failing rows rose in the second half of the work on purpose: a
liveness follow-up (below) now turns 14 rows that used to stay unscored into proven
failures.

Two rows that passed in the first sweep, `D18-10-MUST-008` (unknown control message
type) and `D18-3-4-MUST-001` (unknown unidirectional stream type), fail now. Their
evaluator accepts any session close, and in the first sweep moqxr closed the session
by itself after its unanswered announcement timed out, which counted as the required
close. With the announcement answered, moqxr stays up, logs that it is skipping the
unhandled control message, and serves a follow-up request, so the failure is real.
Close evaluators no longer take the first close whenever it happens. A close is read
as the publisher's reaction only when it follows the delivered stimulus and arrives
within 1.5 s of the last stimulus write (longer for probes with a liveness follow-up),
and, for rules that accept any close code, only when it is not a NO_ERROR close.
Closes that fail these tests leave the row unscored. This removed passes and fails
that depended on moqxr's own read timeout (about two seconds after it announces its
namespace, closing with code 0), for example `D21-6-4-1-MUST-153` and
`D21-11-MUST-503`. The peer-close and response probe families, where the publisher
opens the request stream, keep their own close handling and are not covered yet.

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

- `D21-9-3-MUST-337` and `D21-9-5-MUST-344`: moqxr accepts Track Properties in a
  REQUEST_OK and a responder-side REQUEST_UPDATE and carries on, which the drafts
  forbid (draft 21 lines 3763-3764 and 3854). The runner cannot prove it, because
  moqxr stays silent and the probe family has no liveness follow-up. An earlier FAIL
  for `D21-9-3-MUST-337` came from moqxr's own timeout close and was removed.
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

## moqxr declares no FETCH

moqxr is a live publisher with no cache. It does not implement FETCH: it answers
every FETCH with REQUEST_ERROR 0x1 ("unsupported request stream") and closes the
session. The drafts allow that (draft 18 Section 4, draft 21 Section 1.5: an
endpoint that is not a relay MAY implement only a subset), so the runner treats it
as a declaration, not a failure. Start the runner for moqxr with
`--publisher-no-fetch`, or send `"publisher_capabilities": {"fetch": false}` with
a run; see [http-api.md](http-api.md#declaring-publisher-capabilities).

Effect, measured against `openmoq-publisher` 0.4.1 with the bundled adapter over
native QUIC:

- Run alone and without the declaration, the 45 FETCH-dependent scenarios (23 in
  draft 18, 22 in draft 21) ended in 13 run-level `error` verdicts (9 in draft 18,
  4 in draft 21), 1 `fail` (draft 18) and 31 `incomplete`. None of them could
  produce a pass, and the error runs scored nothing at all.
- With the declaration, each of those 45 selections is refused up front with 422
  `scenario_requires_publisher_capability`; nothing is started.
- With the declaration, a selection that mixes them with other scenarios skips the
  FETCH ones (a `context_skipped` event each) and runs the rest. A draft 18 run of
  four independent and four FETCH scenarios finished `fail` on the independent
  ones (moqxr findings) instead of `error` at the first FETCH context; a draft 21
  run of three and four finished `incomplete`. Rows whose every scenario needs
  FETCH are `not_applicable` and leave the denominators (27 draft 18 rows and 25
  draft 21 rows; for example the draft 18 required denominator is 1470 instead of
  1730). Rows that mix FETCH and other scenarios are unchanged.
- The NOT_SUPPORTED answer that the drafts ask of a limited endpoint is separate
  work in moqxr (item M-17 of the [punch list](moqxr-punch-list.md)). The runner
  sends no FETCH to a publisher that declared it away, so it does not check that
  answer. A scenario that sends a FETCH to such a publisher and requires
  NOT_SUPPORTED could be added later as an optional row; it does not exist today.

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
