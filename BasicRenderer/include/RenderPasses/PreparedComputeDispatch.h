#pragma once

#include "Render/PreparedPass.h"
#include "Render/PipelineState.h"
#include "Render/ShaderAPI.h"
#include "ShaderBuffers.h"

#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace br::render {

inline void BindPreparedDescriptorHeaps(
    rhi::CommandList& commands,
    rhi::DescriptorHeapHandle resourceHeap,
    rhi::DescriptorHeapHandle samplerHeap) {
    // Typed packets normally inherit the execution-slot descriptor snapshots
    // installed by admission.  Explicit heaps remain supported while legacy
    // packets are migrated, but an empty handle must not clear the admitted
    // heaps.
    if (resourceHeap.valid()) {
        commands.SetDescriptorHeaps(resourceHeap,
            samplerHeap.valid()
                ? std::optional<rhi::DescriptorHeapHandle>{samplerHeap}
                : std::nullopt);
    }
}

// Immutable recording packet for the common one-dispatch compute-pass shape.
// Passes capture their frame-varying constants during PrepareFrame; the packet
// owns the pipeline payload and contains no pointer back to the mutable pass.
struct PreparedComputeDispatch {
    rhi::DescriptorHeapHandle resourceHeap{};
    rhi::DescriptorHeapHandle samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    rhi::PipelineHandle pipeline{};
    std::shared_ptr<const org::PipelineStatePayload> pipelineOwner;
    // Preferred typed path: the internal PreparedPass envelope owns and
    // resolves the immutable program generation. Legacy fields above remain
    // temporarily for source-compatible migration.
    std::optional<org::PreparedProgramReference> program;
    std::vector<unsigned int> descriptorIndices;
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    uint32_t groupsX = 0;
    uint32_t groupsY = 1;
    uint32_t groupsZ = 1;
};

inline void RecordPreparedComputeDispatchWithInvocation(
    const PreparedComputeDispatch& data,
    const std::array<unsigned int,NumMiscUintRootConstants>& constants,
    uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ, org::RecordingContext& recording) {
    // Zero work requires neither a program nor command-list mutation.
    if (groupsX == 0 || groupsY == 0 || groupsZ == 0) return;
    auto& commands = recording.Commands();
    BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    commands.BindLayout(data.program ? recording.ResolveLayout(*data.program, data.layout) : data.layout);
    commands.BindPipeline(data.program ? recording.Resolve(*data.program) : data.pipeline);
    if (!data.descriptorIndices.empty()) {
        commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(data.descriptorIndices.size()), data.descriptorIndices.data());
    }
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, constants.data());
    commands.Dispatch(groupsX,groupsY,groupsZ);
}

inline void RecordPreparedComputeDispatch(
    const PreparedComputeDispatch& data, org::RecordingContext& recording) {
    RecordPreparedComputeDispatchWithInvocation(data,data.constants,data.groupsX,data.groupsY,data.groupsZ,recording);
}

inline void RemapDescriptorIndices(PreparedComputeDispatch& data, const org::DescriptorIndexRemap& remap) {
    org::RemapDescriptorIndices(data.descriptorIndices, remap);
}

struct PreparedComputeIndirect {
    bool enabled = true;
    rhi::DescriptorHeapHandle resourceHeap{};
    rhi::DescriptorHeapHandle samplerHeap{};
    std::optional<org::PreparedProgramReference> program;
    rhi::CommandSignatureHandle commandSignature{};
    std::optional<org::PreparedResourceReference> argumentsReference;
    std::optional<org::PreparedResourceReference> countBufferReference;
    std::vector<unsigned int> descriptorIndices;
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    uint64_t argumentsOffset = 0, countOffset = 0;
    uint32_t maximumCount = 1;
};

inline void RecordPreparedComputeIndirect(
    const PreparedComputeIndirect& data, org::RecordingContext& recording) {
    if (!data.enabled) return;
    auto& commands = recording.Commands();
    BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    commands.BindLayout(recording.ResolveLayout(data.program.value()));
    commands.BindPipeline(recording.Resolve(data.program.value()));
    if (!data.descriptorIndices.empty()) {
        commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(data.descriptorIndices.size()), data.descriptorIndices.data());
    }
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, data.constants.data());
    const auto arguments = recording.Resolve(data.argumentsReference.value()).GetHandle();
    const auto countBuffer = data.countBufferReference
        ? recording.Resolve(*data.countBufferReference).GetHandle() : rhi::ResourceHandle{};
    commands.ExecuteIndirect(data.commandSignature, arguments, data.argumentsOffset,
        countBuffer, data.countOffset, data.maximumCount);
}

struct PreparedComputeIndirectSequence {
    struct Step {
        rhi::PipelineHandle pipeline{};
        std::shared_ptr<const org::PipelineStatePayload> pipelineOwner;
        std::optional<org::PreparedProgramReference> program;
        std::vector<unsigned int> descriptorIndices;
        std::array<unsigned int, NumMiscUintRootConstants> constants{};
        uint64_t argumentsOffset = 0;
        uint32_t maximumCount = 1;
    };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    rhi::CommandSignatureHandle commandSignature{};
    std::shared_ptr<const void> commandSignatureOwner;
    rhi::ResourceHandle arguments{}, countBuffer{};
    std::optional<org::PreparedResourceReference> argumentsReference;
    std::optional<org::PreparedResourceReference> countBufferReference;
    std::shared_ptr<const void> argumentsOwner;
    uint64_t countOffset = 0;
    std::vector<Step> steps;
};

