# Publisher Interoperability Notes

This document records what is known about running specific publishers against the
interop runner: how the bundled `moqxr` adapter maps scenarios to the publisher's
command line, what was observed in recent runs (including the draft 22 sweeps against
moqxr and imquic), and the standing rule for
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
| Runner | this repository at `5ca525e` (runner binary built from `6b28cca`; later commits change only tests), bundled `adapters/moqxr/run.sh`; re-swept after the F1 runner fixes at `84dca11` (see below) |
| Date | 2026-10-06 (final sweep 13:10 to 13:32 -07:00; re-sweep after the F1 fixes 18:58 to 19:09) |

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
| draft 22 native QUIC, after the F1 fixes | 223 | 6 | 29 | 188 | 174 | 7 | 7 | 41 (30 / 7 / 4) |
| draft 22 WebTransport, after the F1 fixes | 223 | 6 | 29 | 188 | 174 | 7 | 7 | 41 (30 / 7 / 4) |

Requirement rows touched by the sweep:

| Sweep | rows | pass | fail | not_run |
|---|---:|---:|---:|---:|
| draft 21 native QUIC (baseline) | 148 | 57 | 12 | 79 |
| draft 21 WebTransport | 145 | 57 | 12 | 76 |
| draft 22 native QUIC | 147 | 69 | 5 | 73 |
| draft 22 WebTransport | 144 | 69 | 5 | 70 |
| draft 22 native QUIC, after the F1 fixes | 147 | 69 | 6 | 72 |
| draft 22 WebTransport, after the F1 fixes | 144 | 69 | 6 | 69 |

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

After the F1 fixes. The imquic work (below) changed the runner for draft 22 once more: 29
shared probes that still sent namespace () and track "x" now send the run's namespace and
track on the draft 22 wire (fix R2; the other two F1 fixes touch only the imquic adapter).
The same `4b615f4` build was swept again on both transports with the runner at `84dca11`.
After the F1 fixes of the shared probes the rows moved from 69 pass, 5 fail, 73 not_run to
69 pass, 6 fail, 72 not_run on native QUIC, and from 69 / 5 / 70 to 69 / 6 / 69 on
WebTransport. One row changed, on both transports: `D22-8-9-MUST-281` went from `not_run` to
`fail`. With the run's track, `d22-request-unknown-token-alias` sends a SUBSCRIBE for
`media`/`vide_1` with AUTHORIZATION_TOKEN USE_ALIAS 0, an alias never registered
(`03 0014 01 01 05 media 06 vide_1 01 03 02 02 00`); moqxr answers SUBSCRIBE_OK
(`04 0002 01 00`) and serves Objects instead of rejecting the request with
UNKNOWN_AUTH_TOKEN_ALIAS (lines 3708-3709). Before, the fixed track "x" was refused as an
unknown track, which said nothing about the alias. The run also stores an
`unresolved_error_mapping` event reading `result=NOT_RUN`; that text applies only to a
REQUEST_ERROR answer (draft 22 assigns UNKNOWN_AUTH_TOKEN_ALIAS no REQUEST_ERROR code) and
does not contradict the FAIL. This is the same missing check as punch list D22-10, now seen on
SUBSCRIBE as well as on REQUEST_UPDATE. Every other row, the error runs and the refusals are
as before; `D22-8-9-MUST-279` and `-289`, which already passed with the run's names, still
pass.

### Triage of the draft 22 non-pass rows

Every `fail` and `not_run` row of the draft 22 native QUIC sweep is in exactly one line
below: 5 fail and 73 not_run rows, 78 in all. Categories: (a) runner defect, (b) moqxr
defect, (c) expectation question, (d) not applicable to this peer. Totals: (a) 0 rows,
(b) 15 rows (5 fail, 10 not_run), (c) 3 rows, (d) 60 rows. Line numbers are in
`docs/draft-ietf-moq-transport-22.txt`. No category (a) defect was proven for moqxr. Two
adapter-mode questions were open here (`D22-9-10-MUST-381`, T15, and `D22-3-6-3-MUST-NOT-086`,
T20: their scenarios wait for the publisher's own PUBLISH, which moqxr run `--forward 0` does
not send); the dig against `1883b9f` below ran them with `--forward 1` and neither reached its
stimulus, so both are resolved without an adapter change: 086 stays (d), and 381 moves to (b)
because its other scenario shows moqxr ignoring a subscriber's PUBLISH_STATE_NOTIFY (punch list
D22-11). The 7 `error` runs are the update-overlap probes
(T22), `d22-publisher-goaway-alternate-uri` (T9), the two REQUEST_UPDATE overrun probes
(T13) and two SETUP token-registration probes (T12). After the F1 fixes `D22-8-9-MUST-281`
is a FAIL and moves from T12 to T5: 6 fail and 72 not_run rows, (b) 16 rows (6 fail, 10
not_run), (d) 59 rows; the table below shows it under T5. After the `1883b9f` dig
`D22-9-10-MUST-381` moves from (d) to (b): (b) 17 rows (6 fail, 11 not_run), (c) 3, (d) 58; the
table shows it as T15b, so its category cells add up to these totals.

