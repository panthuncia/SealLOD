#pragma once

#include "Render/Runtime/ITaskService.h"

namespace br {

class TbbTaskService final : public org::runtime::ITaskService {
public:
    void ParallelFor(std::string_view taskName, size_t itemCount, std::function<void(size_t)> func) override;
};

}
