#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "BuiltinResources.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"

ReyesBuildRasterWorkPass::ReyesBuildRasterWorkPass(
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> diceQueueReadOffsetBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> rasterWorkBuffer,
    std::shared_ptr<Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t rasterWorkCapacity)
    : m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_diceQueueReadOffsetBuffer(std::move(diceQueueReadOffsetBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_rasterWorkCapacity(rasterWorkCapacity) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesBuildRasterWork.hlsl",
        L"BuildReyesRasterWorkCS",
        {},
        "CLod.ReyesBuildRasterWork.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesBuildRasterWorkPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_diceQueueBuffer,
            m_diceQueueCounterBuffer,
            m_tessTableConfigsBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_rasterWorkBuffer,
            m_rasterWorkCounterBuffer,
            m_telemetryBuffer);
    if (m_diceQueueReadOffsetBuffer) {
        builder->WithShaderResource(m_diceQueueReadOffsetBuffer);
    }
}

void ReyesBuildRasterWorkPass::Setup() {}

void ReyesBuildRasterWorkPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    const uint32_t zero = 0u;
    BUFFER_UPLOAD(&zero, sizeof(uint32_t), rg::runtime::UploadTarget::FromShared(m_rasterWorkCounterBuffer), 0);
}

PassReturn ReyesBuildRasterWorkPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = m_diceQueueReadOffsetBuffer
        ? m_diceQueueReadOffsetBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_DESCRIPTOR_INDEX] = m_rasterWorkBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_COUNTER_DESCRIPTOR_INDEX] = m_rasterWorkCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_CAPACITY] = m_rasterWorkCapacity;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    commandList.ExecuteIndirect(m_commandSignature->GetHandle(), m_indirectArgsBuffer->GetAPIResource().GetHandle(), 0, {}, 0, 1);
    return {};
}

void ReyesBuildRasterWorkPass::Cleanup() {}
