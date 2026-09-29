#pragma once

#include "moq/interop/requirements/scoring.h"
#include "moq/interop/scenarios/draft21_announcement.h"

#include <vector>

namespace moq::interop::requirements {

// The announcement profile evaluates only directly observed request-stream
// placement and permitted-opening behavior. Other applicable rows are NOT_RUN.
std::vector<Outcome> evaluate_draft21_announcement(
    const RequirementCatalog& catalog,
    const scenarios::Draft21AnnouncementContext& context);

}  // namespace moq::interop::requirements
