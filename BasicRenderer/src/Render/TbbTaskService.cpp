#include "Render/TbbTaskService.h"
#include "Managers/Singletons/TaskSchedulerManager.h"

namespace br {

namespace {
class OrgTaskScope final : public org::runtime::ITaskScope {
public:
    explicit OrgTaskScope(TaskScope value) : scope(std::move(value)) {}
    void Cancel() noexcept override { scope.Cancel(); }
    void Wait() override { scope.Wait(); }
    void CancelAndWait() override { scope.CancelAndWait(); }
    TaskScope scope;
};

TaskLane ToLane(org::runtime::TaskPriority priority) {
    switch (priority) {
    case org::runtime::TaskPriority::FrameCritical: return TaskLane::FrameCritical;
    case org::runtime::TaskPriority::Streaming: return TaskLane::Streaming;
    default: return TaskLane::Background;
    }
}
}

void TbbTaskService::ParallelFor(std::string_view taskName, size_t itemCount, std::function<void(size_t)> func) {
    TaskSchedulerManager::GetInstance().ParallelFor(taskName, itemCount, std::move(func));
}

std::shared_ptr<org::runtime::ITaskScope> TbbTaskService::CreateScope(std::string_view name) {
    return std::make_shared<OrgTaskScope>(TaskSchedulerManager::GetInstance().CreateScope(name));
}

bool TbbTaskService::Submit(const std::shared_ptr<org::runtime::ITaskScope>& scope,
    org::runtime::TaskPriority priority, std::string_view taskName, std::function<void()>&& func) {
    const auto concrete = std::dynamic_pointer_cast<OrgTaskScope>(scope);
    if (!concrete) return false;
    return TaskSchedulerManager::GetInstance().Submit(concrete->scope, ToLane(priority), TaskDomain::General,
        taskName, [func = std::move(func)](const TaskContext&) mutable { func(); });
}

bool TbbTaskService::ScheduleAfter(const std::shared_ptr<org::runtime::ITaskScope>& scope,
    std::chrono::steady_clock::duration delay, org::runtime::TaskPriority priority,
    std::string_view taskName, std::function<void()>&& func) {
    const auto concrete = std::dynamic_pointer_cast<OrgTaskScope>(scope);
    if (!concrete) return false;
    return TaskSchedulerManager::GetInstance().ScheduleAfter(concrete->scope, delay, ToLane(priority), TaskDomain::General,
        taskName, [func = std::move(func)](const TaskContext&) mutable { func(); });
}

}
