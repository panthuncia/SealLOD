#pragma once

#include "Render/Runtime/ITaskService.h"

namespace br {

class TbbTaskService final : public org::runtime::ITaskService {
public:
    void ParallelFor(std::string_view taskName, size_t itemCount, std::function<void(size_t)> func) override;
    std::shared_ptr<org::runtime::ITaskScope> CreateScope(std::string_view name) override;
    bool Submit(const std::shared_ptr<org::runtime::ITaskScope>& scope, org::runtime::TaskPriority priority,
        std::string_view taskName, std::function<void()>&& func) override;
    bool ScheduleAfter(const std::shared_ptr<org::runtime::ITaskScope>& scope,
        std::chrono::steady_clock::duration delay, org::runtime::TaskPriority priority,
        std::string_view taskName, std::function<void()>&& func) override;
};

}
