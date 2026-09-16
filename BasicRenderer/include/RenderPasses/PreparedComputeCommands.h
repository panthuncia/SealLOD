#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <rhi.h>

#include "Render/PreparedPass.h"
#include "Render/ShaderAPI.h"

namespace br::render {

struct PreparedBindComputeProgram {
    org::PreparedProgramReference program;
    std::vector<uint32_t> descriptorIndices;
};
struct PreparedComputeDescriptorIndices { std::vector<uint32_t> values; };

struct PreparedComputeConstants {
    uint32_t rootParameter = 0;
    uint32_t destinationOffset = 0;
    std::vector<uint32_t> values;
};

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
    org::PreparedResourceReference resource;
    rhi::ResourceAccessType beforeAccess = rhi::ResourceAccessType::Common;
    rhi::ResourceAccessType afterAccess = rhi::ResourceAccessType::Common;
    rhi::ResourceSyncState beforeSync = rhi::ResourceSyncState::None;
    rhi::ResourceSyncState afterSync = rhi::ResourceSyncState::None;
};

struct PreparedBufferBarrierBatch { std::vector<PreparedBufferBarrier> barriers; };

struct PreparedExecuteIndirectCommand {
    rhi::CommandSignatureHandle signature{};
    org::PreparedResourceReference arguments;
    uint64_t argumentOffset = 0;
    uint32_t maxCommandCount = 1;
};

struct PreparedSetWorkGraph {
    org::PreparedWorkGraphReference workGraph;
    org::PreparedResourceReference backing;
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
    org::PreparedResourceReference input;
    uint64_t inputAddressOffset = 0;
};

using PreparedComputeCommand = std::variant<
    PreparedBindComputeProgram,
    PreparedComputeDescriptorIndices,
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
    std::optional<bool> initializeBacking = {}) {
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
            } else if constexpr (std::is_same_v<T, PreparedComputeDescriptorIndices>) {
                if (!value.values.empty()) commandList.PushConstants(
                    rhi::ShaderStage::Compute, 0,
                    org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                    static_cast<uint32_t>(value.values.size()), value.values.data());
            } else if constexpr (std::is_same_v<T, PreparedComputeConstants>) {
                if (!value.values.empty()) commandList.PushConstants(
                    rhi::ShaderStage::Compute, 0, value.rootParameter,
                    value.destinationOffset, static_cast<uint32_t>(value.values.size()),
                    value.values.data());
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
                    barrier.buffer = recording.Resolve(source.resource).GetHandle();
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
                    recording.Resolve(value.arguments).GetHandle(), value.argumentOffset,
                    {}, 0, value.maxCommandCount);
            } else if constexpr (std::is_same_v<T, PreparedSetWorkGraph>) {
                commandList.SetWorkGraph(recording.Resolve(value.workGraph),
                    recording.Resolve(value.backing).GetHandle(), initializeBacking.value_or(value.initializeBacking));
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
                dispatch.multiNodeGpuInput.inputBuffer = recording.Resolve(value.input).GetHandle();
                dispatch.multiNodeGpuInput.inputAddressOffset = value.inputAddressOffset;
                commandList.DispatchWorkGraph(dispatch);
            }
        }, command);
    }
}

} // namespace br::render
