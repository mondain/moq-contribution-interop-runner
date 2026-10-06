#pragma once

#include "moq/interop/app/types.h"
#include "moq/interop/requirements/draft22_lineage_data.h"

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::scenarios {
struct RawProbeDefinition;
struct RawProbeTranscript;
}  // namespace moq::interop::scenarios

namespace moq::interop::app {

// What the registry predicates know about an implemented own draft 22 scenario (one of
// lineage_data::kOwnScenarios22). Own scenarios are always raw probes.
struct OwnScenarioTraits22 {
    std::string_view id;
    bool requires_track{true};
};

// Own draft 22 scenarios with a production implementation. Each entry needs a probe in
// src/app/own_scenario_dispatch_22.cpp (the header stays free of scenario code so every target that
// asks a registry predicate links without the scenarios library).
inline constexpr auto kOwnScenarioTraits22 = std::to_array<OwnScenarioTraits22>({
    {"d22-subscribe-bounded-location-range", true},
    {"d22-update-subscription-location-range", true},
    {"d22-fetch-bounded-location-range", true},
});

// An own scenario implementation: its traits and the probe the run manager drives. `probe` is called
// on the run's execution config (draft 21 family) under ScopedWireDraft(22).
struct OwnScenario22 {
    OwnScenarioTraits22 traits;
    std::function<scenarios::RawProbeDefinition(const RunConfig&)> probe;
};

// An own draft 22 evaluator (one of lineage_data::kOwnEvaluators22): its verdict on one transcript of a
// scenario that a row naming this evaluator names, or nothing when the transcript says nothing for it.
struct OwnEvaluator22 {
    std::string_view id;
    std::function<std::optional<bool>(const scenarios::RawProbeTranscript&)> evaluate;
};

// The overlay through which a test registers a stub own scenario or evaluator without touching the
// production tables; an overlay entry shadows a production one with the same id. Register only while no
// run is active: spans handed out stay valid until the next registration change.
class OwnScenarioRegistry22 {
public:
    static OwnScenarioRegistry22& instance() {
        static OwnScenarioRegistry22 registry;
        return registry;
    }

    std::optional<OwnScenarioTraits22> traits(std::string_view id) const {
        std::lock_guard lock(mutex_);
        for (const auto& entry : scenarios_)
            if (entry.traits.id == id) return entry.traits;
        for (const auto& entry : kOwnScenarioTraits22)
            if (entry.id == id) return entry;
        return std::nullopt;
    }

    std::optional<OwnScenario22> registered_scenario(std::string_view id) const {
        std::lock_guard lock(mutex_);
        for (const auto& entry : scenarios_)
            if (entry.traits.id == id) return entry;
        return std::nullopt;
    }

    std::optional<OwnEvaluator22> registered_evaluator(std::string_view id) const {
        std::lock_guard lock(mutex_);
        for (const auto& entry : evaluators_)
            if (entry.id == id) return entry;
        return std::nullopt;
    }

    // Implemented own ids, in kOwnScenarios22 order.
    std::span<const std::string_view> ids() const {
        std::lock_guard lock(mutex_);
        return ids_;
    }

    // `shared` followed by the implemented own ids; `shared` itself when none is implemented. The combined
    // list is cached from the first `shared` seen after a registration change: the only caller
    // (executable_scenarios(22)) always passes the same static list.
    std::span<const std::string_view> with_own(std::span<const std::string_view> shared) {
        std::lock_guard lock(mutex_);
        if (ids_.empty()) return shared;
        if (executable_.empty()) {
            executable_.assign(shared.begin(), shared.end());
            executable_.insert(executable_.end(), ids_.begin(), ids_.end());
        }
        return executable_;
    }

