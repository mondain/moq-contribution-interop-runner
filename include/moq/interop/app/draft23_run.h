#pragma once
#include "moq/interop/app/native_run_manager.h"
#include <atomic>
namespace moq::interop::app {
// Unscored publisher smoke contexts. Every catalog requirement retains NOT_RUN.
bool valid_draft23_run(const RunConfig& config);
void run_draft23(const RunId& id, std::string authority, storage::RunStore& store,
                 const requirements::RequirementCatalog& catalog,
                 const NativeRunManagerConfig& manager, const std::atomic<bool>& stopped,
                 std::unique_ptr<transport::SessionTransport> listener, const RunConfig& config);
}