inline void RecordPreparedComputeIndirectSequence(
    const PreparedComputeIndirectSequence& data, org::RecordingContext& recording,
    std::optional<std::pair<uint32_t, uint32_t>> constantPatch = {},
    std::span<const uint32_t> invocationConstants = {}, uint64_t invocationMask = 0) {
    if (invocationMask && (std::bit_width(invocationMask) > invocationConstants.size()
        || std::bit_width(invocationMask) > NumMiscUintRootConstants))
        throw std::out_of_range("Indirect sequence invocation constants are incomplete");
    auto& commands = recording.Commands();
    BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    for (const auto& step : data.steps) {
        commands.BindLayout(step.program ? recording.ResolveLayout(*step.program, data.layout) : data.layout);
        commands.BindPipeline(step.program ? recording.Resolve(*step.program) : step.pipeline);
        if (!step.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(step.descriptorIndices.size()), step.descriptorIndices.data());
        auto constants = step.constants;
        for (auto mask = invocationMask; mask; mask &= mask-1) {
            const auto index = std::countr_zero(mask);
            constants[index] = invocationConstants[index];
        }
        if (constantPatch) constants.at(constantPatch->first) = constantPatch->second;
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, constants.data());
        const auto arguments = data.argumentsReference
            ? recording.Resolve(*data.argumentsReference).GetHandle() : data.arguments;
        const auto countBuffer = data.countBufferReference
            ? recording.Resolve(*data.countBufferReference).GetHandle() : data.countBuffer;
        commands.ExecuteIndirect(data.commandSignature, arguments, step.argumentsOffset,
            countBuffer, data.countOffset, step.maximumCount);
    }
}

inline void RemapDescriptorIndices(PreparedComputeIndirectSequence& data, const org::DescriptorIndexRemap& remap) {
    for (auto& step : data.steps) org::RemapDescriptorIndices(step.descriptorIndices, remap);
}

struct PreparedComputeDispatchSequence {
    struct Step {
        std::array<unsigned int, NumMiscUintRootConstants> constants{};
        uint32_t groupsX = 0, groupsY = 1, groupsZ = 1;
        bool uavBarrierBefore = false;
        bool uavBarrierAfter = false;
    };
    rhi::DescriptorHeapHandle resourceHeap{};
    rhi::DescriptorHeapHandle samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    rhi::PipelineHandle pipeline{};
    std::shared_ptr<const org::PipelineStatePayload> pipelineOwner;
    std::optional<org::PreparedProgramReference> program;
    std::vector<unsigned int> descriptorIndices;
    std::vector<Step> steps;
};

inline void RecordPreparedComputeDispatchSequence(
    const PreparedComputeDispatchSequence& data, org::RecordingContext& recording) {
    if (data.steps.empty()) return;
    auto& commands = recording.Commands();
    BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    commands.BindLayout(data.program ? recording.ResolveLayout(*data.program, data.layout) : data.layout);
    commands.BindPipeline(data.program ? recording.Resolve(*data.program) : data.pipeline);
    if (!data.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
        org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
        static_cast<uint32_t>(data.descriptorIndices.size()), data.descriptorIndices.data());
    for (const auto& step : data.steps) {
        if (step.uavBarrierBefore) {
            rhi::GlobalBarrier barrier{};
            barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            rhi::BarrierBatch barriers{}; barriers.globals = {&barrier, 1}; commands.Barriers(barriers);
        }
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, step.constants.data());
        commands.Dispatch(step.groupsX, step.groupsY, step.groupsZ);
        if (step.uavBarrierAfter) {
            rhi::GlobalBarrier barrier{};
            barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            rhi::BarrierBatch barriers{}; barriers.globals = {&barrier, 1}; commands.Barriers(barriers);
        }
    }
}

// Sequence variant for passes which switch immutable compute pipelines between
// dispatches. Each step owns its pipeline payload so recording never reaches
// back into the mutable pass or PSO manager.
struct PreparedComputePipelineSequence {
    struct Step {
        rhi::PipelineHandle pipeline{};
        std::shared_ptr<const org::PipelineStatePayload> pipelineOwner;
        std::optional<org::PreparedProgramReference> program;
        std::vector<unsigned int> descriptorIndices;
        std::array<unsigned int, NumMiscUintRootConstants> constants{};
        uint32_t groupsX = 0, groupsY = 1, groupsZ = 1;
        bool uavBarrierBefore = false;
        bool uavBarrierAfter = false;
    };
    rhi::DescriptorHeapHandle resourceHeap{};
    rhi::DescriptorHeapHandle samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    std::vector<Step> steps;
};

inline void RecordPreparedComputePipelineSequence(
    const PreparedComputePipelineSequence& data, org::RecordingContext& recording) {
    auto& commands = recording.Commands();
    BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    for (const auto& step : data.steps) {
        if (step.uavBarrierBefore) {
            rhi::GlobalBarrier barrier{};
            barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            rhi::BarrierBatch barriers{};
            barriers.globals = {&barrier, 1};
            commands.Barriers(barriers);
        }
        commands.BindLayout(step.program ? recording.ResolveLayout(*step.program, data.layout) : data.layout);
        commands.BindPipeline(step.program ? recording.Resolve(*step.program) : step.pipeline);
        if (!step.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(step.descriptorIndices.size()), step.descriptorIndices.data());
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, step.constants.data());
        if (step.groupsX != 0 && step.groupsY != 0 && step.groupsZ != 0)
            commands.Dispatch(step.groupsX, step.groupsY, step.groupsZ);
        if (step.uavBarrierAfter) {
            rhi::GlobalBarrier barrier{};
            barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            rhi::BarrierBatch barriers{};
            barriers.globals = {&barrier, 1};
            commands.Barriers(barriers);
        }
    }
}

} // namespace br::render