| T | Cat | Draft 22 rows (outcome) | Scenarios | Evidence | Draft 22 text | Draft 21 twin | Action |
|---|---|---|---|---|---|---|---|
| T1 | b | `D22-3-3-2-MUST-077`, `D22-9-1-6-MUST-326` (fail) | `d22-range-filter-with-zero-negotiated-limit`, `d22-range-filter-default-zero-limit` (and the row's other scenarios, which stall, see T11) | SUBSCRIBE with a Range Filter while moqxr advertised no MAX_FILTER_RANGES: moqxr answers REQUEST_ERROR code 0x1 (UNAUTHORIZED) "invalid SUBSCRIBE" (`05 0014 01 00 11 ...`) instead of INVALID_FILTER (0x36) | 1501-1505, 4039-4044; codes 7929-7975 | `D21-3-3-2-MUST-065`, `D21-9-1-6-MUST-315` fail | Punch list D22-01 (M-11 open) |
| T2 | b | `D22-9-MUST-295` (fail) | `d22-unknown-control-message` (+ `d22-unknown-request-stream-message`) | The runner writes control message type 0x7e (`7e 00 00`); moqxr keeps the session and answers the liveness SUBSCRIBE with SUBSCRIBE_OK and Objects | 3877-3878 | `D21-9-MUST-284` not_run (its twin runs `--forward 1`) | Punch list D22-02 (M-09 partly open) |
| T3 | b | `D22-9-2-MUST-339` (fail) | `d22-duplicate-request-goaway`, `d22-goaway-on-distinct-request-streams` | After moqxr accepts SUBSCRIBE_NAMESPACE for `media` (`07 0001 00`), two GOAWAYs (`10 0003 00 a7 10`) on that request stream draw no close and the liveness SUBSCRIBE is served: moqxr stops reading the request stream after accepting SUBSCRIBE_NAMESPACE. The distinct-streams negative control was answered (barrier REQUEST_ERROR 0x10) | 4113-4115 | `D21-9-2-MUST-328` not_run (fixed names) | Punch list D22-08 |
| T4 | b | `D22-3-6-MUST-083` (fail) | `d22-subscribe-tracks-overlap`, `d22-discovery-independent-overlap-spaces` | A second SUBSCRIBE_TRACKS for prefix `media` while the first is established is accepted with REQUEST_OK (`07 0001 00`), as is one for the empty prefix | 1716-1719 | `D21-9-18-MUST-393` not_run (no PUBLISH answer at draft 21) | Punch list D22-09 |
| T5 | b | `D22-9-5-1-MUST-357`, `-359`, `-360` (not_run); `D22-8-9-MUST-281` (fail after the F1 fixes; not_run and under T12 before) | `d22-failed-subscription-update-cleanup`, `d22-failed-subscribe-namespace-update-close`, `d22-failed-subscribe-tracks-update-close`; `d22-request-unknown-token-alias` | After the F1 fixes a SUBSCRIBE for the run's track with USE_ALIAS 0 (never registered) gets SUBSCRIBE_OK (`04 0002 01 00`) and Objects: row 281 FAILs. The REQUEST_UPDATE meant to fail (`02 0006 03 01 03 02 02 00`: AUTHORIZATION TOKEN USE_ALIAS 0, never registered) is accepted on a SUBSCRIBE (REQUEST_OK `07 0004 01 09 00 01`) and ignored on SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS (no reply, no FIN). The update never fails, so the cleanup these rows require never comes due; the rows stay unscored | 3708-3709 (unknown alias), 4395-4401 | `D21-9-5-1-MUST-346`, `-348`, `-349` not_run (fixed names at draft 21) | Punch list D22-10 |
| T6 | b | `D22-11-5-1-MUST-541` (not_run) | `d22-inbound-padding-stream` | A padding stream (type 0x132B3E28) makes moqxr close with PROTOCOL_VIOLATION ("received unknown or malformed unidirectional stream type"); the row's evaluator scores only the liveness follow-up, so the close leaves it unscored | 2702, 6633-6647 | `D21-11-5-1-MUST-566` not_run (same close) | Punch list D22-03 |
| T7 | b | `D22-9-5-MUST-356`, `D22-9-5-1-MUST-363` (not_run) | `d22-single-request-update-response`, `d22-coalesced-successful-update-responses`, `d22-coalesced-failed-update-response` | After an accepted SUBSCRIBE, a valid REQUEST_UPDATE (`02 0004 03 01 20 64`; three coalesced) followed by a TRACK_STATUS with FIN on a new request stream: moqxr closes 0x3 "request stream closed before a complete message" (its `read_request_stream_message`) and sends no REQUEST_OK. Which of the two requests triggers the close was not settled here; the `1883b9f` dig settles it: the TRACK_STATUS, whose type moqxr's framing does not know (punch list D22-11) | 4304-4308, 4406-4408 | `D21-9-5-MUST-345`, `D21-9-5-1-MUST-352` not_run (same close) | Punch list D22-05 (medium confidence in this sweep; after the `1883b9f` dig: the TRACK_STATUS close confirmed, D22-11) |
| T8 | b | `D22-4-2-MUST-110`, `D22-9-20-18-MUST-445`, `D22-9-5-MUST-355` (not_run) | `d22-discover-original-publisher-namespaces`; `d22-discovery-update-invalid-forward` (`d22-forward-value-two`, `-255` close 0x3 correctly); `d22-update-on-track-status`, `d22-responder-update-on-publish-namespace`, `d22-subscriber-update-on-publish` | Silence where the draft requires an answer or a close: no NAMESPACE for `media` after accepting SUBSCRIBE_NAMESPACE with the empty prefix; FORWARD 255 in a REQUEST_UPDATE on SUBSCRIBE_TRACKS ignored; a REQUEST_UPDATE on moqxr's own PUBLISH_NAMESPACE (after REQUEST_OK) ignored. Not evidence: `d22-update-on-track-status` (in this sweep confounded by the fixed track "x"; after the F1 fixes it names the run's track and moqxr, in its await-subscribe mode, still answers neither the TRACK_STATUS nor the update: silence, see the paragraph below), and in `d22-subscriber-update-on-publish` (a permitted case) moqxr reset its PUBLISH streams before the update, so nothing was observed. Silence is not proof, so the rows stay unscored | 1929-1931, 5613, 4302 | `D22-4-2-MUST-110` is a draft 22 row (no twin); `D21-9-20-19-MUST-460`, `D21-9-5-MUST-344` not_run | Punch list D22-06 (suspected in this sweep; after the `1883b9f` dig: confirmed by source, still unscored) |
| T9 | b | `D22-9-2-MUST-340` (not_run) | `d22-publisher-goaway-alternate-uri` (error run) | GOAWAY with a New Session URI on the control stream: moqxr logs "received unknown or unsupported control-stream message" and closes 0x3 instead of migrating | 4129-4130 | `D21-9-2-MUST-329` not_run (same error) | Punch list D22-04 (M-19) |
| T10 | c | `D22-13-MUST-568`, `D22-13-MUST-NOT-569`, `D22-13-MUST-NOT-576` (not_run) | `d22-grease-request-error` (+ `-grease-setup-options`, `-auth-token-type`, `-stop-sending`) | The runner rejects moqxr's PUBLISH with GREASE code 0x9d (`05 0004 80 9d 00 00`); moqxr resets the stream and ends the session with code 0. It ends the session after any refused PUBLISH, so whether it closed because of the unknown code cannot be told; the NO_ERROR close is unscored. The other three GREASE scenarios show no close | 7025-7039 | `D21-13-MUST-593`, `-NOT-594`, `-NOT-601` not_run | Expectation question (punch list D22-C1) |
| T11 | d | `D22-8-6-MUST-267`, `D22-3-3-2-MUST-076`, `D22-9-20-12-MUST-425`, `D22-9-20-13-MUST-427`, `D22-9-20-14-MUST-429` (not_run) | range-filter delta overflow, duplicate range-filter key, priority filter, property-filter odd type | moqxr advertises no MAX_FILTER_RANGES (its SETUP carries only PATH and AUTHORITY); these probes wait for it, so the stimulus is never sent | 3535-3536, 1493-1494, 5421-5466 | twins not_run | None (optional capability) |
| T12 | d | `D22-8-9-MUST-276`, `-277`, `-280`, `-282`, `-283`, `-285`, `D22-8-9-MUST-NOT-293`, `D22-9-1-4-MUST-319`, `D22-9-1-4-MUST-NOT-318` (not_run) | token registration and alias probes, `d22-setup-register-*` (2 error runs) | No token cache (no MAX_AUTH_TOKEN_CACHE_SIZE), no operator credential it understands, never registers tokens itself | 3664-3783, 4003-4004 | twins not_run | None (out of scope) |
| T13 | d | `D22-9-1-7-MUST-328`, `D22-9-1-7-MUST-NOT-327` (not_run) | `d22-request-update-overrun`, `-independent-streams` (2 error runs), `-unlimited`; `d22-publisher-update-credit-*` | No MAX_REQUEST_UPDATES advertised; moqxr never sends a REQUEST_UPDATE of its own | 4054-4055, 4068-4069 | twins not_run | None |
| T14 | d | `D22-11-5-MAY-536`, `-537`, `D22-11-5-1-MAY-539`, `-MUST-540`, `D22-11-5-2-MAY-544`, `-MUST-545` (not_run) | `d22-padding-stream-emission`, `d22-padding-datagram-emission` | moqxr sends no padding | 6624-6656 | twins not_run | None |
| T15 | d | `D22-9-10-MUST-380`, `-383`, `D22-9-10-MUST-NOT-382` (not_run) | `d22-publish-state-notify-*` | moqxr sends no PUBLISH_STATE_NOTIFY | 4711-4726 | twins not_run | None |
| T15b | b (d before the `1883b9f` dig) | `D22-9-10-MUST-381` (not_run) | `d22-subscriber-sends-publish-state-notify`, `d22-publish-established-subscriber-sends-publish-state-notify` | The other direction: the runner sends PUBLISH_STATE_NOTIFY to the publisher (imquic passes it). The second scenario answers the publisher's own PUBLISH first, which moqxr run `--forward 0` never sends, so this row was an adapter-mode question. The `1883b9f` dig resolved it: with `--forward 1` moqxr publishes `catalog` first and the run ends `error`, and the first scenario shows moqxr ignoring the subscriber's PUBLISH_STATE_NOTIFY (`22 0001 00`) on the SUBSCRIBE stream, so the row is (b), not adapter-hidden | 4711-4712 | twin not_run | Punch list D22-11 |
| T16 | d | `D22-9-2-MUST-329` (not_run) | `d22-publisher-client-goaway-control`, `-request` | moqxr (the client) never sends GOAWAY | 4076-4078 | `D21-9-2-MUST-318` not_run | None |
| T17 | d | `D22-9-1-1-MUST-305`, `D22-9-1-1-MUST-NOT-303`, `D22-9-1-2-MUST-312`, `D22-9-1-2-MUST-NOT-310` (not_run on native QUIC only) | rows naming a `d22-webtransport-*` scenario | The WebTransport scenario is refused on native QUIC; all four pass on WebTransport | 3934-3955 | same split | None |
| T18 | d | `D22-3-3-1-MUST-NOT-069` (not_run) | `d22-subscribe-bounded-location-range`, `d22-update-subscription-location-range`, `d22-fetch-bounded-location-range` (refused) | The row also names a FETCH scenario. moqxr gives all concurrent subscriptions Track Alias 1 and no LARGEST_OBJECT (allowed, 1123-1133), so Objects cannot be attributed to one filter; every filter type 0x01 to 0x05 was accepted | 1461 | none (a draft 22 row) | None |
| T19 | d | `D22-10-7-MUST-474`, `-475`, `D22-11-2-1-MUST-502`, `D22-11-3-2-MUST-512`, `D22-2-2-MUST-022`, `D22-2-2-MUST-NOT-020`, `D22-3-7-MUST-088`, `D22-6-4-2-2-MUST-172` (not_run) | property filters, datagram flags, subgroup FIN and restart, mandatory property, publisher FIN | Fixture content: two short Groups, no Object properties, no datagrams, nothing at Group 7 | 5815-5816, 6138-6139, 6303, 819-822, 796-798, 1779, 2755-2756 | twins not_run | None (fixture) |
| T20 | d | `D22-11-3-2-MUST-513`, `-519`, `D22-3-1-2-MUST-047`, `D22-3-4-1-MUST-079`, `-080`, `-081`, `D22-3-6-3-MUST-NOT-086`, `D22-3-1-MUST-035`, `D22-3-1-2-MUST-NOT-049`, `D22-4-2-MUST-108`, `D22-4-2-MUST-NOT-112`, `D22-9-4-1-MUST-351`, `D22-9-20-9-MAY-422`, `D22-9-20-MUST-387`, `D22-9-3-MUST-348` (not_run) | subgroup resets, cancellation, fill, PUBLISH_SKIPPED, accepted and rejected pairs, withdrawal order, redirects, `d22-publisher-location-filter-parameter`, parameter serialization, track properties in replies | The condition never arises with this live, single-track publisher: it does not reset its short Groups, opens no fill stream (it delivers both Groups and PUBLISH_DONE at once), never refuses a valid request, never withdraws a namespace or redirects, sends PUBLISH with no parameters (so no LOCATION_FILTER for the MAY row) and no REQUEST_UPDATE for its PUBLISH | 6306, 6376, 1182-1191, 1096-1097, 1607-1615, 1769-1771, 1924-1927, 1947-1948, 4244-4246, 5275-5277, 5062-5063, 4211-4212 | twins not_run (`D22-9-20-9-MAY-422` is a draft 22 row) | None (`D22-3-6-3-MUST-NOT-086`: the adapter-mode question is resolved by the `1883b9f` dig: with `--forward 1` moqxr still sends no PUBLISH on the single granted stream, and it has no PUBLISH_SKIPPED, so the row stays (d)) |
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

Open runner item (no row outcome depends on it). In the sweep above
`d22-unknown-request-stream-message` and `d22-update-on-track-status` still sent a fixed
request for namespace () and track "x" (`03 0005 01 00 01 78 00`, `0d 0005 01 00 01 78 00`) on
the draft 22 wire. Fix R2 of the F1 work made them send the run's track
(`03 0010 01 01 05 media 06 vide_1 00 7e 00 00`, `0d 0010 01 01 05 media 06 vide_1 00 ...`);
against moqxr nothing changed. `d22-unknown-request-stream-message` still runs moqxr
`--forward 1`: moqxr sends its own PUBLISH, ends the session with code 0 and exits, so the
request-stream half of `D22-9-MUST-295` is still never exercised against moqxr; the row's FAIL
comes from the control-message scenario. In `d22-update-on-track-status` moqxr, in its
await-subscribe mode, now answers nothing to a TRACK_STATUS for `media`/`vide_1` and the
REQUEST_UPDATE on it: the names are no longer the confound, but silence is not proof, so that
leg of T8 / punch list D22-06 stays unscored (the `1883b9f` dig found the cause, D22-11). The
draft 21 twins keep the fixed names (draft 21 is frozen). Likewise `d22-unknown-datagram-type`
still runs `--forward 1`, which confounds its WebTransport run (above). The dig below ran both
probes paced: each stimulus then reaches moqxr and is ignored, but no verdict changes, so the
adapter keeps `--forward 1` for them.

### Re-sweep and dig against moqxr 1883b9f

| Item | Value |
|---|---|
| Peer | `openmoq-publisher` (moqxr) at `1883b9febe35c4173f3a8e6ccf439cfdf0d913ae` (`main`, `v0.4.3-2-g1883b9f`, 2026-10-06 13:41 -07:00), `--version` `0.4.3-dev`; nine commits after `4b615f4`; built in a scratch copy with the same picoquic and picotls sources, Release, target `openmoq-publisher` |
| Fixture | as above |
| Runner | this repository at `ef47f8b`, bundled `adapters/moqxr/run.sh` unchanged |
| Date | 2026-10-06 |

Besides a version bump in `CMakeLists.txt`, tests, docs and a CI workflow, the only source changes
since `4b615f4` are in `moqt_session.cpp` (a wait for a forward=1 REQUEST_UPDATE in the
`--forward 1` file-publish path, and a multi-traf moof split in live stdin ingest) and the CMAF
segmenter. `moqt_control_messages.cpp`, `picoquic_client.cpp` and
`webtransport_client.cpp` are byte-identical, and `moqt_session.cpp` is identical up to line 5731.

Method: as above (four runners, one driven run per id, `timeout_ms` 12000, `--publisher-no-fetch`),
on native QUIC and WebTransport, then the 41 row-completion groups per transport. Result: every
run's verdict and pass and fail counts, and every row outcome, equal the `4b615f4` re-sweep after
the F1 fixes, on both transports: native QUIC 223 ids (6 refused 400, 29 refused 422; 174
incomplete, 7 fail, 7 error; groups 30 / 7 / 4), rows 69 pass, 6 fail, 72 not_run; WebTransport
the same run counts, rows 69 pass, 6 fail, 69 not_run. No row changed: nothing was fixed upstream
and nothing regressed, so no run against the `4b615f4` build was needed to separate causes.

