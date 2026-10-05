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
    }
    throw std::logic_error("unreachable DraftVersion");
}

constexpr std::string_view alpn(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return "moqt-18";
        case DraftVersion::Draft21: return "moqt-21";
        case DraftVersion::Draft22: return "moqt-22";
    }
    throw std::logic_error("unreachable DraftVersion");
}

// Draft 22 has a requirement catalog (and wire codecs) but no executable scenarios yet.
constexpr bool runnable(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::Draft18: return true;
        case DraftVersion::Draft21: return true;
        case DraftVersion::Draft22: return false;
    }
    throw std::logic_error("unreachable DraftVersion");
}

// The single place that turns an externally supplied draft number into a DraftVersion.
constexpr std::optional<DraftVersion> parse_draft(unsigned number) {
    for (const auto draft : {DraftVersion::Draft18, DraftVersion::Draft21, DraftVersion::Draft22}) {
        if (draft_number(draft) == number) return draft;
    }
    return std::nullopt;
}

constexpr bool known_alpn(std::string_view value) {
    for (const auto draft : {DraftVersion::Draft18, DraftVersion::Draft21, DraftVersion::Draft22}) {
        if (alpn(draft) == value) return true;
    }
    return false;
}

// Runs the callable for `draft`; the other is never evaluated. Draft 22 is not runnable, so
// reaching it here is a logic error (the run gate refuses it first). Both callables must
// return the same type; give reference-returning lambdas an explicit return type.
template <class When18, class When21>
decltype(auto) by_draft(DraftVersion draft, When18&& when_18, When21&& when_21) {
    switch (draft) {
        case DraftVersion::Draft18: return std::forward<When18>(when_18)();
        case DraftVersion::Draft21: return std::forward<When21>(when_21)();
        case DraftVersion::Draft22: throw std::logic_error("draft 22 is not runnable");
    }
    throw std::logic_error("unreachable DraftVersion");
}

}  // namespace moq::interop::app
