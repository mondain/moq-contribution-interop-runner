#pragma once

#include "moq/interop/app/types.h"

#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace moq::interop::app {

// Per-draft facts. Every function is an exhaustive switch with no default label, so adding
// a draft makes each of them (and every other switch over DraftVersion in a library built
// with -Werror=switch) a compile error until it is handled.

constexpr unsigned draft_number(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return 18;
        case DraftVersion::Draft21: return 21;
        case DraftVersion::Draft22: return 22;
        case DraftVersion::MoqLite06: return 106;
    }
    throw std::logic_error("unreachable DraftVersion");
}

// Scenario, session and evaluator code is shared by lineage: draft 22 runs on draft 21's family.
constexpr DraftVersion family_draft(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return DraftVersion::Draft18;
        case DraftVersion::Draft21: return DraftVersion::Draft21;
        case DraftVersion::Draft22: return DraftVersion::Draft21;
        case DraftVersion::MoqLite06: return DraftVersion::MoqLite06;  // no lineage
    }
    throw std::logic_error("unreachable DraftVersion");
}

constexpr std::string_view alpn(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return "moqt-18";
        case DraftVersion::Draft21: return "moqt-21";
        case DraftVersion::Draft22: return "moqt-22";
        case DraftVersion::MoqLite06: return "moq-lite-06";
    }
    throw std::logic_error("unreachable DraftVersion");
}

// Whether the API accepts runs for the draft (when the server also has its catalog). Draft 22 runs its shared
// scenarios on draft 21's family by lineage and its own scenarios as raw probes on the draft 22 wire.
constexpr bool runnable(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return true;
        case DraftVersion::Draft21: return true;
        case DraftVersion::Draft22: return true;
        case DraftVersion::MoqLite06: return false;  // identification only until a later sub-project
    }
    throw std::logic_error("unreachable DraftVersion");
}

// True for the MoQ Transport drafts (18/21/22), false for moq-lite.
constexpr bool is_moqt(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return true;
        case DraftVersion::Draft21: return true;
        case DraftVersion::Draft22: return true;
        case DraftVersion::MoqLite06: return false;
    }
    throw std::logic_error("unreachable DraftVersion");
}

// External text form. MoQ Transport drafts are integers in the API, so only moq-lite has a text form.
constexpr std::string_view draft_text(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return "";
        case DraftVersion::Draft21: return "";
        case DraftVersion::Draft22: return "";
        case DraftVersion::MoqLite06: return "moq-lite-06";
    }
    throw std::logic_error("unreachable DraftVersion");
}

// The single place that turns an externally supplied draft number into a DraftVersion.
constexpr std::optional<DraftVersion> parse_draft(unsigned number) {
    for (const auto draft : {DraftVersion::Draft18, DraftVersion::Draft21, DraftVersion::Draft22, DraftVersion::MoqLite06}) {
        if (draft_number(draft) == number) return draft;
    }
    return std::nullopt;
}

constexpr std::optional<DraftVersion> parse_draft_text(std::string_view text) {
    if (text == draft_text(DraftVersion::MoqLite06)) return DraftVersion::MoqLite06;
    return std::nullopt;
}

constexpr bool known_alpn(std::string_view value) {
    for (const auto draft : {DraftVersion::Draft18, DraftVersion::Draft21, DraftVersion::Draft22, DraftVersion::MoqLite06}) {
        if (alpn(draft) == value) return true;
    }
    return false;
}

// Runs the callable for `draft`; the other is never evaluated. It chooses between the two family drafts
// only: a site that can see draft 22 must switch on the draft itself (or use family_draft first), so
// reaching draft 22 here is a logic error. Both callables must return the same type; give
// reference-returning lambdas an explicit return type.
template <class When18, class When21>
decltype(auto) by_draft(DraftVersion draft, When18&& when_18, When21&& when_21) {
    switch (draft) {
        case DraftVersion::Draft18: return std::forward<When18>(when_18)();
        case DraftVersion::Draft21: return std::forward<When21>(when_21)();
        case DraftVersion::Draft22: throw std::logic_error("by_draft chooses between drafts 18 and 21 only");
        case DraftVersion::MoqLite06: throw std::logic_error("by_draft does not apply to moq-lite");
    }
    throw std::logic_error("unreachable DraftVersion");
}

}  // namespace moq::interop::app
