#pragma once

#include "moq/interop/wire/draft21/publish.h"

#include <optional>
#include <string>
#include <string_view>

namespace moq::interop::scenarios {

// The wire draft of the run on this thread (21 by default). The run manager sets it for the duration of
// a run so draft 21-family scenario code can read a draft 22 peer's PUBLISH.
unsigned current_wire_draft() noexcept;

// Construction also starts a clean adapter-refusal record on this thread (see take_adapter_refusal), so a
// run never inherits an earlier run's refusal. Destruction restores the previous wire draft; a refusal
// recorded inside the scope survives it unless the enclosing scope already held one (the first is kept).
class ScopedWireDraft {
public:
    explicit ScopedWireDraft(unsigned draft) noexcept;
    ~ScopedWireDraft();
    ScopedWireDraft(const ScopedWireDraft&) = delete;
    ScopedWireDraft& operator=(const ScopedWireDraft&) = delete;

private:
    unsigned previous_;
    std::optional<std::string> previous_refusal_;
};

// Adapter refusals. decode_publish_for_wire records every refusal on the calling thread, so the run
// manager can fail the run loudly even where a scenario drops the DecodeError (it reads the record on the
// run's worker thread, where every decode of the run happens). Only the FIRST detail is kept until taken:
// later refusals of the same run add nothing the run's harness_error would say.
void note_adapter_refusal(std::string_view detail);
// Returns this thread's recorded refusal, if any, and clears it.
std::optional<std::string> take_adapter_refusal();

// The DecodeError detail of the refusal below, so a run can tell the adapter's limit from a peer fault.
inline constexpr std::string_view kUnrepresentableLocationFilterDetail =
    "draft-22 Absolute {0,0} filter has no draft-21 form";

// Drop-in for draft21::decode_publish. Wire draft 22 decodes with draft22::decode_publish and presents
// the result in draft 21's form (LOCATION_FILTER becomes a length-prefixed byte parameter). An Absolute
// {0,0} filter has no draft 21 form and is refused with a ProtocolViolation.
wire::DecodeResult<wire::draft21::PublishMessage> decode_publish_for_wire(wire::Cursor& input);

}  // namespace moq::interop::scenarios
