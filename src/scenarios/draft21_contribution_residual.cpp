// Draft-21 contribution profiles that close the last required rows the earlier
// slices could not induce with canned stimulus alone: delivery to concurrent
// subscriptions, conjunctive filters, Subgroup membership, fill fetch stream
// failure and cancellation. Every expectation cites draft-ietf-moq-transport-21.
//
// Fixture contract (this slice does not use the Group 7 contract of
// draft21_gap_a.h). The configured track holds Groups 0 and 1, each with Objects
// 0 and 1, delivered to every subscription that asks for them; the first Group of
// an LOC/CMAF GOP-per-Group source has exactly this shape. Every context also asks the runner to acknowledge a PUBLISH_NAMESPACE
// the publisher opens, as any subscriber that wants a publisher to keep serving
// requests would; this is not scored.
//
// The rows live in three units by what drives them (subscription, publisher
// observation, token and alias); this file only fixes the order they are returned in.

#include "draft21_contribution_support.h"

#include <iterator>

namespace moq::interop::scenarios::d21c {

std::vector<Spec> residual_specs() {
    auto result = residual_subscription_specs();
    for (auto group : {&residual_publisher_specs, &residual_token_specs}) {
        auto specs = group();
        result.insert(result.end(), std::make_move_iterator(specs.begin()),
                      std::make_move_iterator(specs.end()));
    }
    return result;
}

}  // namespace moq::interop::scenarios::d21c
