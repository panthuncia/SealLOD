#pragma once

#include <cstdint>

#include <rhi.h>

// D3D12 packs small buffers as ranges of a shared resource at 512-byte starts; a structured view whose stride does not divide
// 512 cannot start there, so such a buffer takes a resource of its own (rhi::RF_OwnResource).
inline rhi::ResourceFlags StructuredBackingFlags(uint64_t stride) noexcept {
    return stride != 0 && 512u % stride != 0 ? rhi::RF_OwnResource : rhi::RF_None;
}
