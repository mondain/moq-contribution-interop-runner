# moq-lite-ref adapter

Driver adapter for the moq-lite-06 reference publisher of this repository (`moq-interop-lite-ref-publisher`, built from
`tools/lite-ref`). It is the known-good publisher: the conforming sweep runs all 27 executable scenarios against it on
native QUIC and WebTransport, and the negative sweep runs each named defect.

- Environment: `MOQ_LITE_REF_BIN` names the binary. `MOQ_LITE_REF_ARGS` (optional, word-split) holds the operator's flags:
  `--defect NAME`, `--datagrams`, `--probe-level none|report|increase`, `--frames-per-group N`. They are never taken from
  the runner's request.
- Fixture: broadcast `interop.hang`, track `0.m4s` (the same as `adapters/moq-lite`).
- Command line: `moq-interop-lite-ref-publisher --connect ENDPOINT [flags]`, the same for every scenario unless the
  operator sets flags (`tests/golden/moq-lite-ref-cmdlines.txt`, `tests/e2e/moq-lite-ref-adapter-cmdlines.sh`).
- Datagram scenario: `l06-datagram-size` needs `--datagrams --frames-per-group 1`.
- Exit status: the binary's (0 when the runner ends the session; 1 for a failed dial; 2 for a usage error).
