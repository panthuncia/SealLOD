#pragma once

// Recording helpers for the common pass shapes. Each records directly through the pass's record context,
// which resolves declared views and resources against the frame and retains what the commands name.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "Render/PreparedPass.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace br::render {

// The UintRootConstant block every BasicRenderer root signature carries (MiscUintRootSignatureIndex).
using MiscConstants = std::array<uint32_t, NumMiscUintRootConstants>;

inline void PushMiscConstants(org::RecordingContext& context, const MiscConstants& constants) {
    context.PushConstants(MiscUintRootSignatureIndex, constants);
}

// Bind, push the constant block and dispatch. Zero groups records nothing.
inline void DispatchCompute(org::RecordingContext& context, const org::PipelineState& pipeline,
    const MiscConstants& constants, uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) {
    if (!groupsX || !groupsY || !groupsZ) return;
    context.BindComputePipeline(pipeline);
    PushMiscConstants(context, constants);
    context.Commands().Dispatch(groupsX, groupsY, groupsZ);
}

// Bind, push the constant block and execute indirect dispatches.
inline void DispatchComputeIndirect(org::RecordingContext& context, const org::PipelineState& pipeline,
    const MiscConstants& constants, const std::shared_ptr<const rhi::CommandSignaturePtr>& signature,
    org::ResourceBindingToken arguments, uint64_t argumentOffset = 0, uint32_t maxCommandCount = 1,
    std::optional<org::ResourceBindingToken> count = std::nullopt, uint64_t countOffset = 0) {
    context.BindComputePipeline(pipeline);
    PushMiscConstants(context, constants);
    context.ExecuteIndirect(signature, arguments, argumentOffset, maxCommandCount, count, countOffset);
}

// One full-screen triangle into `color`.
inline void DrawFullscreen(org::RecordingContext& context, const org::PipelineState& pipeline,
    const MiscConstants& constants, const rhi::ColorAttachment& color, uint32_t width, uint32_t height,
    const char* debugName = nullptr) {
    rhi::PassBeginInfo begin{};
    begin.colors = {&color, 1};
    begin.width = width;
    begin.height = height;
    begin.debugName = debugName;
    auto& commands = context.Commands();
    commands.BeginPass(begin);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);
    context.BindGraphicsPipeline(pipeline);
    PushMiscConstants(context, constants);
    commands.Draw(3, 1, 0, 0);
    commands.EndPass();
}

// A color attachment on a declared render-target view.
inline rhi::ColorAttachment ColorTarget(org::RecordingContext& context, const org::DeclaredViewToken& renderTarget,
    rhi::LoadOp loadOp = rhi::LoadOp::Load, uint32_t mip = 0) {
    rhi::ColorAttachment color{};
    color.rtv = context.View(renderTarget);
    color.resource = context.Resolve(renderTarget).GetHandle();
    color.mipSlice = mip;
    color.loadOp = loadOp;
    color.storeOp = rhi::StoreOp::Store;
    return color;
}

// Clears a resource declared with builder.UnorderedAccessClear: its view 0 is shader-visible, view 1 is not.
inline void ClearUavUint(org::RecordingContext& context, const org::DeclaredViewToken& shaderVisible,
    const org::DeclaredViewToken& cpuVisible, uint32_t value) {
    rhi::UavClearInfo info{};
    info.resource = context.Resolve(shaderVisible);
    info.shaderVisible = context.View(shaderVisible);
    info.cpuVisible = context.View(cpuVisible);
    rhi::UavClearUint clear{};
    for (auto& component : clear.v) component = value;
    context.Commands().ClearUavUint(info, clear);
}
inline void ClearUavFloat(org::RecordingContext& context, const org::DeclaredViewToken& shaderVisible,
    const org::DeclaredViewToken& cpuVisible, float value) {
    rhi::UavClearInfo info{};
    info.resource = context.Resolve(shaderVisible);
    info.shaderVisible = context.View(shaderVisible);
    info.cpuVisible = context.View(cpuVisible);
    rhi::UavClearFloat clear{};
    for (auto& component : clear.v) component = value;
    context.Commands().ClearUavFloat(info, clear);
}

} // namespace br::render
