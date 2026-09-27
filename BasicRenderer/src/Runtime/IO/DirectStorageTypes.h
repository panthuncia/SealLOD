#pragma once

#include <cstdint>
#include <rhi.h>

namespace br {

struct DirectStorageBufferRegionCopy {
    uint64_t sourceOffset = 0;
    uint32_t sourceSizeBytes = 0;
    uint32_t uncompressedSizeBytes = 0;
    rhi::Resource destinationResource{};
    uint64_t destinationOffset = 0;
};

} // namespace br
