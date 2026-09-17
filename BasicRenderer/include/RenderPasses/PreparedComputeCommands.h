#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <bit>
#include <span>
#include <optional>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <rhi.h>

#include "Render/PreparedPass.h"
#include "Render/PipelineState.h"
#include "Render/RenderGraph/PersistentGraph.h"
#include "Render/ShaderAPI.h"

namespace br::render {

// Persistent templates use tokens; the legacy preparation adapter may still
// supply frame-local references. Both record through the same command encoder.
using PreparedCommandResource = std::variant<org::PreparedResourceReference,org::persistent::BindingToken>;
inline rhi::Resource ResolveCommandResource(const PreparedCommandResource& resource, org::RecordingContext& recording) {
    return std::visit([&](const auto& reference) { return recording.Resolve(reference); },resource);
}

struct PreparedBindComputeProgram {
    org::PreparedProgramReference program;
    std::vector<uint32_t> descriptorIndices;
};
struct PreparedBindPersistentComputeProgram {
    std::shared_ptr<const org::PipelineStatePayload> program;
};
struct PreparedComputeDescriptorIndices { std::vector<uint32_t> values; };
// Immutable logical slots. Physical descriptor indices are selected by the
// frame's publication, without rebuilding commands on backing replacement.
struct PreparedPersistentDescriptorIndices {
    std::vector<std::optional<org::persistent::ViewToken>> slots;
};

struct PreparedConstantViewBinding {
    uint32_t index = 0; // Offset within values, independent of destinationOffset.
    std::optional<org::persistent::ViewToken> view;
};
struct PreparedComputeConstants {
    uint32_t rootParameter = 0;
    uint32_t destinationOffset = 0;
    std::vector<uint32_t> values;
    uint64_t invocationConstantMask = 0;
    std::vector<PreparedConstantViewBinding> bindingViews;
};

inline void ValidateComputeConstantPatches(const PreparedComputeConstants& constants) {
    if (!constants.invocationConstantMask && constants.bindingViews.empty()) return;
    if (constants.values.size() > 64)
        throw std::out_of_range("Compute constant patch exceeds recording storage");
    if (constants.values.size() < 64 && (constants.invocationConstantMask >> constants.values.size()))
        throw std::out_of_range("Invocation constant mask exceeds values");
    auto occupied = constants.invocationConstantMask;
    for (const auto& patch : constants.bindingViews) {
        if (patch.index >= constants.values.size()) throw std::out_of_range("Descriptor constant patch exceeds values");
        const auto bit = uint64_t{1} << patch.index;
        if (occupied & bit) throw std::invalid_argument("Descriptor constant overlaps another patch");
        occupied |= bit;
    }
}

// Descriptor provenance is explicit during command construction. There is no
// numeric conversion: a scalar equal to a descriptor index remains a scalar.
struct SymbolicComputeConstant {
    std::variant<uint32_t,std::optional<org::persistent::ViewToken>> value{uint32_t{0}};
    SymbolicComputeConstant() = default;
    SymbolicComputeConstant(uint32_t scalar) : value(scalar) {}
    SymbolicComputeConstant(org::persistent::ViewToken view) : value(std::optional{view}) {}
    SymbolicComputeConstant(std::nullopt_t) : value(std::optional<org::persistent::ViewToken>{}) {}
};
inline PreparedComputeConstants BuildSymbolicComputeConstants(uint32_t rootParameter, uint32_t destinationOffset,
    std::span<const SymbolicComputeConstant> source, uint64_t invocationMask = 0) {
    PreparedComputeConstants result{rootParameter,destinationOffset,{},invocationMask};
    result.values.resize(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        if (const auto* scalar = std::get_if<uint32_t>(&source[i].value)) result.values[i] = *scalar;
        else {
            if (i >= UINT32_MAX) throw std::out_of_range("Symbolic compute constant index overflow");
            result.bindingViews.push_back({static_cast<uint32_t>(i),std::get<std::optional<org::persistent::ViewToken>>(source[i].value)});
        }
    }
    ValidateComputeConstantPatches(result);
    return result;
}

struct PreparedResourceAddressPatch {
    uint64_t address = 0;
    uint32_t lowIndex = 0;
    uint32_t highIndex = 0;
};

struct PreparedComputeAddressConstants {
    uint32_t rootParameter = 0;
    uint32_t destinationOffset = 0;
    std::vector<uint32_t> values;
    std::vector<PreparedResourceAddressPatch> addresses;
};

struct PreparedDispatchGroups { uint32_t x = 0, y = 1, z = 1; };

struct PreparedBufferBarrier {
    PreparedCommandResource resource;
    rhi::ResourceAccessType beforeAccess = rhi::ResourceAccessType::Common;
    rhi::ResourceAccessType afterAccess = rhi::ResourceAccessType::Common;
    rhi::ResourceSyncState beforeSync = rhi::ResourceSyncState::None;
    rhi::ResourceSyncState afterSync = rhi::ResourceSyncState::None;
};

struct PreparedBufferBarrierBatch { std::vector<PreparedBufferBarrier> barriers; };

struct PreparedExecuteIndirectCommand {
    rhi::CommandSignatureHandle signature{};
    PreparedCommandResource arguments;
    uint64_t argumentOffset = 0;
    uint32_t maxCommandCount = 1;
    std::shared_ptr<const void> signatureOwner;
    std::optional<PreparedCommandResource> countBuffer;
    uint64_t countOffset = 0;
};

struct PreparedSetWorkGraph {
    std::variant<org::PreparedWorkGraphReference,std::shared_ptr<const rhi::WorkGraphPtr>> workGraph;
    PreparedCommandResource backing;
    bool initializeBacking = false;
};

struct PreparedWorkGraphCpuDispatch {
    uint32_t entryPointIndex = 0;
    uint32_t recordStride = 0;
    std::vector<std::byte> records;

