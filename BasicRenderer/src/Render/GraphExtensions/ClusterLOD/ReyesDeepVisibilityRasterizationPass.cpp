#include "Render/GraphExtensions/ClusterLOD/ReyesDeepVisibilityRasterizationPass.h"
#include "Render/InvocationRevision.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Render/TerrainRvtTelemetry.h"
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
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
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
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
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
    org::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes deep visibility");

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesPatchDeepVisibilityRaster.hlsl",
        L"ReyesPatchDeepVisibilityRasterCS",
        IsTerrainRvtTelemetryDebugEnabled() ? std::vector<DxcDefine>{ DxcDefine{ L"TERRAIN_RVT_TELEMETRY", L"1" } } : std::vector<DxcDefine>{},
        "CLod.ReyesPatchDeepVisibilityRaster.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_commandSignature);
}

ReyesDeepVisibilityRasterBindings ReyesDeepVisibilityRasterizationPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    builder->WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            "Builtin::PerMaterialEvalDataBuffer",
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtPhysicalPageAtlas,
            Builtin::Terrain::RvtHeightResidentCache,
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
        .WithConstantBuffer(Builtin::PerFrameBuffer);

    ReyesDeepVisibilityRasterBindings bindings{
        builder->BindShaderResource(m_visibleClustersBuffer), builder->BindShaderResource(m_visibleClusterTransformIndicesBuffer),
        builder->BindShaderResource(m_diceQueueBuffer), builder->BindShaderResource(m_diceQueueCounterBuffer),
        builder->BindShaderResource(m_rasterWorkBuffer), builder->BindShaderResource(m_rasterWorkCounterBuffer),
        builder->BindShaderResource(m_tessTableConfigsBuffer), builder->BindShaderResource(m_tessTableVerticesBuffer),
        builder->BindShaderResource(m_tessTableTrianglesBuffer), builder->BindIndirectArguments(m_indirectArgsBuffer),
        builder->BindUnorderedAccess(m_telemetryBuffer), builder->BindShaderResource(m_viewRasterInfoBuffer),
        builder->BindUnorderedAccess(m_deepVisibilityNodesBuffer), builder->BindUnorderedAccess(m_deepVisibilityCounterBuffer),
        builder->BindUnorderedAccess(m_deepVisibilityOverflowCounterBuffer)};
    for (const auto& visibilityBuffer : m_visibilityBuffers) {
        bindings.visibilityBuffers.push_back(builder->BindShaderResource(visibilityBuffer));
    }
    for (const auto& headPointers : m_deepVisibilityHeadPointerBuffers) {
        bindings.headPointerBuffers.push_back(builder->BindUnorderedAccess(headPointers));
    }
    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
    bindings.nodeCapacity = m_deepVisibilityNodeCapacity;
    return bindings;
}

void ReyesDeepVisibilityRasterizationPass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    const auto numViews = context.ViewCameraBufferSize();
    std::vector<std::shared_ptr<PixelBuffer>> visibilityBuffers;
    std::vector<std::shared_ptr<PixelBuffer>> deepVisibilityHeadPointerBuffers;

    uint32_t maxViewWidth = 1u;
    uint32_t maxViewHeight = 1u;
    uint64_t totalViewPixels = 0u;

    for (const auto& viewInfo : context.Views()) {
        if (!viewInfo.visibilityBuffer) {
            continue;
        }

        auto headPointers = viewInfo.deepVisibilityHeadPointers;
        if (!headPointers) {
            continue;
        }

        maxViewWidth = std::max(maxViewWidth, headPointers->GetWidth());
        maxViewHeight = std::max(maxViewHeight, headPointers->GetHeight());
        totalViewPixels += static_cast<uint64_t>(headPointers->GetWidth()) *
            static_cast<uint64_t>(headPointers->GetHeight());
    }

    std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);
    for (const auto& viewInfo : context.Views()) {
        if (!viewInfo.visibilityBuffer) {
            continue;
        }

        const auto cameraIndex = viewInfo.cameraBufferIndex;
        CLodViewRasterInfo info{};
        info.scissorMinX = 0u;
        info.scissorMinY = 0u;

        auto headPointers = viewInfo.deepVisibilityHeadPointers;
        if (!headPointers) {
            viewRasterInfo[cameraIndex] = info;
            continue;
        }

        info.opaqueVisibilitySRVDescriptorIndex = viewInfo.visibilitySRVIndex;
        info.deepVisibilityHeadPointerUAVDescriptorIndex =
            viewInfo.deepVisibilityHeadPointersUAVIndex;
        info.scissorMaxX = headPointers->GetWidth();
        info.scissorMaxY = headPointers->GetHeight();
        info.viewportScaleX = static_cast<float>(info.scissorMaxX) / static_cast<float>(maxViewWidth);
        info.viewportScaleY = static_cast<float>(info.scissorMaxY) / static_cast<float>(maxViewHeight);

        visibilityBuffers.push_back(viewInfo.visibilityBuffer);
        deepVisibilityHeadPointerBuffers.push_back(std::move(headPointers));
        viewRasterInfo[cameraIndex] = info;
    }

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
        UploadBufferData(
            m_viewRasterInfos.data(),
            static_cast<uint32_t>(m_viewRasterInfos.size() * sizeof(CLodViewRasterInfo)),
            org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
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

br::render::PreparedComputeIndirect ReyesDeepVisibilityRasterizationPass::Prepare(
    const ReyesDeepVisibilityRasterBindings& bindings, const org::PassPrepareContext& preparation) const
{
    br::render::PreparedComputeIndirect data{};
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    auto binding = preparation.CaptureProgramBinding(m_pso);
    data.program = binding.program;
    data.descriptorIndices = std::move(binding.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        srv(bindings.transforms);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.diceCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.work);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    data.constants[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX] = srv(bindings.viewInfo);
    data.constants[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.workCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.tessVertices);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.tessTriangles);
    data.constants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.nodes);
    data.constants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.nodeCounter);
    data.constants[CLOD_REYES_DEEP_VISIBILITY_RASTER_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.overflowCounter);
    data.constants[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_CAPACITY] = bindings.nodeCapacity;

    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    return data;
}

void ReyesDeepVisibilityRasterizationPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesDeepVisibilityRasterizationPass::Record(const ReyesDeepVisibilityRasterBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
