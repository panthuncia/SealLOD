#include "VirtualGeometry/Streaming/RenderPasses/CLodStreamingFeedbackSortPass.h"
#include "Runtime/StateGraph/InvocationRevision.h"

#include <array>

#include <tracy/Tracy.hpp>

#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodStreamingFeedbackSortRootConstants.h"

CLodStreamingFeedbackSortPass::CLodStreamingFeedbackSortPass(
    std::shared_ptr<org::Buffer> requestKeys,
    std::shared_ptr<org::Buffer> requests,
    std::shared_ptr<org::Buffer> requestCounter,
    std::shared_ptr<org::Buffer> keyScratch,
    std::shared_ptr<org::Buffer> payloadScratch,
    std::shared_ptr<org::Buffer> sumTable,
    std::shared_ptr<org::Buffer> reduceTable,
    std::shared_ptr<org::Buffer> constants,
    std::shared_ptr<org::Buffer> countScatterArgs,
    std::shared_ptr<org::Buffer> reduceScanArgs)
    : m_requestKeys(std::move(requestKeys))
    , m_requests(std::move(requests))
    , m_requestCounter(std::move(requestCounter))
    , m_keyScratch(std::move(keyScratch))
    , m_payloadScratch(std::move(payloadScratch))
    , m_sumTable(std::move(sumTable))
    , m_reduceTable(std::move(reduceTable))
    , m_constants(std::move(constants))
    , m_countScatterArgs(std::move(countScatterArgs))
    , m_reduceScanArgs(std::move(reduceScanArgs)) {
    auto& psoManager = PSOManager::GetInstance();
    const auto computeRootSignature = psoManager.GetComputeRootSignature().GetHandle();
    constexpr const wchar_t* shaderPath = L"Shaders/FidelityFX/ParallelSort/clodStreamingFeedbackSort.hlsl";

    m_setupPso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortSetupCS",
        {},
        "CLod.StreamingFeedbackSort.Setup.PSO");
    m_countPso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortCountCS",
        {},
        "CLod.StreamingFeedbackSort.Count.PSO");
    m_reducePso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortReduceCS",
        {},
        "CLod.StreamingFeedbackSort.Reduce.PSO");
    m_scanPso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortScanCS",
        {},
        "CLod.StreamingFeedbackSort.Scan.PSO");
    m_scanAddPso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortScanAddCS",
        {},
        "CLod.StreamingFeedbackSort.ScanAdd.PSO");
    m_scatterPso = psoManager.MakeComputePipeline(
        computeRootSignature,
        shaderPath,
        L"CLodStreamingFeedbackSortScatterCS",
        {},
        "CLod.StreamingFeedbackSort.Scatter.PSO");
}

StreamingFeedbackSortBindings CLodStreamingFeedbackSortPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    StreamingFeedbackSortBindings bindings;
    bindings.requestCounter = declaration.BindShaderResource(m_requestCounter);
    const std::shared_ptr<org::Buffer> uavs[] = {m_requestKeys, m_requests, m_keyScratch, m_payloadScratch,
        m_sumTable, m_reduceTable, m_constants};
    for (size_t i = 0; i < std::size(uavs); ++i) bindings.uavs[i] = declaration.BindUnorderedAccess(uavs[i]);
    bindings.countScatterUav = declaration.BindUnorderedAccess(m_countScatterArgs);
    bindings.reduceScanUav = declaration.BindUnorderedAccess(m_reduceScanArgs);
    bindings.countScatterIndirect = declaration.BindIndirectArguments(m_countScatterArgs);
    bindings.reduceScanIndirect = declaration.BindIndirectArguments(m_reduceScanArgs);
    return bindings;
}