The dig then read moqxr's code for the 6 FAILs and for every other non-pass (b) row (the (c) rows
of T10, punch list D22-C1, were not re-examined) and
recorded, per item, the source location, a repro, the severity, a fix direction and a confidence
label in the [punch list](moqxr-punch-list.md#status-against-moqxr-1883b9f). What changed in the
reading:

- One root cause behind several items (new punch list D22-11): `next_control_message` frames only
  the message types it lists and reports any other type as incomplete, so an unknown type stalls
  the stream it arrives on instead of closing the session. This, not a silent erase "while
  serving", is why `7e 00 00` on the control stream draws no close (`D22-9-MUST-295`; the earlier
  citation of lines 10462-10533 pointed at the DASH live path, which the file publisher does not
  run). The same list lacks TRACK_STATUS (0x0D): with FIN it produces the "request stream closed
  before a complete message" close of T7 (the TRACK_STATUS, not the REQUEST_UPDATE, ends those
  runs), and without FIN moqxr waits up to its `--timeout`, which is the silence of the T8
  TRACK_STATUS leg. It also lacks PUBLISH_STATE_NOTIFY (0x22): `d22-subscriber-sends-publish-state-notify`
  shows moqxr serving on after a subscriber's `22 0001 00`, so `D22-9-10-MUST-381` is (b), not
  adapter-hidden.
- The padding-stream close (T6) comes from the session's own integer decoder, which knows only
  the 1- to 4-byte vi64 forms; type 0x132B3E28 needs the 5-byte form `f0 13 2b 3e 28`. The earlier
  partial-read hypothesis is withdrawn.
- T1, T3, T4, T5 and the three remaining T8 legs are confirmed in the code: the SUBSCRIBE decoder
  has no Range Filter branch (SUBSCRIBE_TRACKS has one); accepted SUBSCRIBE_NAMESPACE and
  SUBSCRIBE_TRACKS request streams and moqxr's own PUBLISH_NAMESPACE stream are never read again;
  only SUBSCRIBE_NAMESPACE has an overlap check; tokens are checked for structure only; and moqxr
  has no NAMESPACE encoder.
- The intermittent "truncated REQUEST_UPDATE" close is likely a missing empty-buffer check when a
  FIN arrives in its own read after the update was consumed.

Adapter-mode experiments (scratch copy of the adapter; draft 22 only):

| Scenario | Bundled | Tried | Result |
|---|---|---|---|
| `d22-unknown-request-stream-message` | `--forward 1` | `--forward 0 --paced` | `not_run` both ways. Paced, moqxr answers the SUBSCRIBE, serves every Object and PUBLISH_DONE and ignores the trailing `7e 00 00` for 12 s; unscored (no liveness follow-up for this probe). `D22-9-MUST-295` stays `fail` |
| `d22-unknown-datagram-type` | `--forward 1` | `--forward 0 --paced` | Native QUIC `pass` both ways. WebTransport `not_run` both ways; paced, the datagram `f0 13 2b 3e 2a` reaches moqxr, which does nothing for 12 s (consistent with D22-07); silence after a datagram is unscored |
| `d22-publish-established-subscriber-sends-publish-state-notify` | `--forward 0 --paced` | `--forward 1` | `error`: moqxr's first PUBLISH is for `catalog`, the probe waits for the run's track, moqxr gives up after 2 s and closes with code 0. `D22-9-10-MUST-381` `not_run` either way |
| `d22-subscribe-tracks-publish-skipped-then-capacity-recovers` | `--forward 0` | `--forward 1` | `not_run` both ways: with the one bidirectional stream the probe grants, moqxr sends no PUBLISH, and it has no PUBLISH_SKIPPED. `D22-3-6-3-MUST-NOT-086` stays (d) |

No experiment made a row pass or reach a verdict, so `adapters/moqxr/run.sh` is unchanged and the
two adapter-mode questions above are closed as negative results. No runner defect (category (a))
was found.

## Draft 22 sweep against imquic 6836173

This is one peer at one revision on one date. It says nothing about another imquic
revision or another publisher, and a `pass` row is an observation of wire evidence in one
run, not a conformance claim. The peer is imquic's example publisher `moq-pub`, a demo: it
publishes one clock track, serves one subscriber at a time, and either announces and waits
for a SUBSCRIBE or sends its own PUBLISH first. It is not a general MoQ publisher, and many
rows below are out of its reach for that reason, not because the library is wrong.

| Item | Value |
|---|---|
| Peer | `imquic-moq-pub` (`examples/moq-pub.c`) from imquic at `6836173947a5eb0a6edffe3709dab3340ce43871`, branch `fix/core-parser-and-stream-regressions` (clean), reporting `imquic 0.0.2/alpha` |
| Build | a scratch copy of the read-only checkout (no `.git`), `make distclean`, `./configure --enable-moq-examples`, `make -j8`; the LOC demos were skipped (no libav); no network needed |
| Payload | moq-pub's clock: one Object per second, one Group per minute, Group IDs from 0 at start; namespace `media`, track `vide_1` |
| Runner | this repository at `84dca11` (runner and adapter of the final sweep; `2de6385` adds only a test), bundled `adapters/imquic/run.sh` |
| Date | 2026-10-06: first sweep 15:48 to 16:10, final sweep 18:33 to 18:57 (-07:00) |

