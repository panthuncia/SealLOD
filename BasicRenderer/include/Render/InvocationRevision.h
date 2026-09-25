#pragma once

// Helpers for TypedRenderGraphPass::InvocationRevision implementations: the
// values a pass appends must cover everything its Prepare reads besides the
// declared bindings it resolves and the registry descriptor indices it
// captures (both tracked by the framework).

#include <cstdint>
#include <memory>
#include <vector>

#include "Render/PipelineState.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"

namespace br::render {

template<class Handle>
inline uint64_t HandleRevision(const Handle& handle) noexcept {
    return (uint64_t{handle.generation} << 32) | handle.index;
}

// Identity of the immutable pipeline payload; a recompiled or hot-reloaded
// pipeline publishes a new payload object.
inline uint64_t PipelineRevision(const org::PipelineState& pipeline) {
    return reinterpret_cast<uintptr_t>(pipeline.PeekPayload());
}

template<class T>
inline uint64_t OwnerRevision(const std::shared_ptr<T>& owner) noexcept {
    return reinterpret_cast<uintptr_t>(owner.get());
}

// The frame's descriptor heaps as embedded by PreparedComputeDispatch & co.
inline void AppendFrameHeapRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) {
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    out.push_back(context ? HandleRevision(context->textureDescriptorHeap.GetHandle()) : 0);
    out.push_back(context ? HandleRevision(context->samplerDescriptorHeap.GetHandle()) : 0);
}

} // namespace br::render
