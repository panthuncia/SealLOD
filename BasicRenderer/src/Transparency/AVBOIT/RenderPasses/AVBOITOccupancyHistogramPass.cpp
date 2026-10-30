#include "Transparency/AVBOIT/RenderPasses/AVBOITOccupancyHistogramPass.h"
#include "Render/PassBuilders.h"

#include <array>

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITOccupancyHistogramRootConstants.h"

AVBOITOccupancyHistogramPass::AVBOITOccupancyHistogramPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::PixelBuffer> occupancyTexture,
    std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<org::Buffer> occupancyHistogramBuffer)
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

AVBOITOccupancyHistogramBindings AVBOITOccupancyHistogramPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, 
        builder.ShaderResource(m_configBuffer),
        builder.UnorderedAccess(m_occupancyTexture),
        builder.UnorderedAccess(m_occupancySliceMaskTexture),
        builder.UnorderedAccess(m_occupancyHistogramBuffer) };
}

void AVBOITOccupancyHistogramPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    if (!m_occupancyHistogramBuffer) {
        return;
    }

    const std::array<uint32_t, CLodAVBOITDefaultVirtualSliceCount> zeroHistogram{};
    UploadBufferData(
        zeroHistogram.data(),
        sizeof(zeroHistogram),
        org::runtime::UploadTarget::FromShared(m_occupancyHistogramBuffer),
        0);
}

AVBOITOccupancyHistogramFrame AVBOITOccupancyHistogramPass::Prepare(
    const AVBOITOccupancyHistogramBindings& bindings,
    const org::PassPrepareContext& preparation) const {
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture || !m_occupancyHistogramBuffer) {
        return {};
    }

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    return {groupCountX, groupCountY};
}

void AVBOITOccupancyHistogramPass::Record(const AVBOITOccupancyHistogramBindings& bindings, const AVBOITOccupancyHistogramFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
