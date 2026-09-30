#pragma once

#include "moq/interop/requirements/catalog.h"
#include "moq/interop/storage/run_store.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace moq::interop::http {

nlohmann::json serialize_result(const storage::RunRecord& run,
                                const requirements::RequirementCatalog& catalog);
std::string serialize_tap14(const storage::RunRecord& run,
                            const requirements::RequirementCatalog& catalog);

}  // namespace moq::interop::http