StreamingFeedbackSortFrameData CLodStreamingFeedbackSortPass::Prepare(
    const StreamingFeedbackSortBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    StreamingFeedbackSortFrameData data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.signature = preparation.CaptureCommandSignature(CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature());
    data.programs = {preparation.CaptureProgramBinding(m_setupPso), preparation.CaptureProgramBinding(m_countPso),
        preparation.CaptureProgramBinding(m_reducePso), preparation.CaptureProgramBinding(m_scanPso),
        preparation.CaptureProgramBinding(m_scanAddPso), preparation.CaptureProgramBinding(m_scatterPso)};
    for (size_t i = 0; i < bindings.uavs.size(); ++i)
        data.uavResources[i] = preparation.CaptureResource(bindings.uavs[i]);
    data.indirectResources = {preparation.CaptureResource(bindings.countScatterIndirect),
        preparation.CaptureResource(bindings.reduceScanIndirect)};
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    auto constants = [&](org::ResourceBindingToken sourceKeys, org::ResourceBindingToken destKeys,
        org::ResourceBindingToken sourcePayloads, org::ResourceBindingToken destPayloads, uint32_t iteration) {
        std::array<unsigned int, NumMiscUintRootConstants> c{};
        c[CLOD_STREAMING_SORT_REQUEST_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.requestCounter);
        c[CLOD_STREAMING_SORT_CONSTANTS_DESCRIPTOR_INDEX] = uav(bindings.uavs[6]);
        c[CLOD_STREAMING_SORT_COUNT_SCATTER_ARGS_DESCRIPTOR_INDEX] = uav(bindings.countScatterUav);
        c[CLOD_STREAMING_SORT_REDUCE_SCAN_ARGS_DESCRIPTOR_INDEX] = uav(bindings.reduceScanUav);
        c[CLOD_STREAMING_SORT_SOURCE_KEYS_DESCRIPTOR_INDEX] = uav(sourceKeys);
        c[CLOD_STREAMING_SORT_DEST_KEYS_DESCRIPTOR_INDEX] = uav(destKeys);
        c[CLOD_STREAMING_SORT_SUM_TABLE_DESCRIPTOR_INDEX] = uav(bindings.uavs[4]);
        c[CLOD_STREAMING_SORT_REDUCE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.uavs[5]);
        c[CLOD_STREAMING_SORT_SOURCE_PAYLOADS_DESCRIPTOR_INDEX] = uav(sourcePayloads);
        c[CLOD_STREAMING_SORT_DEST_PAYLOADS_DESCRIPTOR_INDEX] = uav(destPayloads);
        c[CLOD_STREAMING_SORT_ITERATION_INDEX] = iteration; c[CLOD_STREAMING_SORT_REQUEST_CAPACITY] = CLodStreamingRequestCapacity;
        return c;
    };
    data.constants[0] = constants(bindings.uavs[0], bindings.uavs[2], bindings.uavs[1], bindings.uavs[3], 0);
    data.constants[1] = constants(bindings.uavs[2], bindings.uavs[0], bindings.uavs[3], bindings.uavs[1], 0);
    return data;
}

void CLodStreamingFeedbackSortPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_setupPso));
    out.push_back(br::render::PipelineRevision(m_countPso));
    out.push_back(br::render::PipelineRevision(m_reducePso));
    out.push_back(br::render::PipelineRevision(m_scanPso));
    out.push_back(br::render::PipelineRevision(m_scanAddPso));
    out.push_back(br::render::PipelineRevision(m_scatterPso));
    out.push_back(br::render::OwnerRevision(CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature()));
}

void CLodStreamingFeedbackSortPass::Record(const StreamingFeedbackSortBindings&,
    const StreamingFeedbackSortFrameData& data, org::PassRecordContext& recording) {
    auto& commands = recording.Commands();
    br::render::BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    const auto uavBarrier = [&] {
        std::array<rhi::BufferBarrier, 7> barriers{};
        for (size_t i = 0; i < barriers.size(); ++i) {
            barriers[i].buffer = recording.Resolve(data.uavResources[i]).GetHandle();
            barriers[i].beforeAccess = barriers[i].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            barriers[i].beforeSync = barriers[i].afterSync = rhi::ResourceSyncState::ComputeShading;
        }
        rhi::BarrierBatch batch{};
        batch.buffers = {barriers.data(), static_cast<uint32_t>(barriers.size())};
        commands.Barriers(batch);
    };
    const auto dispatch = [&](size_t programIndex, const auto& constants, int argumentIndex) {
        const auto& binding = data.programs[programIndex];
        commands.BindLayout(recording.ResolveLayout(binding.program));
        commands.BindPipeline(recording.Resolve(binding.program));
        if (!binding.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(binding.descriptorIndices.size()), binding.descriptorIndices.data());
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, constants.data());
        if (argumentIndex < 0) commands.Dispatch(1, 1, 1);
        else commands.ExecuteIndirect(data.signature,
            recording.Resolve(data.indirectResources[argumentIndex]).GetHandle(), 0, {}, 0, 1);
        uavBarrier();
    };
    dispatch(0, data.constants[0], -1);
    std::array<rhi::BufferBarrier, 2> indirectBarriers{};
    for (size_t i = 0; i < indirectBarriers.size(); ++i) {
        auto& barrier = indirectBarriers[i];
        barrier.buffer = recording.Resolve(data.indirectResources[i]).GetHandle();
        barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        barrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
        barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        barrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
    }
    rhi::BarrierBatch batch{};
    batch.buffers = {indirectBarriers.data(), static_cast<uint32_t>(indirectBarriers.size())};
    commands.Barriers(batch);
    for (uint32_t iteration = 0; iteration < 8; ++iteration) {
        auto constants = data.constants[iteration & 1u];
        constants[CLOD_STREAMING_SORT_ITERATION_INDEX] = iteration;
        dispatch(1, constants, 0);
        dispatch(2, constants, 1);
        dispatch(3, constants, -1);
        dispatch(4, constants, 1);
        dispatch(5, constants, 0);
    }
}
