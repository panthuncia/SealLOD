#include "VirtualGeometry/Streaming/RenderPasses/CLodStreamingFeedbackSortPass.h"

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
    bindings.pipelines = {m_setupPso, m_countPso, m_reducePso, m_scanPso, m_scanAddPso, m_scatterPso};
    bindings.requestCounter = declaration.ShaderResource(m_requestCounter);
    const std::shared_ptr<org::Buffer> uavs[] = {m_requestKeys, m_requests, m_keyScratch, m_payloadScratch,
        m_sumTable, m_reduceTable, m_constants};
    for (size_t i = 0; i < std::size(uavs); ++i) bindings.uavs[i] = declaration.UnorderedAccess(uavs[i]);
    bindings.countScatterUav = declaration.UnorderedAccess(m_countScatterArgs);
    bindings.reduceScanUav = declaration.UnorderedAccess(m_reduceScanArgs);
    bindings.countScatterIndirect = declaration.IndirectArguments(m_countScatterArgs);
    bindings.reduceScanIndirect = declaration.IndirectArguments(m_reduceScanArgs);
    return bindings;
}

StreamingFeedbackSortFrameData CLodStreamingFeedbackSortPass::Prepare(
    const StreamingFeedbackSortBindings&, const org::PassPrepareContext&) const {
    return {CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature()};
}

void CLodStreamingFeedbackSortPass::Record(const StreamingFeedbackSortBindings& bindings,
    const StreamingFeedbackSortFrameData& frame, org::PassRecordContext& context) {
    const auto& uavs = bindings.uavs;
    const auto constants = [&](const org::DeclaredViewToken& sourceKeys, const org::DeclaredViewToken& destKeys,
        const org::DeclaredViewToken& sourcePayloads, const org::DeclaredViewToken& destPayloads) {
        br::render::MiscConstants c{};
        c[CLOD_STREAMING_SORT_REQUEST_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.requestCounter);
        c[CLOD_STREAMING_SORT_CONSTANTS_DESCRIPTOR_INDEX] = context.Index(uavs[6]);
        c[CLOD_STREAMING_SORT_COUNT_SCATTER_ARGS_DESCRIPTOR_INDEX] = context.Index(bindings.countScatterUav);
        c[CLOD_STREAMING_SORT_REDUCE_SCAN_ARGS_DESCRIPTOR_INDEX] = context.Index(bindings.reduceScanUav);
        c[CLOD_STREAMING_SORT_SOURCE_KEYS_DESCRIPTOR_INDEX] = context.Index(sourceKeys);
        c[CLOD_STREAMING_SORT_DEST_KEYS_DESCRIPTOR_INDEX] = context.Index(destKeys);
        c[CLOD_STREAMING_SORT_SUM_TABLE_DESCRIPTOR_INDEX] = context.Index(uavs[4]);
        c[CLOD_STREAMING_SORT_REDUCE_TABLE_DESCRIPTOR_INDEX] = context.Index(uavs[5]);
        c[CLOD_STREAMING_SORT_SOURCE_PAYLOADS_DESCRIPTOR_INDEX] = context.Index(sourcePayloads);
        c[CLOD_STREAMING_SORT_DEST_PAYLOADS_DESCRIPTOR_INDEX] = context.Index(destPayloads);
        c[CLOD_STREAMING_SORT_ITERATION_INDEX] = 0;
        c[CLOD_STREAMING_SORT_REQUEST_CAPACITY] = CLodStreamingRequestCapacity;
        return c;
    };
    // Even iterations sort from the request buffers into scratch, odd ones back.
    const std::array<br::render::MiscConstants, 2> pingPong{
        constants(uavs[0], uavs[2], uavs[1], uavs[3]),
        constants(uavs[2], uavs[0], uavs[3], uavs[1])};
    const auto uavBarrier = [&] {
        context.BufferBarriers({uavs[0].Resource(), uavs[1].Resource(), uavs[2].Resource(), uavs[3].Resource(),
            uavs[4].Resource(), uavs[5].Resource(), uavs[6].Resource()},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
    };
    const std::array<org::ResourceBindingToken, 2> indirect{bindings.countScatterIndirect, bindings.reduceScanIndirect};
    const auto dispatch = [&](size_t pipelineIndex, const br::render::MiscConstants& c, int argumentIndex) {
        context.BindComputePipeline(bindings.pipelines[pipelineIndex]);
        br::render::PushMiscConstants(context, c);
        if (argumentIndex < 0) context.Commands().Dispatch(1, 1, 1);
        else context.ExecuteIndirect(frame.signature, indirect[argumentIndex]);
        uavBarrier();
    };
    dispatch(0, pingPong[0], -1);
    context.BufferBarriers({bindings.countScatterIndirect, bindings.reduceScanIndirect},
        rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
    for (uint32_t iteration = 0; iteration < 8; ++iteration) {
        auto c = pingPong[iteration & 1u];
        c[CLOD_STREAMING_SORT_ITERATION_INDEX] = iteration;
        dispatch(1, c, 0);
        dispatch(2, c, 1);
        dispatch(3, c, -1);
        dispatch(4, c, 1);
        dispatch(5, c, 0);
    }
}