    void add(OwnScenario22 scenario) {
        std::lock_guard lock(mutex_);
        const auto& own = requirements::lineage_data::kOwnScenarios22;
        const auto found = std::find(own.begin(), own.end(), scenario.traits.id);
        if (found == own.end() ||
            std::any_of(scenarios_.begin(), scenarios_.end(),
                        [&](const auto& entry) { return entry.traits.id == scenario.traits.id; }))
            throw std::invalid_argument("not an unregistered own draft 22 scenario: " +
                                        std::string(scenario.traits.id));
        scenario.traits.id = *found;  // the generated table's storage outlives any caller's
        scenarios_.push_back(std::move(scenario));
        rebuild();
    }

    void remove(std::string_view id) {
        std::lock_guard lock(mutex_);
        std::erase_if(scenarios_, [&](const auto& entry) { return entry.traits.id == id; });
        rebuild();
    }

    void add(OwnEvaluator22 evaluator) {
        std::lock_guard lock(mutex_);
        const auto& own = requirements::lineage_data::kOwnEvaluators22;
        const auto found = std::find(own.begin(), own.end(), evaluator.id);
        if (found == own.end() ||
            std::any_of(evaluators_.begin(), evaluators_.end(),
                        [&](const auto& entry) { return entry.id == evaluator.id; }))
            throw std::invalid_argument("not an unregistered own draft 22 evaluator: " +
                                        std::string(evaluator.id));
        evaluator.id = *found;
        evaluators_.push_back(std::move(evaluator));
    }

    void remove_evaluator(std::string_view id) {
        std::lock_guard lock(mutex_);
        std::erase_if(evaluators_, [&](const auto& entry) { return entry.id == id; });
    }

private:
    OwnScenarioRegistry22() { rebuild(); }

    void rebuild() {
        ids_.clear();
        executable_.clear();
        for (const auto id : requirements::lineage_data::kOwnScenarios22) {
            const bool overlay = std::any_of(scenarios_.begin(), scenarios_.end(),
                                             [&](const auto& entry) { return entry.traits.id == id; });
            const bool production = std::any_of(kOwnScenarioTraits22.begin(), kOwnScenarioTraits22.end(),
                                                [&](const auto& entry) { return entry.id == id; });
            if (overlay || production) ids_.push_back(id);
        }
    }

    mutable std::mutex mutex_;
    std::vector<OwnScenario22> scenarios_;
    std::vector<OwnEvaluator22> evaluators_;
    std::vector<std::string_view> ids_;
    std::vector<std::string_view> executable_;
};

// Registers a stub own scenario for the lifetime of the object (tests).
class ScopedOwnScenario22 {
public:
    explicit ScopedOwnScenario22(OwnScenario22 scenario) : id_(scenario.traits.id) {
        OwnScenarioRegistry22::instance().add(std::move(scenario));
    }
    ~ScopedOwnScenario22() { OwnScenarioRegistry22::instance().remove(id_); }
    ScopedOwnScenario22(const ScopedOwnScenario22&) = delete;
    ScopedOwnScenario22& operator=(const ScopedOwnScenario22&) = delete;

private:
    std::string_view id_;
};

// Registers a stub own evaluator for the lifetime of the object (tests).
class ScopedOwnEvaluator22 {
public:
    explicit ScopedOwnEvaluator22(OwnEvaluator22 evaluator) : id_(evaluator.id) {
        OwnScenarioRegistry22::instance().add(std::move(evaluator));
    }
    ~ScopedOwnEvaluator22() { OwnScenarioRegistry22::instance().remove_evaluator(id_); }
    ScopedOwnEvaluator22(const ScopedOwnEvaluator22&) = delete;
    ScopedOwnEvaluator22& operator=(const ScopedOwnEvaluator22&) = delete;

private:
    std::string_view id_;
};

// The kOwnScenarios22 ids that have an implementation (production or registered).
inline std::span<const std::string_view> own_scenario_ids_22() {
    return OwnScenarioRegistry22::instance().ids();
}

// The traits of an implemented own draft 22 scenario; nothing for any other id.
inline std::optional<OwnScenarioTraits22> own_scenario_22(std::string_view id) {
    return OwnScenarioRegistry22::instance().traits(id);
}

}  // namespace moq::interop::app
