# moq-lite-ref adapter

Driver adapter for the moq-lite-06 reference publisher of this repository (`moq-interop-lite-ref-publisher`, built from
`tools/lite-ref`). It is the known-good publisher: the conforming sweep runs all 27 executable scenarios against it on
native QUIC and WebTransport, and the negative sweep runs each named defect.

- Environment: `MOQ_LITE_REF_BIN` names the binary. `MOQ_LITE_REF_ARGS` (optional, word-split) holds the operator's flags,
  appended last: `--defect NAME`, `--datagrams`, `--probe-level none|report|increase`, `--frames-per-group N`, `--groups N`,
  `--group-interval-polls N`, `--retract-after-polls N`. The runner's request never carries flags.
- Per-scenario flags: the adapter itself adds what two scenarios need so that one run of all 27 can be judged:
  `--datagrams --frames-per-group 1` for `l06-datagram-size` (a datagram carries a single-frame group) and
  `--retract-after-polls 200` for `l06-announce-lifecycle` (row 152 needs a retraction). It reads only `scenario_id` for this.
- Fixture: broadcast `interop.hang`, track `0.m4s` (the same as `adapters/moq-lite`).
- Command line: `moq-interop-lite-ref-publisher --connect ENDPOINT [flags]`, the same for every scenario but the two above
  (`tests/golden/moq-lite-ref-cmdlines.txt`, `tests/e2e/moq-lite-ref-adapter-cmdlines.sh`).
- Sweeps: `tests/e2e/moq-lite-ref-matrix.sh` (conforming, plus one run of all 27 scenarios that is a `pass`) and
  `tests/e2e/moq-lite-ref-negative.sh` (each `--defect` fails exactly its row).
- Exit status: the binary's (0 when the runner ends the session; 1 for a failed dial; 2 for a usage error).
