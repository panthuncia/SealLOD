#pragma once

#include "Render/PreparedPass.h"

namespace br::render {

inline void RecordPreparedComputeUavBarrier(
    org::PreparedResourceReference resource, org::RecordingContext& recording) {
    rhi::BufferBarrier barrier{};
    barrier.buffer = recording.Resolve(resource).GetHandle();
    barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    rhi::BarrierBatch batch{};
    batch.buffers = {&barrier};
    recording.Commands().Barriers(batch);
}

} // namespace br::render
