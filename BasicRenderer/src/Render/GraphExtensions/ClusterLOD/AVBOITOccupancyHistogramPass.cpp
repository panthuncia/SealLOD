#include "Render/GraphExtensions/ClusterLOD/AVBOITOccupancyHistogramPass.h"

#include <array>

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITOccupancyHistogramRootConstants.h"

AVBOITOccupancyHistogramPass::AVBOITOccupancyHistogramPass(
    std::shared_ptr<Buffer> configBuffer,
    std::shared_ptr<PixelBuffer> occupancyTexture,
    std::shared_ptr<PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<Buffer> occupancyHistogramBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_occupancyTexture(std::move(occupancyTexture))
    , m_occupancySliceMaskTexture(std::move(occupancySliceMaskTexture))
    , m_occupancyHistogramBuffer(std::move(occupancyHistogramBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITOccupancyHistogram.hlsl",
        L"CLodAVBOITOccupancyHistogramCS",
        {},
        "CLod.AVBOITOccupancyHistogram.PSO");
}

void AVBOITOccupancyHistogramPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_configBuffer)
        .WithUnorderedAccess(
            m_occupancyTexture,
            m_occupancySliceMaskTexture,
            m_occupancyHistogramBuffer);
}

void AVBOITOccupancyHistogramPass::Setup()
{
}

void AVBOITOccupancyHistogramPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    if (!m_occupancyHistogramBuffer) {
        return;
    }

    const std::array<uint32_t, CLodAVBOITDefaultVirtualSliceCount> zeroHistogram{};
    BUFFER_UPLOAD(
        zeroHistogram.data(),
        sizeof(zeroHistogram),
        org::runtime::UploadTarget::FromShared(m_occupancyHistogramBuffer),
        0);
}

PassReturn AVBOITOccupancyHistogramPass::Execute(PassExecutionContext& executionContext)
{
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture || !m_occupancyHistogramBuffer) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_CONFIG_DESCRIPTOR_INDEX] = m_configBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_BUFFER_DESCRIPTOR_INDEX] =
        m_occupancyHistogramBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        misc);

    const uint32_t groupCountX = (m_occupancyTexture->GetWidth() + 7u) / 8u;
    const uint32_t groupCountY = (m_occupancyTexture->GetHeight() + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    commandList.Dispatch(groupCountX, groupCountY, 1u);
    return {};
}

void AVBOITOccupancyHistogramPass::Cleanup()
{
}