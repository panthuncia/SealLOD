#pragma once

#include "Render/Runtime/FrameWorkQueue.h"
#include <memory>

namespace org { class Buffer; }

// Published CPU-written ranges remain unavailable to the producer while queued,
// reserved, or submitted. The reservation owns the lease through GPU retirement.
struct VirtualShadowUpgradeWork {
    std::shared_ptr<org::Buffer> buffer;
    uint32_t inputCount = 0;
    std::shared_ptr<const void> uploadLease;
};
using VirtualShadowUpgradeQueue = org::runtime::FrameWorkQueue<VirtualShadowUpgradeWork>;