    template<class Record>
    static PreparedWorkGraphCpuDispatch From(uint32_t entryPoint, const std::vector<Record>& source) {
        PreparedWorkGraphCpuDispatch result{
            .entryPointIndex = entryPoint,
            .recordStride = static_cast<uint32_t>(sizeof(Record)),
        };
        result.records.resize(source.size() * sizeof(Record));
        if (!source.empty()) std::memcpy(result.records.data(), source.data(), result.records.size());
        return result;
    }
};

struct PreparedWorkGraphGpuDispatch {
    PreparedCommandResource input;
    uint64_t inputAddressOffset = 0;
};

using PreparedComputeCommand = std::variant<
    PreparedBindComputeProgram,
    PreparedBindPersistentComputeProgram,
    PreparedComputeDescriptorIndices,
    PreparedPersistentDescriptorIndices,
    PreparedComputeConstants,
    PreparedComputeAddressConstants,
    PreparedDispatchGroups,
    PreparedBufferBarrierBatch,
    PreparedExecuteIndirectCommand,
    PreparedSetWorkGraph,
    PreparedWorkGraphCpuDispatch,
    PreparedWorkGraphGpuDispatch>;

struct PreparedComputeCommandSequence {
    rhi::PipelineLayoutHandle layout{};
    std::vector<PreparedComputeCommand> commands;
};

inline void RemapDescriptorIndices(PreparedComputeCommandSequence& sequence, const org::DescriptorIndexRemap& remap) {
    for (auto& command : sequence.commands) {
        if (auto* program = std::get_if<PreparedBindComputeProgram>(&command)) org::RemapDescriptorIndices(program->descriptorIndices, remap);
        else if (auto* indices = std::get_if<PreparedComputeDescriptorIndices>(&command)) org::RemapDescriptorIndices(indices->values, remap);
    }
}

// Explicit publication/program-build check, never a frame preparation scan.
// A persistent template must not accidentally retain compiler indices, captured
// descriptor indices, unowned native objects or backing-specific GPU addresses.
inline void ValidatePersistentComputeCommands(const PreparedComputeCommandSequence& sequence) {
    if (sequence.layout.valid()) throw std::invalid_argument("Persistent sequence requires an owned program layout");
    const auto stable = [](const PreparedCommandResource& resource) {
        if (!std::holds_alternative<org::persistent::BindingToken>(resource))
            throw std::invalid_argument("Persistent command contains a frame-local resource reference");
    };
    for (const auto& command : sequence.commands) std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,PreparedBindComputeProgram> || std::is_same_v<T,PreparedComputeDescriptorIndices>)
            throw std::invalid_argument("Persistent command contains frame-local program or descriptor bindings");
        else if constexpr (std::is_same_v<T,PreparedBindPersistentComputeProgram>) {
            if (!value.program || !value.program->pso.Get().GetHandle().valid()
                || !value.program->layout.valid() || !value.program->layoutOwner)
                throw std::invalid_argument("Persistent program has incomplete ownership");
        } else if constexpr (std::is_same_v<T,PreparedBufferBarrierBatch>) {
            for (const auto& barrier : value.barriers) stable(barrier.resource);
        } else if constexpr (std::is_same_v<T,PreparedExecuteIndirectCommand>) {
            stable(value.arguments);
            if (value.countBuffer) stable(*value.countBuffer);
            if (!value.signature.valid() || !value.signatureOwner)
                throw std::invalid_argument("Persistent indirect command has no signature owner");
        } else if constexpr (std::is_same_v<T,PreparedSetWorkGraph>) {
            stable(value.backing);
            const auto* graph = std::get_if<std::shared_ptr<const rhi::WorkGraphPtr>>(&value.workGraph);
            if (!graph || !*graph || !(*graph)->Get().GetHandle().valid())
                throw std::invalid_argument("Persistent command has no work-graph owner");
        } else if constexpr (std::is_same_v<T,PreparedWorkGraphGpuDispatch>) stable(value.input);
        else if constexpr (std::is_same_v<T,PreparedWorkGraphCpuDispatch>) {
            if (!value.records.empty()) throw std::invalid_argument("Persistent command contains invocation records");
        } else if constexpr (std::is_same_v<T,PreparedPersistentDescriptorIndices>) {
            if (value.slots.size() > org::shaderapi::kNumResourceDescriptorIndicesRootConstants)
                throw std::invalid_argument("Persistent descriptor bindings exceed root capacity");
        } else if constexpr (std::is_same_v<T,PreparedComputeConstants>) ValidateComputeConstantPatches(value);
        else if constexpr (std::is_same_v<T,PreparedComputeAddressConstants>) {
            if (!value.addresses.empty()) throw std::invalid_argument("Persistent command contains captured physical addresses");
        }
    },command);
}

