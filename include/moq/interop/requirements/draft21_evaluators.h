#pragma once

#include "moq/interop/requirements/scoring.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/scenarios/draft21_announcement.h"
#include "moq/interop/scenarios/raw_probe.h"

#include <vector>
#include <span>

namespace moq::interop::requirements {

// The announcement profile evaluates only directly observed request-stream
// placement and permitted-opening behavior. Other applicable rows are NOT_RUN.
std::vector<Outcome> evaluate_draft21_announcement(
    const RequirementCatalog& catalog,
    const scenarios::Draft21AnnouncementContext& context);

std::vector<ExecutableBinding> draft21_executable_bindings();

std::vector<Outcome> evaluate_draft21_raw_probes(
    const RequirementCatalog& catalog,
    std::span<const scenarios::RawProbeTranscript> transcripts);

std::vector<Outcome> evaluate_draft21_close_probe(
    const RequirementCatalog& catalog,
    const scenarios::RawProbeTranscript& transcript);

std::vector<Outcome> evaluate_draft21_request_probe(
    const RequirementCatalog& catalog,
    const scenarios::RawProbeTranscript& transcript);

}  // namespace moq::interop::requirements
