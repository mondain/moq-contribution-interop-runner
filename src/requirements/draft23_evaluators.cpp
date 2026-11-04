#include "moq/interop/requirements/draft23_evaluators.h"

#include "moq/interop/wire/cursor.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <variant>

namespace moq::interop::requirements {
namespace {

constexpr std::string_view kScenarios[] = {"d23-publisher-smoke", "d23-direct-subscribe-smoke"};

constexpr std::uint64_t kPathOption = 1;
constexpr std::uint64_t kAuthorityOption = 5;
constexpr std::uint64_t kMaxControlMessageSizeOption = 9;

struct Row {
    std::string_view requirement;
    std::string_view evaluator;
};
constexpr Row kMultiplicity{"D23-9-1-MUST-NOT-265", "d23-setup-option-multiplicity"};
constexpr Row kMaxSize{"D23-9-1-8-MUST-288", "d23-setup-max-control-message-size"};
constexpr Row kAuthority{"D23-9-1-1-MUST-269", "d23-setup-native-authority"};
constexpr Row kPath{"D23-9-1-2-MUST-273", "d23-setup-native-path"};
constexpr Row kQuery{"D23-9-1-2-MUST-274", "d23-setup-native-path-query"};
constexpr Row kRows[] = {kMultiplicity, kMaxSize, kAuthority, kPath, kQuery};

// The state of a row no SETUP evaluator judges.
OutcomeState staged_state(const Requirement& row) {
    if (!row.reviewed) return OutcomeState::NotRun;
    switch (row.testability) {
        case Testability::NotTestable: return OutcomeState::NotTestable;
        case Testability::NotApplicable: return OutcomeState::NotApplicable;
        case Testability::Testable: return OutcomeState::NotRun;
    }
    return OutcomeState::NotRun;
}

OutcomeState verdict(bool ok) { return ok ? OutcomeState::Pass : OutcomeState::Fail; }

std::vector<const wire::draft21::KeyValue*> options_of(const session::Draft23PeerSetup& setup, std::uint64_t type) {
    std::vector<const wire::draft21::KeyValue*> found;
    for (const auto& option : setup.options) if (option.type == type) found.push_back(&option);
    return found;
}

std::optional<std::string> text_of(const wire::draft21::KeyValue& option) {
    const auto* bytes = std::get_if<std::vector<std::byte>>(&option.value);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// Option types whose definition allows a single instance per SETUP (Section 9.1); AUTHORIZATION TOKEN (0x03) may repeat.
bool single_instance_option(std::uint64_t type) { return type == kPathOption || (type >= 4 && type <= 9); }

OutcomeState multiplicity(const session::Draft23PeerSetup& setup) {
    std::map<std::uint64_t, int> counts;
    for (const auto& option : setup.options) if (single_instance_option(option.type)) ++counts[option.type];
    return verdict(std::none_of(counts.begin(), counts.end(), [](const auto& entry) { return entry.second > 1; }));
}

OutcomeState max_size(const session::Draft23PeerSetup& setup) {
    const auto found = options_of(setup, kMaxControlMessageSizeOption);
    if (found.empty()) return OutcomeState::NotApplicable;  // nothing sent, so no value to constrain
    const auto* bytes = std::get_if<std::vector<std::byte>>(&found.front()->value);
    if (!bytes) return OutcomeState::Fail;
    wire::Cursor input(*bytes);
    const auto decoded = wire::read_vi64(input);
    const auto* value = std::get_if<std::uint64_t>(&decoded);
    return verdict(value && input.remaining() == 0 && *value >= 65535);
}

OutcomeState authority(const session::Draft23PeerSetup& setup, const Draft23SetupContext& context) {
    const auto found = options_of(setup, kAuthorityOption);
    return verdict(found.size() == 1 && text_of(*found.front()) == context.authority);
}

// PATH is the path-abempty, then a question mark and the query when the URI has one (Section 9.1.2).
std::optional<std::string> path_option(const session::Draft23PeerSetup& setup) {
    const auto found = options_of(setup, kPathOption);
    if (found.size() != 1) return std::nullopt;
    return text_of(*found.front());
}

OutcomeState path(const session::Draft23PeerSetup& setup, const Draft23SetupContext& context) {
    const auto value = path_option(setup);
    return verdict(value && value->substr(0, value->find('?')) == context.path);
}

OutcomeState query(const session::Draft23PeerSetup& setup, const Draft23SetupContext& context) {
    if (!context.query) return OutcomeState::NotApplicable;
    const auto value = path_option(setup);
    if (!value) return OutcomeState::Fail;
    const auto mark = value->find('?');
    return verdict(mark != std::string::npos && value->substr(mark + 1) == *context.query);
}

}  // namespace

std::vector<Outcome> evaluate_draft23_setup(const RequirementCatalog& catalog, const Draft23SetupContext& context) {
    std::vector<Outcome> outcomes;
    outcomes.reserve(catalog.requirements.size());
    for (const auto& row : catalog.requirements) {
        auto state = staged_state(row);
        const bool setup_row = std::any_of(std::begin(kRows), std::end(kRows),
                                           [&](const Row& candidate) { return candidate.requirement == row.id; });
        if (setup_row && row.reviewed && row.testability == Testability::Testable && context.peer_setup) {
            const auto& setup = *context.peer_setup;
            const bool native_only = row.id == kAuthority.requirement || row.id == kPath.requirement ||
                                     row.id == kQuery.requirement;
            if (native_only && context.webtransport) state = OutcomeState::NotApplicable;
            else if (row.id == kMultiplicity.requirement) state = multiplicity(setup);
            else if (row.id == kMaxSize.requirement) state = max_size(setup);
            else if (row.id == kAuthority.requirement) state = authority(setup, context);
            else if (row.id == kPath.requirement) state = path(setup, context);
            else if (row.id == kQuery.requirement) state = query(setup, context);
        }
        outcomes.push_back({row.id, state});
    }
    return outcomes;
}

std::vector<ExecutableBinding> draft23_executable_bindings() {
    std::vector<ExecutableBinding> bindings;
    for (const auto& row : kRows)
        for (const auto scenario : kScenarios)
            bindings.push_back({23, std::string(row.requirement), std::string(scenario), std::string(row.evaluator),
                                {"peer_setup_received"}});
    return bindings;
}

}  // namespace moq::interop::requirements
