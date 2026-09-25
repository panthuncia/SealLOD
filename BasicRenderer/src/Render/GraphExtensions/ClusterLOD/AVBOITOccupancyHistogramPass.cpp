#include "Render/GraphExtensions/ClusterLOD/AVBOITOccupancyHistogramPass.h"

#include <array>

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
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
    return {
        builder.BindShaderResource(m_configBuffer),
        builder.BindUnorderedAccess(m_occupancyTexture),
        builder.BindUnorderedAccess(m_occupancySliceMaskTexture),
        builder.BindUnorderedAccess(m_occupancyHistogramBuffer) };
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

br::render::PreparedComputeDispatch AVBOITOccupancyHistogramPass::Prepare(
    const AVBOITOccupancyHistogramBindings& bindings,
    const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture || !m_occupancyHistogramBuffer) {
        return {};
    }

    const auto* renderContext = preparation.preparationData->Get<UpdateContext>();
    auto& context = *renderContext;

    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);

    auto& misc = data.constants;
    misc[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_CONFIG_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_OCCUPANCY_HISTOGRAM_BUFFER_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.histogram, {org::BindlessViewKind::UnorderedAccess}).index;

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    data.groupsX = groupCountX; data.groupsY = groupCountY; data.groupsZ = 1u;
    return data;
}

void AVBOITOccupancyHistogramPass::Record(const AVBOITOccupancyHistogramBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
