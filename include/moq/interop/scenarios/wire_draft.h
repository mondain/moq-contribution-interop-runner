#pragma once

#include "moq/interop/wire/draft21/publish.h"

namespace moq::interop::scenarios {

// The wire draft of the run on this thread (21 by default). The run manager sets it for the duration of
// a run so draft 21-family scenario code can read a draft 22 peer's PUBLISH.
unsigned current_wire_draft() noexcept;

class ScopedWireDraft {
public:
    explicit ScopedWireDraft(unsigned draft) noexcept;
    ~ScopedWireDraft();
    ScopedWireDraft(const ScopedWireDraft&) = delete;
    ScopedWireDraft& operator=(const ScopedWireDraft&) = delete;

private:
    unsigned previous_;
};

// Drop-in for draft21::decode_publish. Wire draft 22 decodes with draft22::decode_publish and presents
// the result in draft 21's form (LOCATION_FILTER becomes a length-prefixed byte parameter). An Absolute
// {0,0} filter has no draft 21 form and is refused with a ProtocolViolation.
wire::DecodeResult<wire::draft21::PublishMessage> decode_publish_for_wire(wire::Cursor& input);

}  // namespace moq::interop::scenarios
