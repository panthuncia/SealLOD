#include "Render/GraphExtensions/ClusterLOD/ReyesPatchRasterizationPass.h"
#include "Render/InvocationRevision.h"

#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/ObjectReyesAtlasTelemetry.h"
#include "Render/TerrainRvtTelemetry.h"
#include "BuiltinResources.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesPatchRasterRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "RenderPasses/PreparedComputeDispatch.h"

ReyesPatchRasterizationPass::ReyesPatchRasterizationPass(
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
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    uint32_t maxDiceQueueEntries,
    uint32_t phaseIndex,
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
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_maxDiceQueueEntries(maxDiceQueueEntries)
    , m_phaseIndex(phaseIndex)
    , m_patchVisibilityIndexBase(patchVisibilityIndexBase) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesPatchRaster.hlsl",
        L"ReyesPatchRasterCS",
        [] {
            // reyesPatchRaster.hlsl carries both telemetry blocks behind defines
            // that default to 0, so neither costs anything in a normal run.
            std::vector<DxcDefine> defines;
            if (IsTerrainRvtTelemetryDebugEnabled()) {
                defines.push_back(DxcDefine{ L"TERRAIN_RVT_TELEMETRY", L"1" });
            }
            if (IsObjectReyesAtlasTelemetryDebugEnabled()) {
                defines.push_back(DxcDefine{ L"CLOD_REYES_PATCH_RASTER_ATLAS_DEBUG_TELEMETRY", L"1" });
            }
            return defines;
        }(),
        "CLod.ReyesPatchRaster.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

ReyesPatchRasterBindings ReyesPatchRasterizationPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(
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
    ReyesPatchRasterBindings bindings{
        builder.BindShaderResource(m_visibleClustersBuffer), builder.BindShaderResource(m_visibleClusterTransformIndicesBuffer),
        builder.BindShaderResource(m_diceQueueBuffer), builder.BindShaderResource(m_diceQueueCounterBuffer),
        builder.BindShaderResource(m_rasterWorkBuffer), builder.BindShaderResource(m_rasterWorkCounterBuffer),
        builder.BindShaderResource(m_tessTableConfigsBuffer), builder.BindShaderResource(m_tessTableVerticesBuffer),
        builder.BindShaderResource(m_tessTableTrianglesBuffer),
        builder.BindIndirectArguments(m_indirectArgsBuffer), builder.BindUnorderedAccess(m_telemetryBuffer)};

    for (const auto& visibilityBuffer : m_visibilityBuffers) {
        builder.WithUnorderedAccess(visibilityBuffer);
    }
    if (m_slabResourceGroup) {
        builder.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    bindings.phase = m_phaseIndex;
    bindings.patchIndexBase = m_patchVisibilityIndexBase;
    bindings.enabled = !SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableNonVoxelVisibilitySettingName)();
    return bindings;
}

void ReyesPatchRasterizationPass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    std::vector<std::shared_ptr<PixelBuffer>> nextVisibilityBuffers;
    for (const auto& view : context.Views())
        if (view.visibilityBuffer) nextVisibilityBuffers.push_back(view.visibilityBuffer);

    m_declaredResourcesChanged = (nextVisibilityBuffers != m_visibilityBuffers);
    m_visibilityBuffers = std::move(nextVisibilityBuffers);
}

bool ReyesPatchRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

br::render::PreparedComputeIndirect ReyesPatchRasterizationPass::Prepare(
    const ReyesPatchRasterBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputeIndirect data{};
    data.enabled = bindings.enabled;
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = srv(bindings.transforms);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.diceCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.work);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    data.constants[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX] =
        ViewRasterInfoTable(preparation).Publish(preparation, m_viewRasterInfoPublisher);
    data.constants[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.telemetry, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_REYES_PATCH_RASTER_PHASE_INDEX] = bindings.phase;
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.workCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE] = bindings.patchIndexBase;
    {
        // The value baked here must equal the base the material passes use to
        // recognise a Reyes patch pixel; a mismatch makes patch pixels read as
        // ordinary clusters.
        static std::atomic<std::uint32_t> loggedPatchBase{ 0 };
        if (loggedPatchBase.fetch_add(1, std::memory_order_relaxed) < 8u) {
            spdlog::info("ReyesPatchRaster recipe: phase={} patchIndexBase={} enabled={}",
                bindings.phase, bindings.patchIndexBase, bindings.enabled);
        }
    }
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.tessVertices);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.tessTriangles);
    return data;
}

void ReyesPatchRasterizationPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
    ViewRasterInfoTable(preparation).AppendRevision(preparation, out);
}

CLodViewRasterInfoTable ReyesPatchRasterizationPass::ViewRasterInfoTable(const org::PassPrepareContext& preparation) const {
    const auto& context = CLodPreparationSnapshot(preparation);
    return BuildCLodVisibilityViewRasterInfo(context.Views(), context.ViewCameraBufferSize(), CLodRasterOutputKind::VisibilityBuffer);
}

void ReyesPatchRasterizationPass::Record(const ReyesPatchRasterBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
