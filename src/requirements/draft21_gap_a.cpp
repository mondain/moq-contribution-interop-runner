#include "moq/interop/requirements/draft21_gap_a.h"

namespace moq::interop::requirements {

std::vector<ExecutableBinding> draft21_gap_a_bindings() {
    return {
        {21, "D21-6-3-MUST-NOT-141", "d21-publisher-request-stream-openers",
         "d21-request-stream-first-message-allowed",
         {"publish_observed", "response_delivered"}},
        {21, "D21-9-1-MUST-NOT-289", "d21-publisher-setup-option-multiplicity",
         "d21-setup-option-duplicates-only-when-permitted",
         {"peer_setup_received"}},
        {21, "D21-9-1-1-MUST-NOT-292", "d21-webtransport-publisher-setup",
         "d21-webtransport-no-authority-option", {"peer_setup_received"}},
        {21, "D21-9-1-2-MUST-NOT-299", "d21-webtransport-publisher-setup",
         "d21-webtransport-no-path-option", {"peer_setup_received"}},
        {21, "D21-9-1-1-MUST-294", "d21-webtransport-server-sends-authority",
         "d21-webtransport-authority-invalid-authority",
         {"local_setup_sent", "peer_closed"}},
        {21, "D21-9-1-2-MUST-301", "d21-webtransport-server-sends-path",
         "d21-webtransport-path-invalid-path",
         {"local_setup_sent", "peer_closed"}},
    };
}

}  // namespace moq::interop::requirements
