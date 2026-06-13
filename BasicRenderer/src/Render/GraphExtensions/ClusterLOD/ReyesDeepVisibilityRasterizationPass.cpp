#include "Render/GraphExtensions/ClusterLOD/ReyesDeepVisibilityRasterizationPass.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesDeepVisibilityRasterRootConstants.h"
#include "../shaders/PerPassRootConstants/clodReyesPatchRasterRootConstants.h"

namespace {
constexpr uint32_t kDeepVisibilityAverageFragmentsPerPixel = 5u;
}

ReyesDeepVisibilityRasterizationPass::ReyesDeepVisibilityRasterizationPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> rasterWorkBuffer,
    std::shared_ptr<Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> tessTableVerticesBuffer,
    std::shared_ptr<Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<Buffer> deepVisibilityNodesBuffer,
    std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    std::string_view resourceName,
    uint32_t patchVisibilityIndexBase)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_deepVisibilityNodesBuffer(std::move(deepVisibilityNodesBuffer))
    , m_deepVisibilityCounterBuffer(std::move(deepVisibilityCounterBuffer))
    , m_deepVisibilityOverflowCounterBuffer(std::move(deepVisibilityOverflowCounterBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_patchVisibilityIndexBase(patchVisibilityIndexBase) {
    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName(std::string(resourceName));
    rg::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes deep visibility");

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesPatchDeepVisibilityRaster.hlsl",
        L"ReyesPatchDeepVisibilityRasterCS",
        {},
        "CLod.ReyesPatchDeepVisibilityRaster.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesDeepVisibilityRasterizationPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_visibleClustersBuffer,
            m_diceQueueBuffer,
            m_diceQueueCounterBuffer,
            m_rasterWorkBuffer,
            m_rasterWorkCounterBuffer,
            m_tessTableConfigsBuffer,
            m_tessTableVerticesBuffer,
            m_tessTableTrianglesBuffer,
            m_viewRasterInfoBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtHeightAtlas,
            Builtin::Terrain::RvtAlbedoAtlas,
            Builtin::Terrain::RvtNormalAtlas,
            Builtin::Terrain::RvtMaterialAtlas,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
		.WithUnorderedAccess(
            Builtin::Material::TextureStreamingFeedbackBuffer,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_telemetryBuffer,
            m_deepVisibilityNodesBuffer,
            m_deepVisibilityCounterBuffer,
            m_deepVisibilityOverflowCounterBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer);

    for (const auto& visibilityBuffer : m_visibilityBuffers) {
        builder->WithShaderResource(visibilityBuffer);
    }
    for (const auto& headPointers : m_deepVisibilityHeadPointerBuffers) {
        builder->WithUnorderedAccess(headPointers);
    }
    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
}

void ReyesDeepVisibilityRasterizationPass::Setup()
{
}

void ReyesDeepVisibilityRasterizationPass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    const auto numViews = context.viewManager->GetCameraBufferSize();
    std::vector<std::shared_ptr<PixelBuffer>> visibilityBuffers;
    std::vector<std::shared_ptr<PixelBuffer>> deepVisibilityHeadPointerBuffers;

    uint32_t maxViewWidth = 1u;
    uint32_t maxViewHeight = 1u;
    uint64_t totalViewPixels = 0u;

    context.viewManager->ForEachView([&](uint64_t viewID) {
        const auto* viewInfo = context.viewManager->Get(viewID);
        if (!viewInfo || !viewInfo->gpu.visibilityBuffer) {
            return;
        }

        auto headPointers = context.viewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
        if (!headPointers) {
            return;
        }

        maxViewWidth = std::max(maxViewWidth, headPointers->GetWidth());
        maxViewHeight = std::max(maxViewHeight, headPointers->GetHeight());
        totalViewPixels += static_cast<uint64_t>(headPointers->GetWidth()) *
            static_cast<uint64_t>(headPointers->GetHeight());
    });

    std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);
    context.viewManager->ForEachView([&](uint64_t viewID) {
        const auto* viewInfo = context.viewManager->Get(viewID);
        if (!viewInfo || !viewInfo->gpu.visibilityBuffer) {
            return;
        }

        const auto cameraIndex = viewInfo->gpu.cameraBufferIndex;
        CLodViewRasterInfo info{};
        info.scissorMinX = 0u;
        info.scissorMinY = 0u;

        auto headPointers = context.viewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
        if (!headPointers) {
            viewRasterInfo[cameraIndex] = info;
            return;
        }

        info.opaqueVisibilitySRVDescriptorIndex = viewInfo->gpu.visibilityBuffer->GetSRVInfo(0).slot.index;
        info.deepVisibilityHeadPointerUAVDescriptorIndex = headPointers->GetUAVShaderVisibleInfo(0).slot.index;
        info.scissorMaxX = headPointers->GetWidth();
        info.scissorMaxY = headPointers->GetHeight();
        info.viewportScaleX = static_cast<float>(info.scissorMaxX) / static_cast<float>(maxViewWidth);
        info.viewportScaleY = static_cast<float>(info.scissorMaxY) / static_cast<float>(maxViewHeight);

        visibilityBuffers.push_back(viewInfo->gpu.visibilityBuffer);
        deepVisibilityHeadPointerBuffers.push_back(std::move(headPointers));
        viewRasterInfo[cameraIndex] = info;
    });

    const uint64_t maxNodes = totalViewPixels * kDeepVisibilityAverageFragmentsPerPixel;
    m_deepVisibilityNodeCapacity = std::max<uint32_t>(
        1u,
        static_cast<uint32_t>(std::min<uint64_t>(maxNodes, std::numeric_limits<uint32_t>::max())));
    if (m_deepVisibilityNodesBuffer) {
        m_deepVisibilityNodesBuffer->ResizeStructured(m_deepVisibilityNodeCapacity);
    }

    const bool resourcesChanged =
        (m_visibilityBuffers != visibilityBuffers) ||
        (m_deepVisibilityHeadPointerBuffers != deepVisibilityHeadPointerBuffers);

    m_visibilityBuffers = std::move(visibilityBuffers);
    m_deepVisibilityHeadPointerBuffers = std::move(deepVisibilityHeadPointerBuffers);

    if (m_viewRasterInfos != viewRasterInfo || resourcesChanged) {
        m_viewRasterInfos = std::move(viewRasterInfo);
        m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_viewRasterInfos.size()));
        BUFFER_UPLOAD(
            m_viewRasterInfos.data(),
            static_cast<uint32_t>(m_viewRasterInfos.size() * sizeof(CLodViewRasterInfo)),
            rg::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
            0);
        m_declaredResourcesChanged = true;
    }
    else {
        m_declaredResourcesChanged = false;
    }
}

bool ReyesDeepVisibilityRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

PassReturn ReyesDeepVisibilityRasterizationPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = m_rasterWorkBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX] = m_rasterWorkCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_tessTableVerticesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_tessTableTrianglesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_BUFFER_DESCRIPTOR_INDEX] = m_deepVisibilityNodesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_COUNTER_DESCRIPTOR_INDEX] = m_deepVisibilityCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_DEEP_VISIBILITY_RASTER_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = m_deepVisibilityOverflowCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_CAPACITY] = m_deepVisibilityNodeCapacity;

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

void ReyesDeepVisibilityRasterizationPass::Cleanup()
{
}
