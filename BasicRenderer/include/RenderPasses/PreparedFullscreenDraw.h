#pragma once

#include "Render/PreparedPass.h"
#include "Render/ShaderAPI.h"
#include "ShaderBuffers.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace br::render {

struct PreparedFullscreenDraw {
    rhi::DescriptorHeapHandle resourceHeap{};
    rhi::DescriptorHeapHandle samplerHeap{};
    std::optional<org::PreparedResourceReference> targetResource;
    uint32_t targetMip = 0;
    std::optional<org::PreparedDescriptorReference> renderTargetReference;
    std::optional<org::ExternalBindingKey> externalRenderTarget;
    rhi::LoadOp loadOp = rhi::LoadOp::Load;
    decltype(rhi::ColorAttachment{}.clear) clear{};
    uint32_t width = 0;
    uint32_t height = 0;
    std::string debugName;
    std::optional<org::PreparedProgramReference> program;
    std::vector<unsigned int> descriptorIndices;
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    rhi::ShaderStage constantStage = rhi::ShaderStage::Pixel;
};

// Capture the coherent program version and its declaration-derived descriptor mapping.
inline void BindPreparedProgram(
    PreparedFullscreenDraw& draw,
    const org::FramePreparationContext& preparation,
    const org::PipelineState& pipeline,
    org::BackendInstanceId backend = org::BackendInstanceId::Primary) {
    auto binding = preparation.CaptureProgramBinding(pipeline, backend);
    draw.program = binding.program;
    draw.descriptorIndices = std::move(binding.descriptorIndices);
}

inline void RecordPreparedFullscreenDraw(
    const PreparedFullscreenDraw& data, org::RecordingContext& recording) {
    auto& commands = recording.Commands();
    if (data.resourceHeap.valid())
        commands.SetDescriptorHeaps(data.resourceHeap,
            data.samplerHeap.valid() ? std::optional{data.samplerHeap} : std::nullopt);
    rhi::ColorAttachment color{};
    color.rtv = data.externalRenderTarget
        ? recording.Resolve(*data.externalRenderTarget)
        : recording.Resolve(data.renderTargetReference.value());
    color.mipSlice = data.targetMip;
    if (data.targetResource) color.resource = recording.Resolve(*data.targetResource).GetHandle();
    color.loadOp = data.loadOp;
    color.storeOp = rhi::StoreOp::Store;
    color.clear = data.clear;
    rhi::PassBeginInfo begin{};
    begin.colors = {&color, 1};
    begin.width = data.width;
    begin.height = data.height;
    begin.debugName = data.debugName.empty() ? nullptr : data.debugName.c_str();
    commands.BeginPass(begin);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);
    commands.BindLayout(recording.ResolveLayout(data.program.value()));
    commands.BindPipeline(recording.Resolve(data.program.value()));
    if (!data.descriptorIndices.empty()) {
        commands.PushConstants(rhi::ShaderStage::All, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(data.descriptorIndices.size()), data.descriptorIndices.data());
    }
    commands.PushConstants(data.constantStage, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, data.constants.data());
    commands.Draw(3, 1, 0, 0);
    commands.EndPass();
}

} // namespace br::render
