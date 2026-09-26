#pragma once

#include <cstdint>

namespace br::render {

struct TransientWindRegion {
    std::uint32_t transformBaseMatrices = 0;
    std::uint32_t previousTransformBaseMatrices = 0;
    std::uint32_t inverseSkinBaseMatrices = 0;
    std::uint32_t capacityMatrices = 0;
    bool valid = false;
};

// Serialized allocator for GPU-written procedural-pose ranges. It owns no
// scene state; immutable skeleton membership comes from PublishedPoseState.
class IWindPaletteService {
public:
    virtual ~IWindPaletteService() = default;
    virtual TransientWindRegion ReserveTransientWindRegion(std::uint32_t matrixCapacity) = 0;
    virtual void EnsureTransientWindInstanceSlots(std::uint32_t drawRecordCapacity) = 0;
};

} // namespace br::render
