#pragma once

#include <memory>
#include <ORGModuleServices/Async/GraphScheduler.h>

namespace br {
class TaskSchedulerManager;
// The host scheduler must outlive this adapter and every graph using it.
std::shared_ptr<org::async::GraphScheduler> MakeGraphSchedulerAdapter(TaskSchedulerManager& scheduler);
}