// Owner-thread command preparation facade.  Passes describe immutable commands;
// this object owns the otherwise repetitive dependency collection and packet
// finalization needed to make those commands safe to record later or elsewhere.
class PreparedComputeCommandBuilder {
public:
    PreparedComputeCommandBuilder(
        const org::FramePreparationContext& preparation,
        rhi::PipelineLayoutHandle layout)
        : m_dependencies(preparation.dependencyCollector
              ? preparation.dependencyCollector
              : std::make_shared<org::PreparedDependencyCollector>())
        , m_ownsDependencies(!preparation.dependencyCollector)
        , m_preparation(preparation)
        , m_sequence{
            .layout = layout,
        }
    {
        m_preparation.dependencyCollector = m_dependencies;
    }

    [[nodiscard]] org::PreparedResourceReference Capture(
        const std::shared_ptr<Resource>& resource)
    {
        return resource
            ? m_preparation.CaptureResource(resource->GetGlobalResourceID())
            : org::PreparedResourceReference{};
    }

    [[nodiscard]] org::PreparedResourceReference ResourceAt(uint32_t slot) const
    {
        return {slot};
    }

    [[nodiscard]] org::PreparedProgramReference CaptureProgram(
        const PipelineState& pipeline)
    {
        return m_preparation.CaptureProgram(pipeline);
    }

    [[nodiscard]] org::PreparedProgramBinding CaptureProgramBinding(
        const PipelineState& pipeline)
    {
        return m_preparation.CaptureProgramBinding(pipeline);
    }

