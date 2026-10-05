# moq5 (libmoq) adapter

`run.sh` lets the runner drive `examples/service/media_send` from the sibling `moq5`
checkout (built as `moq_example_media_send`) as a contribution publisher, for draft 18
or 21. This is a copy of `tools/interop-adapter/` in moq5, which is the maintained
source; `test-adapter.sh` is its no-network contract test (needs `bash` and `jq`).

```sh
# in moq5: cmake --build build/dev --target moq_example_media_send
export MOQ5_MEDIA_SEND_BIN=/path/to/moq5/build/dev/examples/service/moq_example_media_send
build/moq-interop-runner ... --driver-executable $PWD/adapters/moq5/run.sh \
  --driver-fixture /path/to/any/readable/file
```

Use `"transport": "native-quic"`. WebTransport only connects with a backend that offers
the current WebTransport profile (not the picoquic one in the moq5 `dev` build).

The fixture is ignored (the publisher sends placeholder access units), and there are no
per-scenario modes, so scenarios that need a particular object layout or publisher
behaviour stay `not_run`. Malformed or unsupported requests exit 64 before the publisher
starts. See moq5 `docs/conformance.md` (Draft 21) for the latest scores.