The adapter (see [the harness guide](publisher-harness-guide.md#the-imquic-adapter-draft-22))
passes `-M 22 -n media -N vide_1 -d 4`, `-q -r HOST -R PORT` for native QUIC or
`-w -H PATH` for WebTransport, and chooses per scenario between publish-first (`-X`: PUBLISH
right after SETUP, every SUBSCRIBE refused) and announce-and-wait (PUBLISH_NAMESPACE, then
one SUBSCRIBE served). The choice is derived from the moqxr adapter (moqxr `--forward 1` gives
`-X`, `--forward 0` gives none) with 17 enumerated exceptions and an explicit entry for each
own draft 22 scenario and probe; 67 of the 223 ids run `-X` (the command-line golden has a
68th `-X` line for its synthetic fallback id `scenario-without-special-options`). It stays alive as a small
supervisor around `timeout --foreground --preserve-status -k 2 -s TERM <timeout+3>` so that
moq-pub's 40 to 160 ms shutdown fits the runner's 100 ms stop grace. Of moq-pub's emission
options it passes only `-D datagram`, for `d22-object-datagram-flags`: no padding (`-P`), no
prior group or object gap (`-f`, `-F`), no Object properties (`-x`).

Method: as for the moqxr sweep above, with `IMQUIC_PUB_BIN` and `adapters/imquic/run.sh` in
place of `MOQXR_BIN` and the moqxr adapter, and `--publisher-no-fetch` (moq-pub registers no
FETCH handler). Every one of the 221 executable draft 22 ids and the two unscored probes ran
as its own driven run on each transport (four runners in parallel, `"timeout_ms": 12000`),
then every row still `fail` or `not_run` whose scenarios were all runnable got one
row-completion group run. `moq-interop-audit --draft 22` ran on every database. The final
sweep also re-ran the moqxr `4b615f4` draft 22 sweep on both transports with the same driver,
the same runner build and the bundled moqxr adapter (`MOQXR_BIN`), so that both peers are
compared at the same runner revision (see the update in the moqxr section above).

Runs (refused: HTTP refusal, nothing started):

| Sweep | ids | refused 400 | refused 422 | runs | incomplete | fail | error | group runs (incomplete / fail / error) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| first, native QUIC, adapter as committed then | 223 | 6 | 29 | 188 | 95 | 10 | 83 | 43 (10 / 1 / 32) |
| first, native QUIC, sweep-only shutdown wrapper | 223 | 6 | 29 | 188 | 168 | 17 | 3 | 43 (39 / 4 / 0) |
| first, WebTransport | 223 | 6 | 29 | 188 | 169 | 0 | 19 | 49 (13 / 0 / 30; 6 refused 422) |
| final, native QUIC | 223 | 6 | 29 | 188 | 163 | 22 | 3 | 43 (37 / 6 / 0) |
| final, native QUIC, A4 scenario re-run after `92d3c58` | 223 | 6 | 29 | 188 | 164 | 21 | 3 | 43 (37 / 6 / 0) |
| final, WebTransport | 223 | 6 | 29 | 188 | 188 | 0 | 0 | 49 (43 / 0 / 0; 6 refused 422) |

Requirement rows touched by the sweep:

| Sweep | rows | pass | fail | not_run |
|---|---:|---:|---:|---:|
| imquic, first, native QUIC (adapter as committed then) | 146 | 25 | 10 | 111 |
| imquic, first, native QUIC (shutdown wrapper; first triage) | 146 | 47 | 16 | 83 |
| imquic, final, native QUIC | 146 | 45 | 20 | 81 |
| imquic, final, native QUIC, A4 scenario re-run after `92d3c58` | 146 | 45 | 19 | 82 |
| imquic, final, WebTransport | 144 | 0 | 0 | 144 |
| moqxr `4b615f4`, final, native QUIC (same runner) | 147 | 69 | 6 | 72 |
| moqxr `4b615f4`, final, WebTransport (same runner) | 144 | 69 | 6 | 69 |

- The 45 / 20 / 81 counts, the triage below as first written and the baseline comparison were
  taken before the subgroup-completion evaluator fix (`92d3c58`, triage line A4). With the
  fixed runner, `d22-subgroup-completion-withheld-acknowledgments` was re-run against the same
  imquic scratch build. That run started mid-minute and got `not_run` for `D22-5-2-MUST-144`
  instead of `fail` (no PUBLISH_DONE, so the publisher was never shown to know that the
  Subgroup was complete). Substituting it into the final native sweep and re-aggregating with
  the sweep's own script gives 45 pass, 19 fail and 82 not_run: the numbers of this sweep, not a
  fixed result, since the other 222 runs were not repeated and this row depends on the phase
  of moq-pub's one-minute Group 0 relative to the run start. Further re-runs started at chosen
  seconds of the minute gave `not_run` when Group 0 ended after 6 or 7 Objects (its End of
  Group marker and FIN fit in the 64-byte credit) and a genuine `fail` when it ended after 8
  or more (the stream stalled, PUBLISH_DONE arrived 9 to 10 s in, and no reset followed):
  imquic parses SUBGROUP_DELIVERY_TIMEOUT but never enforces it (imquic punch list, I-23).
  The same re-run against moqxr `1883b9f` still passes the row (PUBLISH_DONE, then the stream
  reset about 200 ms later).
- Refusals: 6 scenarios need a reserved-namespace fixture (400), 23 FETCH scenarios are
  refused for the no-FETCH declaration and 6 transport-specific scenarios run only on their
  own transport (422). On WebTransport 6 group runs mix typed scenarios and are refused (422).
- The 3 `error` runs on native QUIC: `d22-native-publisher-uri-query` and
  `d22-native-publisher-empty-query`, which the adapter refuses (exit 64: moq-pub's raw QUIC
  client cannot carry a `moqt://` query), and `d22-subscribe-oversized-full-track-name`, where
  moq-pub crashes (I-04). No run of the final sweep ended in a runner SIGKILL: the 282
  adapter stops of the native sweep (single and group runs) and the 188 of the WebTransport
  single runs all ended on SIGTERM within the grace.
- WebTransport: every one of the 188 publisher logs shows "Connection established
  (ALPN=h3)" after "Buffering incoming STREAM data on unknown context", and no WebTransport
  session follows (I-01), so all 144 rows are `not_run`, exactly as in the first sweep; only
  the 19 SIGKILL errors of the first sweep are gone.
- Unscored probes: `d22-location-filter-unknown-type` passed on native QUIC (close 0x3 on
  the undefined LOCATION_FILTER Type); `d22-location-filter-absolute-origin` stayed `not_run`
  (SUBSCRIBE_OK, no Object: the probe's SUBSCRIBE has no FORWARD, I-05). Both are `not_run` on
  WebTransport.
- Audit: `Required executable coverage: 170/170` and `Static gate: PASS` on all 16 imquic
  databases (and the 16 moqxr ones). The execution audit reports one `run_error` per `error`
  run, a `stored_score_mismatch` for the crashed run, and `scored_without_binding` for
  `D22-6-3-MUST-NOT-155`, `D22-9-MUST-294` and `D22-9-1-MUST-NOT-300` (the typed announcement
  scenario), as for moqxr: pre-existing audit behavior.

Baseline comparison with moqxr `4b615f4` (final sweeps, same runner, native QUIC; imquic does
not touch `D22-9-1-2-MUST-315`, which only the refused query scenario scores): 36 rows pass on
both; 19 pass on moqxr and fail on imquic; 13 pass on moqxr and are `not_run` on imquic; 8 are
`not_run` on moqxr and pass on imquic; `D22-9-MUST-295` fails on moqxr and passes on imquic;
4 fail on moqxr and are `not_run` on imquic; `D22-8-9-MUST-281` fails on both; 64 are
`not_run` on both. With the re-run after the evaluator fix, `D22-5-2-MUST-144` moves from
"pass on moqxr, fail on imquic" to "pass on moqxr, `not_run` on imquic" (18 and 14) in this
sweep; a run in another phase of the minute can give a genuine fail instead.

### What the first imquic sweep found in the runner

The first sweep found three runner and adapter defects; they were fixed before the final
sweep, and none is in imquic:

- **R1, adapter shutdown timing.** The runner stops a driver with SIGTERM to its process group
  and SIGKILLs the group 100 ms later, recording that as a driver failure (run `error`). moq-pub
  needs 41 to 157 ms after SIGTERM (median 102 ms) to send PUBLISH_DONE and
  PUBLISH_NAMESPACE_DONE and close; with the adapter's `exec timeout ...`, 80 of 188 native
  single runs and 19 of 188 WebTransport runs ended `error`. The adapter now supervises the
  publisher and exits at once on SIGTERM (commit `9afa0da`); the runner is unchanged.
- **R2, fixed names in shared probes.** 29 draft 21 probes shared by draft 22 sent their
  SUBSCRIBE or TRACK_STATUS for namespace () and track "x". imquic closes a zero-field
  namespace with 0x3 before it reads the rest (I-03), and the evaluators scored that close:
  false FAILs (`D22-8-9-MUST-279`, `-281`, `-289`) and false passes (among them
  `D22-9-20-8-MUST-421` and `D22-9-20-SHOULD-393`). On the draft 22 wire these probes now name
  the run's track (commits `728ce69`, `584f130`); draft 21 is unchanged.
- **R3, adapter mode table.** Three SETUP probes ran `-X` by derivation, so moq-pub refused
  their liveness SUBSCRIBE (REQUEST_ERROR 0x19) and their rows stayed unscored; they now run
  announce-and-wait (commit `3e7c0d5`).

Row changes from the first sweep (wrapper, the first triage's basis) to the final sweep, all
from R2 or R3: `D22-8-3-MUST-249` and `-250` not_run to fail (R3, imquic does not close on
the malformed SETUP); `D22-9-20-1-MUST-396` not_run to fail and `D22-9-20-8-MUST-421` pass to
fail (R2, imquic accepts EXPIRES or FILL_TIMEOUT in a SUBSCRIBE and GROUP_ORDER 0);
`D22-9-20-SHOULD-393` pass to not_run (R2, the duplicate FORWARD is accepted; an advisory row
records no fail). The token rows `D22-8-9-MUST-279`, `-281`, `-289` fail in both sweeps, but
only the final FAILs are about tokens (I-19); likewise `D22-9-20-MUST-388`, `-390`,
`D22-9-20-15-MUST-432`, `D22-9-20-21-MUST-460` and `D22-9-MUST-295` pass in both and are now
backed by closes on the run's track (432 with the caveat of C1 below). Against the first
sweep as committed (no wrapper), 33 rows changed: the 28 that the wrapper alone already
changed (R1: 22 not_run to pass, 6 not_run to fail) and the 5 above.

Prediction scorecard (made before the first sweep, from the adapter review): 60 s Groups
leave the Subgroup FIN rows without a verdict, confirmed; a REQUEST_UPDATE overrun needs SETUP
option 8, refuted (imquic sends MAX_REQUEST_UPDATES=1) but the rows stay unscored (I-06);
`d22-grease-request-error` fails, refuted (it passes); the publisher LOCATION_FILTER row is
inconclusive, confirmed; an absent FORWARD is treated as 0, confirmed (I-05); one subscriber
at a time, confirmed (0x19, I-17); padding rows lack verdicts, confirmed (no `-P`); no
discovery or FETCH handling of its own, confirmed; shutdown timing, confirmed and decisive
(R1); the `-X` derivation wrong somewhere, confirmed for three ids (R3).

### Triage of the imquic draft 22 non-pass rows

Every `fail` and `not_run` row of the final native QUIC sweep is in exactly one line below:
20 fail and 81 not_run rows, 101 in all. Categories: (a) runner or adapter defect, (b) defect of
the peer under test, (c) expectation question, (d) not applicable to this peer. The peer under
test is moq-pub built on imquic, so a (b) line says whether the fault is in the imquic
library (it applies to every imquic application) or in the moq-pub example (a demo limit,
reported but labeled as such). Totals: (a) 1 row (fail), (b) 34 rows (19 fail, 15 not_run),
(c) 3 rows, (d) 63 rows. A review of this triage moved `D22-5-2-MUST-144` from (b) to (a)
(A4): the draft starts the SUBGROUP_DELIVERY_TIMEOUT timer only once all objects of the
subgroup have been published (lines 2301-2307), and moq-pub's one-minute group was still being
published when the window ended. That (a) line was a runner defect and is now fixed
(`92d3c58`). The re-run of its scenario in this sweep gave `not_run`, so this sweep's totals
become 19 fail and 82 not_run, (a) 0 rows, (b) 34, (c) 3 and (d) 64 (A4 counted with the
clock-payload limits). The row's outcome depends on the phase of moq-pub's one-minute group:
in some phases it is a genuine fail, a (b) library finding (A4 line below). The other counts
and outcomes below are those of the sweep before the fix. The second outcome in brackets is
moqxr's in its final sweep. Line numbers are in `docs/draft-ietf-moq-transport-22.txt`;
imquic source lines are from the scratch copy of `6836173` that the sweep ran (the read-only checkout may
move, so its line numbers can differ). The items are detailed in
[the imquic punch list](imquic-punch-list.md).

| T | Cat | Draft 22 rows (imquic outcome; moqxr 4b615f4 outcome) | Scenarios | Evidence | Draft 22 lines | Action |
|---|---|---|---|---|---|---|
| A4 | a, fixed; now d or b by phase | `D22-5-2-MUST-144` (fail; after the fix `not_run` or fail by phase; pass) | `d22-subgroup-completion-withheld-acknowledgments` | SUBSCRIBE with SUBGROUP_DELIVERY_TIMEOUT 200 ms (`06 80c8`), FORWARD 1, Group 0; the runner holds stream credit at 64 bytes. moq-pub's Group 0 is a one-minute clock group: after a date-prefix Object 0 it sent one Object per second (Objects 1 to 8, payloads the clock seconds "35" to "42", a few bytes each) until the 64-byte credit stalled the stream, its log shows Objects 9 to 11 produced after that, and the group was still being published when the 12 s window ended, so the timer of lines 2301-2307, which starts only once all objects of the subgroup have been published, never started and no reset was owed. The evaluator (src/scenarios/draft21_contribution_residual_token.cpp, uncommitted_subgroup_spec) assumes "the fixture's Group 0 is complete and its first Object is larger than 64 bytes" (true for moqxr's fixture) and FAILs any subgroup stream still open at the end of the window: a false FAIL for this peer | 2301-2312, 4613-4615 | Fixed in `92d3c58`: on the draft 22 wire a stream left open is a FAIL only after the publisher's PUBLISH_DONE (sent only once it has closed every stream of the subscription) arrived at least the 200 ms timer plus 1 s before the window ended; otherwise no verdict. Re-runs on imquic depend on the phase of the minute: `not_run` when the group is still being published or its FIN fits in the credit, a genuine fail when the group ends after 8 or more Objects (stream stalled, PUBLISH_DONE at 9 to 10 s, no reset; the library parses SUBGROUP_DELIVERY_TIMEOUT but never enforces it). Still pass on moqxr. Former punch list I-12 withdrawn; the library finding is punch list I-23 |
| I-02 | b (library) | `D22-9-2-MUST-338` (fail; pass), `D22-9-2-MUST-340` (not_run; not_run) | `d22-duplicate-control-goaway`, `d22-publisher-goaway-alternate-uri` | The first control-stream GOAWAY (`10 0003 00 a710`; with a URI `10 0021 1f moqt://...`) closes the session 0x4 INVALID_REQUEST_ID: "imquic_moq_parse_goaway:5031 Invalid Request ID". moq.c:5029 applies the draft 18 Request ID parity check for every version >= 18 although the field is parsed only for version 18 (request_id stays 0) | 4113-4115, 4116-4122 (format, no Request ID), 4129-4130 | Punch list I-02 |
| I-04 | b (library + example) | `D22-8-7-MUST-272` (not_run; pass) | `d22-subscribe-oversized-full-track-name` (error run) | SUBSCRIBE with a 4096-byte namespace field plus track name: "imquic_moq_namespace_str:285 Insufficient buffer to render namespace(s)", then a segmentation fault (publisher exit 139, "the monitored command dumped core"; signal 11 in the first sweep A); no close, run `error`. The NULL rendering is then passed to strcasecmp in the example (moq-pub.c:188-197); the missing 4,096-byte check is the library's | 3576-3580 | Punch list I-04 (crash) |
| I-05 | b (example) | `D22-3-1-2-MUST-047` (not_run; not_run) | `d22-cancel-subscribe-with-open-streams` | SUBSCRIBE without FORWARD (`03 0010 01 01 05 media 06 vide_1 00`) gets SUBSCRIBE_OK and no Objects (moq-pub.c:295 `forward_set && forward`), so no data stream is open when the runner cancels; the row cannot be judged | 5618-5621 (absent = 1) | Punch list I-05 |
| I-06a | b (example) | `D22-9-5-MUST-356` (not_run; not_run), `D22-9-5-1-MUST-357` (not_run; not_run), `D22-6-4-2-2-MUST-172` (not_run; not_run), `D22-9-9-MUST-376` (not_run; pass), `D22-9-5-MUST-355` (not_run; not_run) | `d22-single-request-update-response`, `d22-failed-subscription-update-cleanup`, `d22-established-subscription-publisher-fin`, `d22-publish-done-without-data-streams`, `d22-subscriber-update-on-publish` (with `d22-update-on-track-status`, `d22-responder-update-on-publish-namespace`) | Example limit (moq-pub, not the library): REQUEST_UPDATE is never answered. The library hands each update to the application callback (moq.c:3513-3515; it rejects NOT_SUPPORTED itself only without a callback, 3518) and offers imquic_moq_accept_request_update / imquic_moq_reject_request_update (moq.c:7754, 7806); moq-pub's imquic_demo_request_updated (examples/moq-pub.c:325-340) calls neither and only starts or pauses delivery by FORWARD. So no REQUEST_OK/REQUEST_ERROR follows `02 0004 03 01 20 64`. In `d22-failed-subscription-update-cleanup` the SUBSCRIBE carries no FORWARD, delivery never starts, the update that must fail (USE_ALIAS of an unregistered alias, `02 0006 03 01 03 02 02 00`) logs "Incoming update (3) for request 1" and gets no reply, and the FIN is taken as an unsubscribe. In the scenarios with delivery running, an update without FORWARD logs "Pausing delivery of objects". D22-9-5-MUST-355: the TRACK_STATUS and PUBLISH_NAMESPACE legs correctly close 0x3 (library, moq.c:3440-3442); the permitted leg on the subscription gets no answer, so the row stays unscored | 4304-4308, 4320-4321, 4395-4397, 3708-3709, 2755-2756, 4298-4302 | Punch list I-06aa (example) |
| I-06b | b (library) | `D22-9-1-7-MUST-328` (not_run; not_run) | `d22-request-update-overrun`, `d22-request-update-independent-streams` (with `d22-request-update-unlimited`) | Library: a second outstanding REQUEST_UPDATE against imquic's own MAX_REQUEST_UPDATES=1 closes 0x3 "Invalid use of REQUEST_UPDATE on bidirectional request" (moq.c:3440-3442, after the FIXME at 3439) instead of TOO_MANY_REQUEST_UPDATES | 4068-4069 | Punch list I-06bb (library) |
| I-07 | b (library) | `D22-6-4-1-MUST-167` (fail; pass), `D22-6-3-MUST-156` (fail; pass), `D22-9-MUST-296` (fail; pass), `D22-8-3-MUST-251` (fail; pass), `D22-11-MUST-488` (not_run; pass), `D22-9-20-9-MUST-424` (fail; pass), `D22-8-3-MUST-249` (fail; pass), `D22-8-3-MUST-250` (fail; pass) | `d22-unknown-unidirectional-stream-type`, `d22-invalid-bidirectional-request-stream-opener`, `d22-message-body-length-mismatch`, `d22-request-message-truncated-at-fin`, `d22-setup-known-key-value-malformed-value`, `d22-unknown-datagram-type`, `d22-location-filter-end-group-overflow`, `d22-setup-key-value-type-overflow`, `d22-setup-key-value-declared-length-overflow` | Malformed or unknown input is logged and ignored, no close, and the liveness SUBSCRIBE is answered: unknown uni stream type `00` ("not allowed on media streams"), REQUEST_OK opening a bidi stream `07 0001 00`, GOAWAY with Length 1 `10 0001 00` ("Broken GOAWAY"), SUBSCRIBE truncated at FIN (treated as unsubscribe), SETUP AUTHORIZATION_TOKEN value `04` undecodable, datagram type 0x132B3E2A ("Broken MoQ Message", window timed out), LOCATION_FILTER type 3 with StartGroup 2^62-1 + delta overflow accepted with SUBSCRIBE_OK; SETUP with a Delta Type that overflows 2^64-1 (`af00 000c ffffffffffffffff ff 00 01 00`, "Unsupported parameter '18446744073709551615'") and SETUP with an odd option whose Length exceeds 2^16-1 (`af00 0004 09 c1 00 00`, "Broken SETUP", moq.c:2697) are not closed; the liveness SUBSCRIBE is answered SUBSCRIBE_OK (since R3 the adapter runs both announce-and-wait) | 2707-2708, 2540-2541, 3882, 3407-3408, 5955-5956, 5368-5369, 3377-3379, 3397-3400 | Punch list I-07 |
| I-08 | b (library) | `D22-6-3-2-MUST-164` (fail; pass), `D22-9-1-1-MUST-307` (fail; pass), `D22-9-1-2-MUST-314` (fail; pass) | `d22-native-quic-required-setup-options`, `d22-native-publisher-uri-options` | Native QUIC SETUP carries only MOQT_IMPLEMENTATION and MAX_REQUEST_UPDATES (`af00 0016 07 12 "imquic 0.0.2/alpha" 01 01`), no AUTHORITY or PATH (moq.c:280-281 "TODO For raw quic connections ... PATH") | 3943-3944, 3959-3961, 2648-2649 | Punch list I-08 |
| I-09 | b (library) | `D22-9-1-1-MUST-304` (fail; pass) | `d22-server-sends-authority` | Server SETUP with AUTHORITY is closed 0x8 INVALID_PATH ("AUTHORITY received from a server", moq.c:2716) instead of 0x19 INVALID_AUTHORITY. (PATH from a server correctly gets 0x8) | 3929-3939 (INVALID_AUTHORITY requirement 3935-3939), 7917 | Punch list I-09 |
| I-10 | b (library) | `D22-9-4-1-MUST-352` (fail; pass) | `d22-publish-namespace-redirect-nonempty-track-name` | REQUEST_ERROR REDIRECT with a nonempty Track Name on its PUBLISH_NAMESPACE (`05 0009 34 00 00 00 01 01 6e 01 78`): "Got an error announcing namespace: error 52", session ended with code 0 (NO_ERROR) instead of PROTOCOL_VIOLATION | 4245-4246 | Punch list I-10 |
| I-11 | b (library) | `D22-3-3-2-MUST-077` (not_run; fail), `D22-9-1-6-MUST-326` (not_run; fail) | `d22-range-filter-with-zero-negotiated-limit`, `d22-range-filter-default-zero-limit` (other row scenarios wait for MAX_FILTER_RANGES) | SUBSCRIBE with a Range Filter while imquic advertises no MAX_FILTER_RANGES: "Received 1 range filters, where 0 were allowed", then "imquic_moq_reject_subscribe:7632 Invalid request/state (Error)": moq.c:3406 sets the request state to ERROR before calling reject, so the INVALID_FILTER REQUEST_ERROR is never sent (silence, window timed out) | 1501-1505, 4039-4044 | Punch list I-11 |
| I-13 | b (library) | `D22-6-4-2-1-MUST-169` (not_run; pass) | `d22-duplicate-request-id-across-streams`, `d22-duplicate-request-update-id` | Two SUBSCRIBE_NAMESPACE with Request ID 1 (`50 0003 01 00 00`, `50 0005 01 01 01 6d 00`): both answered REQUEST_ERROR NOT_SUPPORTED, no INVALID_REQUEST_ID close | 2733-2734 | Punch list I-13 |
| I-14 | b (library) | `D22-6-5-MUST-186` (fail; pass) | `d22-session-namespace-unknown-namespace-request` | SUBSCRIBE_NAMESPACE for (".session", "x") (`50 000e 01 02 08 .session 01 78 00`) is answered by the library default REQUEST_ERROR NOT_SUPPORTED "Not handled" (`05 000e 03 00 0b ...`), not DOES_NOT_EXIST | 2811-2812 | Punch list I-14 (library default handler) |
| I-15 | b (library) | `D22-6-3-MAY-159` (not_run; pass) | `d22-request-stream-before-peer-setup` | With only the first SETUP byte (`af`) delivered, imquic already answers the early SUBSCRIBE (SUBSCRIBE_OK before the rest `000000` of the runner SETUP): neither buffered nor reset, so the MAY row is inconclusive | 2549-2553 (SHOULD buffer) | Punch list I-15 (low) |
| I-19 | b (library) | `D22-8-9-MUST-279` (fail; pass), `D22-8-9-MUST-281` (fail; fail), `D22-8-9-MUST-289` (fail; pass) | `d22-request-undecodable-authorization-token`, `d22-request-unknown-token-alias` (+ `-deleted-token-alias`), `d22-request-token-cache-overflow`, `d22-request-alias-registration-with-default-zero-cache` | The AUTHORIZATION_TOKEN parameter of a SUBSCRIBE is stored as opaque bytes and never decoded (moq.c:6486-6500): an undecodable token (`03 01 03`), a USE_ALIAS of an alias never registered (`03 02 02 00`) and a REGISTER while imquic announced no token cache (`03 04 01 00 00 78`, `03 04 01 00 80 9d`) all get SUBSCRIBE_OK `04 0002 00 00`, and the liveness SUBSCRIBE is answered. Since R2 the probes name the run's track; before, the FAILs were the zero-field-namespace close (first sweep A2) | 3704-3705, 3708-3709, 3760-3765 | Punch list I-19 |
| I-20 | b (library) | `D22-9-20-8-MUST-421` (fail; pass) | `d22-group-order-zero` (with `d22-group-order-above-two`, `d22-fill-invalid-group-order`) | SUBSCRIBE with GROUP_ORDER 0 (`22 00`) is accepted with SUBSCRIBE_OK and the liveness SUBSCRIBE is answered: the check is `group_order > IMQUIC_MOQ_ORDERING_DESCENDING` only (moq.c:6557), so 0 passes. GROUP_ORDER 3 is closed 0x3 correctly; the fill leg closes for the C1 reason | 5256-5260 | Punch list I-20 |
| I-21 | b (library) | `D22-9-20-1-MUST-396` (fail; pass) | `d22-parameter-invalid-message-scope`, `d22-fill-timeout-outside-fill-or-fetch` (with `d22-group-order-in-subscription-update`) | A SUBSCRIBE carrying EXPIRES (`08 01`) or FILL_TIMEOUT (`0a 00`), parameters not allowed in SUBSCRIBE, is accepted with SUBSCRIBE_OK; the parameter parser has no per-message scope check (moq.c:6448-6800). GROUP_ORDER in a subscription REQUEST_UPDATE is also accepted silently | 5126-5129 | Punch list I-21 |
| I-22 | b (library) | `D22-9-20-SHOULD-393` (not_run; pass) | `d22-unexpected-duplicate-message-parameter` | SUBSCRIBE with FORWARD twice (`10 00 00 00`, the second as Delta Type 0) is accepted with SUBSCRIBE_OK; no close in the window. The parameter loop never checks for repeats (moq.c:6833-6843). A SHOULD, so the advisory row is `not_run`, not `fail` | 5104-5108 | Punch list I-22 (SHOULD, low) |
| C1 | c | `D22-3-4-1-MUST-079` (not_run; not_run), `D22-3-4-1-MUST-080` (not_run; not_run), `D22-3-4-1-MUST-081` (not_run; not_run) | `d22-cancel-subscription-with-concurrent-fill-streams`, `d22-fill-fails-before-first-object` (+ `d22-fill-location-filter-end-group-overflow`); also every fill probe (`d22-fill-forbidden-*`, `d22-fill-recursive-parameter`, `d22-fill-invalid-group-order`) | SUBSCRIBE with FILL_PARAMETERS (Type 0x23; on the wire it is a Type Delta: `02 10 01 13 02 21 00` = FORWARD 1, then delta 0x13 to 0x23, Length 2, the nested LOCATION_FILTER `21 00`, in `d22-cancel-subscription-with-concurrent-fill-streams`; `23 ...` where FILL_PARAMETERS is the first parameter) (one nested LOCATION_FILTER type 0, no count): imquic expects a Number of Parameters first (moq.c:6761), reads 0x21 as a count and closes 0x3 "Broken MoQ request parameter". The same parse error closes every fill probe: D22-9-20-15-MUST-432 passes and the fill legs of D22-9-20-8-MUST-421 and D22-9-20-9-MUST-424 close 0x3 for this reason, not because imquic judged the nested parameter (confounded passes) | 5478-5482 ("a sequence of Parameters ... encoded as if they were Parameters for a separate message") | Expectation question C1: the runner (and moqxr 4b615f4) read the value as a bare parameter sequence; imquic reads a Number of Parameters first (see the punch list) |
| D-clock | d | `D22-11-3-2-MUST-512` (not_run; not_run), `D22-2-2-MUST-022` (not_run; not_run), `D22-2-2-MUST-NOT-020` (not_run; not_run), `D22-3-7-MUST-088` (not_run; not_run), `D22-10-7-MUST-474` (not_run; not_run), `D22-10-7-MUST-475` (not_run; not_run), `D22-11-3-2-MUST-513` (not_run; not_run), `D22-11-3-2-MUST-519` (not_run; not_run) | subgroup FIN (`d22-complete-subgroup-fin`, `-start-location-fin`), group 7 probes (`d22-original-publisher-opens-new-subgroup`, `d22-subscribe-single-subgroup`, `d22-subgroup-restart-after-reset`, `d22-publish-track-with-mandatory-property`), property filters, premature close / early hand-off resets | Clock payload limit: one Object per second, one Group per minute, Group IDs counted from 0 at start. A 12 s window ends before a Group (and its Subgroup FIN) completes; Group 7 is 7 minutes away ("Starting delivery of objects: [7/0]", no Object); no Object properties (`-x` not passed); moq-pub never resets or prematurely closes a subgroup | 819-822, 796-798, 1779, 5815-5816, 6303, 6306, 6376 | None (F2 payload/emission options: -x, shorter groups) |
| D-pad | d | `D22-11-5-MAY-536` (not_run; not_run), `D22-11-5-MAY-537` (not_run; not_run), `D22-11-5-1-MAY-539` (not_run; not_run), `D22-11-5-1-MUST-540` (not_run; not_run), `D22-11-5-2-MAY-544` (not_run; not_run), `D22-11-5-2-MUST-545` (not_run; not_run) | `d22-padding-stream-emission`, `d22-padding-datagram-emission` | The adapter does not pass `-P` (moq-pub padding); no padding stream or datagram is sent | 6624-6656 | None now (adapter emission option -P later) |
| D-disc | d | `D22-3-6-MUST-083` (not_run; fail), `D22-3-6-MUST-084` (not_run; not_run), `D22-3-6-3-MUST-NOT-086` (not_run; not_run), `D22-4-2-MUST-108` (not_run; not_run), `D22-4-2-MUST-110` (not_run; not_run), `D22-4-2-MUST-111` (not_run; pass), `D22-4-2-MUST-NOT-112` (not_run; not_run), `D22-4-2-1-MUST-115` (not_run; not_run), `D22-9-2-MUST-339` (not_run; fail), `D22-9-4-1-MUST-351` (not_run; not_run), `D22-9-5-1-MUST-359` (not_run; not_run), `D22-9-5-1-MUST-360` (not_run; not_run), `D22-9-10-MUST-380` (not_run; not_run), `D22-9-20-2-MUST-NOT-399` (not_run; pass), `D22-9-20-20-MUST-455` (not_run; not_run), `D22-9-20-20-MUST-456` (not_run; not_run), `D22-9-20-18-MUST-445` (not_run; not_run) | SUBSCRIBE_NAMESPACE / SUBSCRIBE_TRACKS probes (discovery, overlap, authorization, GOAWAY on request streams, redirects, failed updates, PUBLISH_STATE_NOTIFY on namespace requests, prefix updates); `d22-discovery-update-invalid-forward` (row 445, with `d22-forward-value-two`, `-255`) | moq-pub registers no SUBSCRIBE_NAMESPACE / SUBSCRIBE_TRACKS handler: every such request gets the library default REQUEST_ERROR NOT_SUPPORTED "Not handled" + FIN (`05 000e 03 00 0b 4e6f742068616e646c6564`), so no request is established for the stimulus. (FETCH-only scenarios of these rows are refused 422 by the no-FETCH declaration). D22-9-20-18-MUST-445: the SUBSCRIBE legs with FORWARD 2 and 255 now name the run's track and correctly draw close 0x3 (moq.c:6684); the SUBSCRIBE_TRACKS leg is refused NOT_SUPPORTED, so the row stays unscored | 1716-1719, 1944-1945, 1929-1931, 4113-4115, 4244-4246, 4395-4401, 4711-4712, 5608-5613 | None (optional discovery) |
| D-sub1 | d | `D22-3-1-MUST-043` (not_run; pass), `D22-9-20-MUST-NOT-394` (not_run; pass), `D22-9-20-17-MUST-441` (not_run; pass), `D22-3-3-1-MUST-NOT-069` (not_run; not_run) | `d22-overlapping-subscriptions-*`, `d22-subscribe-parameters-preserve-payload`, `d22-largest-object-required-after-publication`, `d22-subscribe-bounded-location-range` (+ refused `d22-fetch-bounded-location-range`) | One subscriber at a time: once delivery started, a further SUBSCRIBE is refused REQUEST_ERROR 0x19 "We already have a subscriber" (`05 001f 19 00 1c ...`, moq-pub.c:216). 069 also names a FETCH scenario (refused 422) | 1130-1132, 5113-5115, 5579, 1461 | None (demo limit); note 0x19 is not a draft 22 REQUEST_ERROR code |
| D-tok | d | `D22-8-9-MUST-276` (not_run; not_run), `D22-8-9-MUST-277` (not_run; not_run), `D22-8-9-MUST-280` (not_run; not_run), `D22-8-9-MUST-282` (not_run; not_run), `D22-8-9-MUST-283` (not_run; not_run), `D22-8-9-MUST-285` (not_run; not_run), `D22-8-9-MUST-NOT-293` (not_run; not_run), `D22-8-6-MUST-267` (not_run; not_run), `D22-3-3-2-MUST-076` (not_run; not_run), `D22-9-20-12-MUST-425` (not_run; not_run), `D22-9-20-13-MUST-427` (not_run; not_run), `D22-9-20-14-MUST-429` (not_run; not_run), `D22-9-5-1-MUST-363` (not_run; not_run), `D22-9-1-4-MUST-NOT-318` (not_run; not_run) | token registration/alias probes, operator-credential probes, range/priority/property filter probes, coalesced updates; `d22-setup-register-default-zero-cache` with `d22-setup-register-exceeds-token-cache` | Gates never open: imquic advertises no MAX_AUTH_TOKEN_CACHE_SIZE and no MAX_FILTER_RANGES (SETUP options 7 and 8 only), no credentials are configured, the publisher never registers or deletes tokens, and MAX_REQUEST_UPDATES=1 is below the 3 the coalesced probes need; the runner sends nothing after SETUP. D22-9-1-4-MUST-NOT-318 is bound to two scenarios that are not alternatives; the sibling `d22-setup-register-exceeds-token-cache` needs a MAX_AUTH_TOKEN_CACHE_SIZE of at least 1 and imquic announces none (a legitimate default of 0) | 3664-3783, 1493-1494, 3535-3536, 5421-5466, 4048-4061, 4003-4004 | None (optional capabilities) |
| D-pub | d | `D22-2-5-MUST-034` (not_run; pass), `D22-3-1-3-MUST-NOT-050` (not_run; pass), `D22-3-1-2-MUST-NOT-049` (not_run; not_run), `D22-3-1-MUST-035` (not_run; not_run), `D22-9-1-7-MUST-NOT-327` (not_run; not_run), `D22-9-10-MUST-383` (not_run; not_run), `D22-9-10-MUST-NOT-382` (not_run; not_run), `D22-9-2-MUST-329` (not_run; not_run), `D22-9-20-9-MAY-422` (not_run; not_run), `D22-9-3-MUST-348` (not_run; not_run) | distinct tracks, rejected-publish pairs, accepted/rejected SUBSCRIBE pair, publisher REQUEST_UPDATE credit, PUBLISH_STATE_NOTIFY, client GOAWAY, LOCATION_FILTER in PUBLISH, track properties in replies | The condition never arises with this single-track demo: one track only; no Object before PUBLISH_OK, and a refused PUBLISH ends the session (code 0) rather than a stream reaction; an accepted live SUBSCRIBE never completes in the window (imquic also treats the runner's FIN as UNSUBSCRIBE, "Getting rid of SUBSCRIBE"); moq-pub sends no REQUEST_UPDATE, no PUBLISH_STATE_NOTIFY, no GOAWAY; its PUBLISH carries FORWARD and GROUP_ORDER only (`1d 0017 ... 02 10 01 12 01`), no LOCATION_FILTER. (348: the PUBLISH_OK and PUBLISH_NAMESPACE_OK legs correctly close 0x3 "Track properties not empty") | 982-983, 1200-1201, 1182-1191, 1096-1097, 4054-4055, 4711-4726, 4076-4078, 5275-5277, 4211-4212 | None |
| D-wt | d | `D22-9-1-1-MUST-305` (not_run; not_run), `D22-9-1-1-MUST-NOT-303` (not_run; not_run), `D22-9-1-2-MUST-312` (not_run; not_run), `D22-9-1-2-MUST-NOT-310` (not_run; not_run) | rows naming a `d22-webtransport-*` scenario | The WebTransport scenario is refused on native QUIC (422 unsupported_run_config); the native halves passed or are in I-09; on WebTransport nothing runs (I-01) | 3934-3955 | None |

WebTransport: every one of the 144 rows touched is `not_run` because no session is ever
established (I-01, category b); the native triage above says what each would need.

Fixed runner item:

- The subgroup-completion evaluator (`d22-subgroup-completion-withheld-acknowledgments`,
  `uncommitted_subgroup_spec` in `src/scenarios/draft21_contribution_residual_token.cpp`)
  failed any subgroup stream still open at the end of the window, although the timer starts
  only once the publisher is aware that all objects of the subgroup have been published (lines
  2301-2307): a false FAIL for a publisher whose group outlasts the window, as for imquic's
  clock (A4). Fixed in `92d3c58`, on the draft 22 wire only (draft 21 is frozen and keeps its
  judgement). The held stream credit hides the stream's FIN, so the evaluator takes the
  publisher's PUBLISH_DONE as the evidence that it knew: a sender "MUST NOT send PUBLISH_DONE
  until it has closed all streams it will ever open" for the subscription (lines 4613-4615),
  and this subscription ends with Group 0. A stream still open at the end of the window is a
  FAIL only after a PUBLISH_DONE that arrived at least the 200 ms timer plus 1 s before the end;
  a reset still passes; anything else has no verdict. A publisher that completes the group but
  sends no PUBLISH_DONE is therefore no longer failed: its awareness is not observable. For
  imquic the outcome now depends on when in the minute the run starts; the fail it gives in
  some phases is a genuine library finding (A4 line, imquic punch list I-23). The other
  evaluators in that file do not depend on a complete Group 0, and no other (b) row of
  this triage rests on group completion.

Open runner items:

- The `unresolved_error_mapping` event of `d22-request-unknown-token-alias` says
  `result=NOT_RUN` (`src/app/native_run_manager.cpp` lines 1305-1308) next to the FAIL of
  `D22-8-9-MUST-281` on both peers. The text applies only to a REQUEST_ERROR answer; both
  peers answered SUBSCRIBE_OK, which the evaluator scores FAIL. The wording could say so.
- C1 decides whether `D22-9-20-15-MUST-432` is a real pass for imquic and whether the fill
  rows can be scored; if the runner's FILL_PARAMETERS encoding is the wrong reading, it is a
  runner defect for every fill probe.
- `D22-9-1-4-MUST-NOT-318` is bound to two scenarios that are not alternatives, so a peer
  without a token cache can never score it (binding design question; draft 21 is frozen).
- The adapter passes none of moq-pub's emission options except `-D datagram` (no `-P`, `-f`,
  `-F`, `-x`), and the
  clock payload cannot reach Group 7 or complete a Group within a run window: the D-clock and
  D-pad rows need a payload or emission change (the F2 motivation).

Caveats: one peer at one revision on one date. The clock payload and the unpassed emission
options limit the verdicts (14 rows in D-clock and D-pad, 15 with A4 after the evaluator
fix), and Group 0 ends whenever the wall-clock minute rolls over, so a row that needs a
complete group can depend on when in the minute its run starts (as A4 does). The WebTransport
column is empty because of I-01, so this sweep says nothing about imquic's WebTransport MoQ
behavior. The publisher is a demo with one subscriber and two fixed modes; rows in D-sub1, D-pub, D-disc and
D-tok would need a different application on the same library. One intermittent moq-pub
startup crash (GLib-CRITICAL `g_source_destroy`, then a core dump) was seen once in about 840
processes of the first sweep and not in the final one.

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

## moq-lite-06 (L1d)

L1d implements the moq-lite-06 session layer and its 19 scenarios (see
[scenario-reference.md](scenario-reference.md)): a session recorder over bidirectional streams, a scripted probe
engine, 30 evaluators bound to catalog rows, and a lite family in the native run manager. The expected behavior is
`docs/draft-lcurley-moq-lite-06.txt` through `requirements/moq-lite-06.json`.

- **Staged results.** The catalog is `complete: false` (75 of 212 rows unreviewed), so a run is scored with the staged
  scorer: Fail when a required reviewed row fails, otherwise Incomplete. It is never Pass. `moq-interop-audit --draft
  moq-lite-06` reports 26 of 26 required reviewed rows covered and exits 0 while saying the catalog is staged.
- **Evidence.** A passing row records exactly the evidence kinds its binding declares; a transcript that is
  harness-failed, hit the event limit or timed out contributes no verdicts.
- **Reachability (L1e).** The HTTP API accepts moq-lite-06 runs (`"draft": "moq-lite-06"`, a string; the MoQ
  Transport drafts stay integers) when the runner loaded the lite catalog, `runnable(MoqLite06)` is true, and
  `adapters/moq-lite` drives the real `moq` CLI as the publisher. In L1d the family ran only through the native run
  manager in tests against a scripted conforming publisher; the first real-publisher sweep is
  [below](#moq-lite-06-first-sweep-against-the-moq-cli-b8b0d235). The session URL is fixed: path `/moq`, query
  `token=l1d` (native `moql://HOST:PORT/moq?token=l1d`, WebTransport `https://HOST:PORT/moq?token=l1d`).
- **Runner duties and the Group-payload rule (L1e Task 1).** The engine FINs the runner's send side of a runner-opened
  bidirectional stream when the publisher ends it (recorded as an engine action; it does not count as a stimulus),
  and closes a WebTransport session with PROTOCOL_VIOLATION when the client's strictly decoded SETUP carries Path
  (draft 7.3.2): row 125 fails, 111 is judged, and every other row of that session, 014 included, is `not_run`.
  That consequence was never observed against the `moq` CLI (it sends no Path on WebTransport). The payload bytes of
  FRAMEs on Group streams are not stored (length and FIN are, plus the counter `group_payload_bytes_dropped`), so
  a media publisher does not exhaust the recorder; the ceilings that remain (16384 events, 64 MiB) are about 15 to
  57 Mbps and far above the 200 kbps test source.
- **Incomplete trailing messages.** An incomplete message still buffered on an announce response stream when the window ends (for example a second ANNOUNCE_OK, which reads as a message waiting for bytes) leaves rows 139, 141 and 152 not run instead of passing.

## moq-lite-06 first sweep against the moq CLI b8b0d235

This is one peer at one revision on one date. It says nothing about another revision of the
CLI or another moq-lite publisher, and a `pass` row is an observation of wire evidence in one
run, not a conformance claim. Expected behavior comes from
`docs/draft-lcurley-moq-lite-06.txt` through `requirements/moq-lite-06.json` (the standing
rule above applies to moq-lite the same way). The catalog is staged, so no run is ever `pass`:
a run is `fail` when a required reviewed row fails and `incomplete` otherwise. The upstream
work list is [moq-lite-punch-list.md](moq-lite-punch-list.md).

| Item | Value |
|---|---|
| Peer | `moq 0.14.1` (`rs/moq-cli`) from moq-dev/moq at `b8b0d235a99bddb9043f453c46c958362d6c9247`; its WebTransport stack is web-transport-moq 2.0.1 / web-transport-proto 0.6.2 |
| Build | a scratch copy of the read-only checkout without `target/`, `.git` and `rust-toolchain.toml`: `CARGO_TARGET_DIR=<scratch>/target cargo +1.98.1 build -p moq-cli --release --offline --locked`; the binary was copied to a stable path and passed as `MOQ_CLI_BIN` |
| Payload | `adapters/moq-lite/run.sh`: ffmpeg `testsrc2` 640x360 at 30 fps, libx264 baseline about 200 kbps, 1 s GOP, fragmented MP4 piped into `moq --log-level debug --connect-version moq-lite-06 --connect-once --connect-timeout 10s --connect-tls-insecure --connect <endpoint> --broadcast interop.hang import fmp4`; one Group per GOP (about 0.95 s), one FRAME per video frame |
| Fixture | broadcast `interop.hang` (namespace `["696e7465726f702e68616e67"]`), track `0.m4s` (`302e6d3473`), as pinned in L1e Task 4 |
| Endpoints | native QUIC `moql://127.0.0.1:PORT/moq?token=l1d`; WebTransport `https://127.0.0.1:PORT/moq?token=l1d` |
| Runner | this repository at `8a26e38` for sweeps 1 to 3 and at `ffecfed` for the final sweep, `tests/e2e/driven-moq-lite.sh` as in `7352293` |
| Date | 2026-10-09, 22:21 to 22:38 -07:00 (2026-10-10 05:21 to 05:38 UTC), loopback, one machine |

Method: as for the moqxr and imquic sweeps above. Every one of the 19 scenarios ran as its own
driven run with `"timeout_ms": 15000` on each transport, the two transports in parallel on
separate ports; then six group runs per transport (the five scenarios of row 027 together;
setup; announce and subscribe; floor and abutting start; the three error-code scenarios; all
19 in one run) to check that contexts do not leak into each other; then the whole single-run
sweep again to look for flakiness; then, after the row 027 fix below, a final sweep of the 19
single runs plus the five-scenario and the 19-scenario group runs. The commands, from the
repository root:

```sh
export MOQ_CLI_BIN=/path/to/moq   # the scratch build above
MOQ_INTEROP_TEST_HTTP_PORT=29435 MOQ_INTEROP_TEST_UDP_PORT=29436 MOQ_INTEROP_TEST_KEEP=1 \
    bash tests/e2e/driven-moq-lite.sh native_quic build/moq-interop-runner
MOQ_INTEROP_TEST_HTTP_PORT=29437 MOQ_INTEROP_TEST_UDP_PORT=29438 MOQ_INTEROP_TEST_KEEP=1 \
    bash tests/e2e/driven-moq-lite.sh webtransport build/moq-interop-runner
# group runs: one comma-joined argument per run, for example
bash tests/e2e/driven-moq-lite.sh native_quic build/moq-interop-runner \
    l06-errors-code-space,l06-setup-duplicate-stream,l06-setup-duplicate-parameter,l06-setup-server-path,l06-setup-server-role
build/moq-interop-audit --draft moq-lite-06 --database /tmp/moq-interop-driven.XXXXXX/runs.sqlite3 --format json
```

Each driven script run ends with `moq-interop-audit --draft moq-lite-06 --database` over its
run database; the static audit (`--draft moq-lite-06` alone) and the server's
`/results/completeness.json` moq-lite-06 entry (a runner started on a copy of each final
database) were read as well. `tests/e2e/moq-lite-matrix.sh` runs the 19 single runs and the
five-scenario row 027 group run for both transports in one command (since `428b151`).

Runs (every run finalized; verdicts of the stored runs):

| Sweep | transport | runs | incomplete | fail | error | scored rows | execution audit |
|---|---|---:|---:|---:|---:|---:|---|
| 1, single runs | native QUIC | 19 | 17 | 2 | 0 | 27 | consistent, 0 findings |
| 1, single runs | WebTransport | 19 | 18 | 1 | 0 | 25 | consistent, 0 findings |
| 2, group runs | native QUIC | 6 | 4 | 2 | 0 | 54 | consistent, 0 findings |
| 2, group runs | WebTransport | 6 | 4 | 2 | 0 | 50 | consistent, 0 findings |
| 3, single runs repeated | native QUIC | 19 | 17 | 2 | 0 | 27 | consistent, 0 findings |
| 3, single runs repeated | WebTransport | 19 | 18 | 1 | 0 | 25 | consistent, 0 findings |
| final, 19 single + 2 group runs | native QUIC | 21 | 17 | 4 | 0 | 61 | consistent, 0 findings |
| final, 19 single + 2 group runs | WebTransport | 21 | 18 | 3 | 0 | 56 | consistent, 0 findings |

The `fail` runs are the runs holding `l06-setup-server-path` (native QUIC) or
`l06-setup-server-role`, whose MUST rows fail; row 107 is a SHOULD and leaves its run
`incomplete`. The static audit reports 26 of 26 required and 4 of 4 optional reviewed
Applicable and Testable rows covered by bindings, the source-keyword audit complete, one
non-blocking finding (75 unreviewed rows, 40 of them required) and `STAGED: incomplete
catalog (not a pass)`, status 0; every `--database` audit has status 0.

Bound rows (the 30 rows the 19 scenarios judge; best state over the runs of the final sweep's
database: `fail` if any run failed it, else `pass` if any run passed it):

| Transport | pass | fail | not_run | fail rows | not_run rows |
|---|---:|---:|---:|---|---|
| native QUIC | 25 | 3 | 2 | 107, 126, 131 | 125, 152 |
| WebTransport | 24 | 2 | 4 | 107, 131 | 120, 124, 126, 152 |

Required coverage: of the 26 required (MUST / MUST NOT) bound rows, 22 pass, 2 fail (126, 131)
and 2 are `not_run` (125, 152) on native QUIC; on WebTransport 22 pass, 1 fails (131) and 3 are
`not_run` (120, 126, 152). Of the 4 SHOULD rows, 3 pass (124, 143, 190) and 1 fails (107) on
native QUIC; 2 pass (143, 190), 1 fails (107) and 1 is `not_run` (124) on WebTransport. Before
the row 027 fix, sweeps 1 to 3 gave 24 / 3 / 3 (native QUIC) and 23 / 2 / 5 (WebTransport),
with 027 the extra `not_run` row. The 182 rows outside the bindings are `not_run` (unreviewed,
or reviewed but judged by no scenario), `not_testable` or `not_applicable` in every run, by the
catalog.

Per scenario (final sweep, single runs; the same in sweeps 1 and 3 and in every group run,
except 027):

| Scenario | native QUIC | WebTransport |
|---|---|---|
| `l06-setup-stream` | 014 pass, 111 pass | 014 pass, 111 pass |
| `l06-setup-unknown-parameter` | 110 pass | 110 pass |
| `l06-setup-duplicate-parameter` | 112 pass (close 0x3) | 112 pass (close 0x3) |
| `l06-setup-duplicate-stream` | 092 pass (close 0x3) | 092 pass (close 0x3) |
| `l06-setup-server-path` | 126 fail (ML-01) | 126 not_run (not judged on WebTransport) |
| `l06-setup-server-role` | 131 fail (ML-02) | 131 fail (ML-02) |
| `l06-setup-client-path` | 120 pass, 124 pass, 125 not_run (WebTransport row) | 125 pass, 120 and 124 not_run (native rows) |
| `l06-announce-prefix` | 139, 141, 143 pass | 139, 141, 143 pass |
| `l06-announce-lifecycle` | 152 not_run | 152 not_run |
| `l06-session-stream-close` | 025 pass | 025 pass |
| `l06-subscribe-latest` | 093, 097, 190 pass | 093, 097, 190 pass |
| `l06-subscribe-refused` | 062 pass | 062 pass |
| `l06-subscribe-invalid-frame-bounds` | 023 pass | 023 pass |
| `l06-subscribe-group-floor` | 159, 172 pass | 159, 172 pass |
| `l06-subscribe-abutting-frame-start` | 020 pass | 020 pass |
| `l06-errors-unknown-stream-type` | 108, 109 pass | 108, 109 pass |
| `l06-errors-unknown-reset-code` | 030, 032 pass | 030, 032 pass |
| `l06-errors-reserved-reset-code` | 033 pass | 033 pass |
| `l06-errors-code-space` | 107 fail (ML-03) | 107 fail (ML-03) |
| row 027 (five-scenario group run) | pass (not_run before `ffecfed`) | pass (not_run before `ffecfed`) |

Run ids of the final sweep: native QUIC `run-18dd144b8126aacd` to `run-18dd145b01c12772`
(single runs), `run-18dd145bd0fefc86` (027 group), `run-18dd145e44ec7d11` (19 scenarios);
WebTransport `run-18dd144b90600c45` to `run-18dd145b01c15bb5`, `run-18dd145bd0fefc7c`,
`run-18dd145e44ec7d18`.

### Triage of the moq-lite-06 non-pass rows

- **(a) Runner defects, fixed (each with a failing test first, moq-lite only):**
  - `f526394`: on WebTransport the reset-code scenarios ended `error` ("the transport rejected
    step 'cancel-reset'") in the L1e Task 4 matrix: `WebTransportSession::reset` always used
    `picowt_reset_stream`, which sends RESET_STREAM_AT and is refused unless both ends enabled
    RESET_STREAM_AT; the CLI does not. The session now falls back to a plain RESET_STREAM on
    such a connection (MoQ Transport clients must enable RESET_STREAM_AT to be admitted, so
    their path is unchanged). Rows 030, 032 and 033 pass on WebTransport since.
  - `ffecfed`: row 027 could never settle against this CLI. Its catalog rationale takes the
    session half from the closes of the four MUST-level setup probes "so it is not tied to the
    SHOULD-level reaction of L06-7-1-SHOULD-107; any one close code from those scenarios
    suffices", but the aggregation needed a close in every one of its five contexts, the
    code-space context included. In a run holding all five scenarios the row now passes on the
    CLI's application close 0x3 on 092 and 112 and its UNROUTABLE (0x36) refusal of the unserved
    SUBSCRIBE (draft lines 579-582). Only 0x36 discriminates the space (it is in the stream table
    alone, Table 3); 0x3 is PROTOCOL_VIOLATION in the session table (Table 2) and SESSION_CLOSED in
    the stream table, and the CANCELLED (0x1) stream resets are INTERNAL_ERROR in the session table,
    so the session half of this Pass rests on codes registered in both tables, which the catalog
    rationale accepts as unable to show the space. Before the fix the row was `not_run` in every
    run.
  - `428b151` (from the review): the completeness entry (`/results/completeness.json`) counted a
    scored row as observed only when every evidence kind its binding declares was present, for a
    Fail as for a Pass, so a Fail by the absence of a close (107, 126, 131: the close probes
    declare `peer_close`) was listed under `not_run` there while the run and the execution audit
    said `fail`. For the staged moq-lite-06 catalog a Fail now counts as observed by
    `audit_execution`'s own rule (a binding of the row whose scenario the run selected); a Pass
    still needs its declared evidence; the MoQ Transport entries are unchanged. On a copy of the
    final native database the entry went from 25 observed (not_run 107, 125, 126, 131, 152) to 28
    (not_run 125, 152); on WebTransport from 24 to 26 (not_run 120, 124, 126, 152).
  - Also in this task, not counted as defects: `918a1e2` (the lite SETTINGS sniff stops
    tracking streams at its 1 KiB bound), `8a26e38` (`moq-interop-audit --draft moq-lite-06
    --database`), `7352293` (the driven script waits for group runs).
- **(b) Peer defects (the punch list):** ML-01 (126, a server's SETUP Path accepted), ML-02
  (131, a server's SETUP Role accepted), ML-03 (107, SHOULD: a Message Length mismatch resets
  the Announce stream with CANCELLED instead of closing the session). 3 rows on native QUIC, 2
  on WebTransport.
- **(c) Expectation questions:** ML-Q1 (which stream code answers a protocol violation on one
  stream; no row affected). The runner reporting question first recorded here (the
  completeness entry listing Fail-by-absence rows as `not_run`) was fixed in `428b151`, above.
- **(d) Not applicable to this peer or transport:** 125 on native QUIC and 120, 124 on
  WebTransport (each is judged on the other binding only); 126 on WebTransport (not judged
  there by design: a Path there is also a URI-binding violation); 152 on both (it needs the
  adapter to end and restart the broadcast within one session, which the driver contract does
  not offer; the CLI keeps its one broadcast for the whole session, so no ANNOUNCE_END is seen).

Other checks of the sweep:

- No context ended with a harness error, a skip, a timeout or the event limit; every publisher
  process ended `stopped` with exit 0 (the adapter's SIGTERM at context end; ffmpeg's log then
  says "Immediate exit requested", which is that stop). The longest context took 6 s of probe
  time, far inside the 18 s source (15 s timeout rounded up plus 3 s).
- No NOT_FOUND (0x33) reset on `0.m4s`: the only 0x33 resets are case (b) of
  `l06-subscribe-refused` (an unknown track of the announced broadcast; no on-demand track
  creation), so the announce-before-moov race fixed in Task 4 did not return. Case (a), an
  uncovered broadcast, is reset with UNROUTABLE (0x36).
- WebTransport Path consequence (a WebTransport client whose SETUP carries Path makes the
  runner close, leaving every row of the session `not_run` except 111 and 125): not observed;
  the CLI sends no Path on WebTransport (row 125 passes), and no context closed for a Path.
- The CLI's SETUP: native QUIC `1:01,2:2f6d6f713f746f6b656e3d6c3164,3:01,5:<random hop>`
  (Probe 1, Path `/moq?token=l1d`, Role Publisher, Hop), WebTransport the same without Path.
  Its Hop ID changes per process, which rows 141 and 143 tolerate (both pass). It opens no
  Probe stream: the runner's SETUP has no Probe parameter, so the CLI logs "peer does not
  support probing; skipping probe stream", and the Probe behavior under Role=Publisher is not
  exercised.
- After the runner's STOP_SENDING with 0x4d1 or 0x2a on a Group stream the stream is reset
  with the same code: the QUIC stack copying the code (RFC 9000 section 3.5), not a moq-lite
  meaning given to it (030, 032, 033 pass).
- Repeats: sweep 3 reproduced sweep 1 row for row on both transports, and the group runs gave
  the single-run states for every row (027 apart, which only a group run can settle), so no
  result was flaky and contexts did not interfere.

Reconciliation: the counts above were taken from the stored runs (each sweep's
`runs.sqlite3`, read directly and through `moq-interop-audit --database`), not from the
script output alone. The scored-row totals reconcile with the per-run tables (final native
QUIC: 27 in the single runs + 6 in the 027 group + 28 in the 19-scenario group = 61;
WebTransport: 25 + 5 + 26 = 56), and the server's completeness entry for the final databases
(with `428b151`) lists the same observed rows: 28 on native QUIC (25 pass, 3 fail) and 26 on
WebTransport (24 pass, 2 fail), and as `not_run` exactly the `not_run` rows of the bound-row
table.

To re-run: build the CLI from a scratch copy as above, then run the commands of this section
(or `MOQ_CLI_BIN=... bash tests/e2e/moq-lite-matrix.sh build/moq-interop-runner`). The live
scripts are not ctests; they need the CLI, ffmpeg, jq, curl, openssl and timeout and skip
(exit 77) without them.

Known limits: one machine on the loopback, so no loss, reordering or delay; one source (an
ffmpeg test pattern, video only); `moqt://` was not tried (the adapter dials `moql://`); the
catalog is staged (75 rows unreviewed), so the 19 scenarios judge 30 rows and a run can never
pass; row 152 needs an adapter action the contract lacks; row 027 is judged only in a run that
holds all five of its scenarios (the matrix script posts that group run since `428b151`).

## Other publishers

A different publisher integrates through the same driver contract with no change
to runner code; see [publisher-harness-guide.md](publisher-harness-guide.md). The
sibling `moq-rs/moq-pub` project was recorded as supporting draft versions only
through 14 with ALPN `moq-00`, so it was not used as a draft-18 or draft-21
acceptance fixture. imquic's example publisher has a bundled draft 22 adapter
(`adapters/imquic/run.sh`); its sweep is
[above](#draft-22-sweep-against-imquic-6836173).