    [[nodiscard]] org::PreparedProgramBinding CaptureProgramBinding(
        std::shared_ptr<const PipelineStatePayload> payload)
    {
        return m_preparation.CaptureProgramBinding(std::move(payload));
    }

    void Bind(const PipelineState& pipeline)
    {
        auto binding = m_preparation.CaptureProgramBinding(pipeline);
        m_sequence.commands.emplace_back(PreparedBindComputeProgram{
            binding.program, std::move(binding.descriptorIndices)});
    }

    void Bind(const PipelineState& pipeline, std::vector<uint32_t> descriptorIndices)
    {
        m_sequence.commands.emplace_back(PreparedBindComputeProgram{
            CaptureProgram(pipeline), std::move(descriptorIndices)});
    }

    void Constants(
        const uint32_t* values,
        uint32_t count,
        uint32_t rootParameter,
        uint32_t destinationOffset = 0)
    {
        m_sequence.commands.emplace_back(PreparedComputeConstants{
            rootParameter,
            destinationOffset,
            std::vector<uint32_t>(values, values + count)});
    }

    template<size_t Count>
    void Constants(
        const uint32_t (&values)[Count],
        uint32_t rootParameter,
        uint32_t destinationOffset = 0)
    {
        Constants(values, static_cast<uint32_t>(Count), rootParameter, destinationOffset);
    }

    void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1)
    {
        m_sequence.commands.emplace_back(PreparedDispatchGroups{x, y, z});
    }

    void SetWorkGraph(std::shared_ptr<const rhi::WorkGraphPtr> workGraph,
        org::PreparedResourceReference backing, bool initializeBacking)
    {
        m_sequence.commands.emplace_back(PreparedSetWorkGraph{
            m_preparation.CaptureWorkGraph(std::move(workGraph)), backing,
            initializeBacking});
    }

    void Barriers(
        std::initializer_list<std::shared_ptr<Buffer>> resources,
        rhi::ResourceAccessType beforeAccess,
        rhi::ResourceAccessType afterAccess,
        rhi::ResourceSyncState beforeSync = rhi::ResourceSyncState::ComputeShading,
        rhi::ResourceSyncState afterSync = rhi::ResourceSyncState::ComputeShading)
    {
        PreparedBufferBarrierBatch batch;
        batch.barriers.reserve(resources.size());
        for (const auto& resource : resources) {
            if (!resource) continue;
            batch.barriers.push_back({
                Capture(resource), beforeAccess, afterAccess, beforeSync, afterSync});
        }
        if (!batch.barriers.empty())
            m_sequence.commands.emplace_back(std::move(batch));
    }

    template<class T>
    void Retain(std::shared_ptr<T> owner)
    {
        m_preparation.Retain(std::move(owner));
    }

    template<class State>
    void Reserve(
        std::shared_ptr<State> state,
        typename org::PreparedOwnedLifecycle<State>::SubmittedFn submitted = nullptr,
        typename org::PreparedOwnedLifecycle<State>::CompletedFn completed = nullptr,
        typename org::PreparedOwnedLifecycle<State>::AbandonedFn abandoned = nullptr)
    {
        m_preparation.Reserve(
            std::move(state), submitted, completed, abandoned);
    }

    [[nodiscard]] org::FramePreparationContext& Context() { return m_preparation; }
    [[nodiscard]] PreparedComputeCommandSequence& Sequence() { return m_sequence; }
    [[nodiscard]] std::vector<PreparedComputeCommand>& Commands()
    {
        return m_sequence.commands;
    }

    template<class Recorder>
    [[nodiscard]] org::PreparedPass Finish()
    {
        if (!m_ownsDependencies)
            throw std::logic_error("Typed preparation returns command data; it does not finish packets");
        return org::PreparedPass::FromTyped<Recorder>(
            std::move(m_sequence), std::move(*m_dependencies).Freeze());
    }

    // Canonical typed-pass result. Pipeline/resource/lifecycle ownership was
    // collected into the context installed by TypedRenderGraphPass and is
    // frozen by the framework after Prepare returns.
    [[nodiscard]] PreparedComputeCommandSequence FinishData() &&
    {
        return std::move(m_sequence);
    }

private:
    std::shared_ptr<org::PreparedDependencyCollector> m_dependencies;
    bool m_ownsDependencies = false;
    org::FramePreparationContext m_preparation;
    PreparedComputeCommandSequence m_sequence;
};

inline void RecordPreparedComputeCommands(
    const PreparedComputeCommandSequence& data, org::RecordingContext& recording,
    const PreparedWorkGraphCpuDispatch* cpuInvocation = nullptr,
    std::optional<bool> initializeBacking = {},
    std::span<const uint32_t> invocationConstants = {}) {
    auto& commandList = recording.Commands();
    // Descriptor snapshots belong to the execution slot and are installed by
    // admission before any pass records. Frame data must not retain heap
    // handles from its earlier logical-frame preparation.
    if (data.layout.valid()) commandList.BindLayout(data.layout);

    for (const auto& command : data.commands) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, PreparedBindComputeProgram>) {
                commandList.BindPipeline(recording.Resolve(value.program));
                if (!value.descriptorIndices.empty()) {
                    commandList.PushConstants(rhi::ShaderStage::Compute, 0,
                        org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                        static_cast<uint32_t>(value.descriptorIndices.size()),
                        value.descriptorIndices.data());
                }
            } else if constexpr (std::is_same_v<T, PreparedBindPersistentComputeProgram>) {
                if (!value.program || !value.program->pso.Get().GetHandle().valid()
                    || !value.program->layout.valid() || !value.program->layoutOwner)
                    throw std::invalid_argument("Persistent compute program lost its native ownership");
                commandList.BindLayout(value.program->layout);
                commandList.BindPipeline(value.program->pso.Get().GetHandle());
            } else if constexpr (std::is_same_v<T, PreparedComputeDescriptorIndices>) {
                if (!value.values.empty()) commandList.PushConstants(
                    rhi::ShaderStage::Compute, 0,
                    org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                    static_cast<uint32_t>(value.values.size()), value.values.data());
            } else if constexpr (std::is_same_v<T, PreparedPersistentDescriptorIndices>) {
                std::array<uint32_t,org::shaderapi::kNumResourceDescriptorIndicesRootConstants> indices;
                if (value.slots.size() > indices.size())
                    throw std::out_of_range("Persistent descriptor bindings exceed root constant capacity");
                for (size_t i = 0; i != value.slots.size(); ++i)
                    indices[i] = value.slots[i] ? recording.Resolve(*value.slots[i]).index : UINT32_MAX;
                if (!value.slots.empty()) commandList.PushConstants(rhi::ShaderStage::Compute,0,
                    org::shaderapi::kResourceDescriptorIndicesRootParameter,0,
                    static_cast<uint32_t>(value.slots.size()),indices.data());
            } else if constexpr (std::is_same_v<T, PreparedComputeConstants>) {
                const auto* constants = value.values.data();
                std::array<uint32_t, 64> patched;
                if (value.invocationConstantMask || !value.bindingViews.empty()) {
                    ValidateComputeConstantPatches(value);
                    std::copy(value.values.begin(), value.values.end(), patched.begin());
                    for (const auto& patch : value.bindingViews)
                        patched[patch.index] = patch.view ? recording.Resolve(*patch.view).index : UINT32_MAX;
                    auto mask = value.invocationConstantMask;
                    while (mask) {
                        const auto index = static_cast<uint32_t>(std::countr_zero(mask));
                        if (index >= value.values.size() || index >= invocationConstants.size())
                            throw std::out_of_range("Invocation constant patch has no current value");
                        patched[index] = invocationConstants[index];
                        mask &= mask - 1;
                    }
                    constants = patched.data();
                }
                if (!value.values.empty()) commandList.PushConstants(
                    rhi::ShaderStage::Compute, 0, value.rootParameter,
                    value.destinationOffset, static_cast<uint32_t>(value.values.size()),
                    constants);
            } else if constexpr (std::is_same_v<T, PreparedComputeAddressConstants>) {
                auto constants = value.values;
                for (const auto& patch : value.addresses) {
                    constants.at(patch.lowIndex) = static_cast<uint32_t>(patch.address);
                    constants.at(patch.highIndex) = static_cast<uint32_t>(patch.address >> 32u);
                }
                if (!constants.empty()) commandList.PushConstants(
                    rhi::ShaderStage::Compute, 0, value.rootParameter,
                    value.destinationOffset, static_cast<uint32_t>(constants.size()),
                    constants.data());
            } else if constexpr (std::is_same_v<T, PreparedDispatchGroups>) {
                if (value.x) commandList.Dispatch(value.x, value.y, value.z);
            } else if constexpr (std::is_same_v<T, PreparedBufferBarrierBatch>) {
                std::vector<rhi::BufferBarrier> barriers;
                barriers.reserve(value.barriers.size());
                for (const auto& source : value.barriers) {
                    rhi::BufferBarrier barrier{};
                    barrier.buffer = ResolveCommandResource(source.resource,recording).GetHandle();
                    barrier.beforeAccess = source.beforeAccess;
                    barrier.afterAccess = source.afterAccess;
                    barrier.beforeSync = source.beforeSync;
                    barrier.afterSync = source.afterSync;
                    barriers.push_back(barrier);
                }
                if (!barriers.empty()) {
                    rhi::BarrierBatch batch{};
                    batch.buffers = rhi::Span<rhi::BufferBarrier>(
                        barriers.data(), static_cast<uint32_t>(barriers.size()));
                    commandList.Barriers(batch);
                }
            } else if constexpr (std::is_same_v<T, PreparedExecuteIndirectCommand>) {
                commandList.ExecuteIndirect(value.signature,
                    ResolveCommandResource(value.arguments,recording).GetHandle(), value.argumentOffset,
                    value.countBuffer ? ResolveCommandResource(*value.countBuffer,recording).GetHandle() : rhi::ResourceHandle{},
                    value.countOffset, value.maxCommandCount);
            } else if constexpr (std::is_same_v<T, PreparedSetWorkGraph>) {
                const auto workGraph = std::visit([&](const auto& reference) {
                    using Reference = std::decay_t<decltype(reference)>;
                    if constexpr (std::is_same_v<Reference,org::PreparedWorkGraphReference>) return recording.Resolve(reference);
                    else {
                        if (!reference || !reference->Get().GetHandle().valid())
                            throw std::invalid_argument("Persistent work graph lost its native ownership");
                        return reference->Get().GetHandle();
                    }
                },value.workGraph);
                commandList.SetWorkGraph(workGraph,
                    ResolveCommandResource(value.backing,recording).GetHandle(), initializeBacking.value_or(value.initializeBacking));
            } else if constexpr (std::is_same_v<T, PreparedWorkGraphCpuDispatch>) {
                const auto& input = cpuInvocation ? *cpuInvocation : value;
                if (input.records.empty()) return;
                rhi::WorkGraphDispatchDesc dispatch{};
                dispatch.dispatchMode = rhi::WorkGraphDispatchMode::NodeCpuInput;
                dispatch.nodeCpuInput.entryPointIndex = input.entryPointIndex;
                dispatch.nodeCpuInput.pRecords = input.records.data();
                dispatch.nodeCpuInput.numRecords = static_cast<uint32_t>(
                    input.records.size() / input.recordStride);
                dispatch.nodeCpuInput.recordByteStride = input.recordStride;
                commandList.DispatchWorkGraph(dispatch);
            } else if constexpr (std::is_same_v<T, PreparedWorkGraphGpuDispatch>) {
                rhi::WorkGraphDispatchDesc dispatch{};
                dispatch.dispatchMode = rhi::WorkGraphDispatchMode::MultiNodeGpuInput;
                dispatch.multiNodeGpuInput.inputBuffer = ResolveCommandResource(value.input,recording).GetHandle();
                dispatch.multiNodeGpuInput.inputAddressOffset = value.inputAddressOffset;
                commandList.DispatchWorkGraph(dispatch);
            }
        }, command);
    }
}

} // namespace br::render
