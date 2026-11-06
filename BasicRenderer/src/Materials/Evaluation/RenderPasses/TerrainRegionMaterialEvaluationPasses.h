#pragma once

#include <bit>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include <BasicRenderer/Extensions/IndirectCommand.h>
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/Buffers/PagePool.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"

namespace TerrainRegionMaterialEval
{
    static constexpr uint32_t MaxTerrainRegions = 65536u;
    static constexpr uint32_t PrefixBlockSize = 1024u;
    static constexpr uint32_t TerrainSetIndexV1 = 0u;

    inline bool IsTerrainMaterialFlags(MaterialCompileFlags flags)
    {
        return (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0;
    }

    struct PipelineBindings {
        org::PipelineState pipeline;
    };
}

class TerrainRegionCounterResetPass : public org::TypedRenderGraphPass<TerrainRegionCounterResetPass,
    org::EmptyPassFrameData, TerrainRegionMaterialEval::PipelineBindings> {
public:
    TerrainRegionMaterialEval::PipelineBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->UnorderedAccess(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer");
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"ClearTerrainRegionCountersCS",
            "VisUtil_ClearTerrainRegionCountersPSO"))};
    }

    static void Record(const TerrainRegionMaterialEval::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        br::render::DispatchCompute(context, bindings.pipeline, constants,
            (TerrainRegionMaterialEval::MaxTerrainRegions + 63u) / 64u);
    }
};

struct TerrainRegionMaterialRangeBindings {
    org::PipelineState pipeline;
    org::ResourceBindingToken arguments;
    org::CommandSignatureState signature;
};
struct TerrainRegionMaterialRangeFrame {
    std::vector<uint64_t> argumentOffsets;
};

template<class Derived>
class TerrainRegionMaterialRangePassBase : public org::TypedRenderGraphPass<Derived,
    TerrainRegionMaterialRangeFrame, TerrainRegionMaterialRangeBindings> {
public:
    explicit TerrainRegionMaterialRangePassBase(const wchar_t* entryPoint, const char* debugName)
        : m_entryPoint(entryPoint), m_debugName(debugName) {}

    TerrainRegionMaterialRangeFrame Prepare(const TerrainRegionMaterialRangeBindings&,
        const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        TerrainRegionMaterialRangeFrame frame;
        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        const auto materialState = ctx.publishedRendererState
            ? ctx.publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        if (!materialState) return frame;
        for (std::size_t activeIndex = 0; activeIndex < materialState->activeCompileFlags.size(); ++activeIndex) {
            const MaterialCompileFlags flags = materialState->activeCompileFlags[activeIndex];
            if (!TerrainRegionMaterialEval::IsTerrainMaterialFlags(flags)) {
                continue;
            }
            if (activeIndex >= materialState->activeCompileFlagSlots.size()) {
                continue;
            }
            const uint32_t slot = materialState->activeCompileFlagSlots[activeIndex];
            if (slot >= materialState->compileFlagSlotsUsed) continue;
            frame.argumentOffsets.push_back(static_cast<uint64_t>(slot) * stride);
        }
        return frame;
    }

    static void Record(const TerrainRegionMaterialRangeBindings& bindings, const TerrainRegionMaterialRangeFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        for (const uint64_t argumentOffset : frame.argumentOffsets)
            br::render::DispatchComputeIndirect(context, bindings.pipeline, constants, bindings.signature,
                bindings.arguments, argumentOffset);
    }

protected:
    TerrainRegionMaterialRangeBindings DeclareArguments(org::PassBuilder& builder) {
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", m_entryPoint, m_debugName)),
            builder.IndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer"),
            builder.CommandSignature(br::render::MaterialEvaluationCommandSignature())};
    }

    std::wstring m_entryPoint;
    std::string m_debugName;
};

class TerrainRegionHistogramPass : public TerrainRegionMaterialRangePassBase<TerrainRegionHistogramPass> {
public:
    TerrainRegionHistogramPass()
        : TerrainRegionMaterialRangePassBase<TerrainRegionHistogramPass>(L"TerrainRegionHistogramFromMaterialRangeCS", "VisUtil_TerrainRegionHistogramPSO") {}

    TerrainRegionMaterialRangeBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets);
        b->UnorderedAccess(
                "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
                "Builtin::VisUtil::TerrainRegionActiveListBuffer",
                "Builtin::VisUtil::TerrainRegionActiveCountBuffer");
        b->ConstantBuffer(Builtin::PerFrameBuffer);
        return DeclareArguments(builder);
    }
};

class TerrainRegionBlockScanPass : public org::TypedRenderGraphPass<TerrainRegionBlockScanPass,
    org::EmptyPassFrameData, TerrainRegionMaterialEval::PipelineBindings> {
public:
    TerrainRegionMaterialEval::PipelineBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource("Builtin::VisUtil::TerrainRegionPixelCountBuffer");
        b->UnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionBlockSumsBuffer");
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/materialPrefixSum.hlsl", L"TerrainRegionBlockScanCS",
            "VisUtil_TerrainRegionBlockScanPSO"))};
    }

    static void Record(const TerrainRegionMaterialEval::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        br::render::DispatchCompute(context, bindings.pipeline, constants,
            (TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize);
    }
};

class TerrainRegionBlockOffsetsPass : public org::TypedRenderGraphPass<TerrainRegionBlockOffsetsPass,
    org::EmptyPassFrameData, TerrainRegionMaterialEval::PipelineBindings> {
public:
    TerrainRegionMaterialEval::PipelineBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionBlockSumsBuffer");
        b->UnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionScannedBlockSumsBuffer",
                "Builtin::VisUtil::TerrainRegionTotalPixelCountBuffer");
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/materialPrefixSum.hlsl", L"TerrainRegionBlockOffsetsCS",
            "VisUtil_TerrainRegionBlockOffsetsPSO"))};
    }

    static void Record(const TerrainRegionMaterialEval::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        constants[1] = (TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize;
        br::render::DispatchCompute(context, bindings.pipeline, constants, 1u);
    }
};

class TerrainRegionPixelListPass : public TerrainRegionMaterialRangePassBase<TerrainRegionPixelListPass> {
public:
    TerrainRegionPixelListPass()
        : TerrainRegionMaterialRangePassBase<TerrainRegionPixelListPass>(L"TerrainRegionListFromMaterialRangeCS", "VisUtil_TerrainRegionPixelListPSO") {}

    TerrainRegionMaterialRangeBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets,
            "Builtin::VisUtil::TerrainRegionOffsetBuffer");
        b->UnorderedAccess(
                "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
                "Builtin::VisUtil::TerrainRegionPixelListBuffer");
        b->ConstantBuffer(Builtin::PerFrameBuffer);
        return DeclareArguments(builder);
    }
};

class BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass : public org::TypedRenderGraphPass<BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass,
    org::EmptyPassFrameData, TerrainRegionMaterialEval::PipelineBindings> {
public:
    TerrainRegionMaterialEval::PipelineBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource("Builtin::VisUtil::TerrainRegionActiveCountBuffer");
        b->UnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"BuildTerrainRegionCommandBuildDispatchArgsCS",
            "VisUtil_BuildTerrainRegionCommandBuildDispatchArgsPSO"))};
    }

    static void Record(const TerrainRegionMaterialEval::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, 1u);
    }
};

struct BuildTerrainRegionMaterialIndirectCommandBufferBindings {
    org::PipelineState pipeline;
    org::ResourceBindingToken dispatchArgs;
    org::CommandSignatureState signature;
};

class BuildTerrainRegionMaterialIndirectCommandBufferPass : public org::TypedRenderGraphPass<BuildTerrainRegionMaterialIndirectCommandBufferPass,
    org::EmptyPassFrameData, BuildTerrainRegionMaterialIndirectCommandBufferBindings> {
public:
    BuildTerrainRegionMaterialIndirectCommandBufferBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->ShaderResource(
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer",
            "Builtin::VisUtil::TerrainRegionActiveListBuffer",
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionOffsetBuffer");
        b->UnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
        return {builder.Pipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"BuildTerrainRegionEvaluateIndirectArgsCS",
            "VisUtil_BuildTerrainRegionEvaluateIndirectArgsPSO")), builder.IndirectArguments("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer"),
            builder.CommandSignature(br::render::RawDispatchCommandSignature())};
    }

    static void Record(const BuildTerrainRegionMaterialIndirectCommandBufferBindings& bindings,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        constants[1] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        br::render::DispatchComputeIndirect(context, bindings.pipeline, constants, bindings.signature, bindings.dispatchArgs);
    }
};

struct EvaluateTerrainRegionMaterialGroupsBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken visibleClusters, reyesDiceQueue;
    org::DeclaredViewToken reyesTessTableConfigs, reyesTessTableVertices, reyesTessTableTriangles;
    org::ResourceBindingToken arguments, activeCount;
    org::CommandSignatureState signature;
    bool hasReyesDiceQueue = false, hasReyesTessTables = false;
    uint32_t patchVisibilityIndexBase = 0;
};
struct EvaluateTerrainRegionMaterialGroupsFrame {
    bool useNormalMaps = true;
    float terrainNormalBlend = 0;
    uint32_t terrainNormalMipBias = 0;
    float objectNormalMapBlend = 0;
};

class EvaluateTerrainRegionMaterialGroupsPass : public org::TypedRenderGraphPass<EvaluateTerrainRegionMaterialGroupsPass,
    EvaluateTerrainRegionMaterialGroupsFrame, EvaluateTerrainRegionMaterialGroupsBindings> {
public:
    explicit EvaluateTerrainRegionMaterialGroupsPass(const MaterialEvaluationBuildInputs& services) {
        m_visibleClusterResource = services.visibleClusters;
        m_reyesDiceQueueResource = services.reyesDiceQueue;
        m_reyesTessTableConfigsResource = services.reyesTessTableConfigs;
        m_reyesTessTableVerticesResource = services.reyesTessTableVertices;
        m_reyesTessTableTrianglesResource = services.reyesTessTableTriangles;
        m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(services.visibleClusterCapacity);
        m_slabResourceGroup = services.clodSlabResources;
    }
    EvaluateTerrainRegionMaterialGroupsBindings Declare(org::PassBuilder& builder) {
        EvaluateTerrainRegionMaterialGroupsBindings bindings{};
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(
            L"shaders/VisUtilEvaluateTerrainRegion.hlsl",
            L"EvaluateTerrainRegionMaterialGroupCS",
            "VisUtil_EvaluateTerrainRegionMaterialGroupPSO",
            {{L"PSO_TERRAIN", L"1"},
             {L"VISUTIL_SPECIALIZED_MATERIAL_EVAL", L"1"},
             {L"VISUTIL_USE_COMPACT_MATERIAL_EVAL", L"1"},
             {L"CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE", L"1"}}));
        bindings.visibleClusters = builder.ShaderResource(m_visibleClusterResource).View();
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = builder.ShaderResource(m_reyesDiceQueueResource).View();
            bindings.hasReyesDiceQueue = true;
        }
        if (m_reyesTessTableConfigsResource && m_reyesTessTableVerticesResource && m_reyesTessTableTrianglesResource) {
            bindings.reyesTessTableConfigs = builder.ShaderResource(m_reyesTessTableConfigsResource).View();
            bindings.reyesTessTableVertices = builder.ShaderResource(m_reyesTessTableVerticesResource).View();
            bindings.reyesTessTableTriangles = builder.ShaderResource(m_reyesTessTableTrianglesResource).View();
            bindings.hasReyesTessTables = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;

        if (m_slabResourceGroup) {
            b->ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->ShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::PrimaryCamera::LinearDepthMap,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases,
            Builtin::CameraBuffer,
            Builtin::NormalMatrixBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMaterialDataBuffer,
            "Builtin::PerMaterialEvalDataBuffer",
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
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
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::PerMaterialOpenPBRDataBuffer);
        b->UnorderedAccess(
                Builtin::Surface::BaseColorOpacity,
                Builtin::Surface::NormalRoughness,
                Builtin::Surface::SpecularAo,
                Builtin::Surface::Emissive,
                Builtin::Surface::Motion,
                Builtin::Surface::Payload0,
                Builtin::Surface::Payload1,
				Builtin::Surface::Identity,
				Builtin::Surface::Records,
				Builtin::DebugVisualization,
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats,
				Builtin::Material::TextureStreamingFeedbackBuffer);
        b->ConstantBuffer(Builtin::PerFrameBuffer);
        bindings.arguments = b->IndirectArguments("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
        bindings.activeCount = b->IndirectArguments("Builtin::VisUtil::TerrainRegionActiveCountBuffer");
        bindings.signature = b->CommandSignature(br::render::TerrainRegionMaterialEvaluationCommandSignature());
        return bindings;
    }

    EvaluateTerrainRegionMaterialGroupsFrame Prepare(const EvaluateTerrainRegionMaterialGroupsBindings&,
        const org::PassPrepareContext&) const {
        return {CLodReyesUseNormalMaps(), CLodReyesTerrainNormalBlend(), CLodReyesTerrainNormalMipBias(),
            CLodReyesObjectNormalMapBlend()};
    }

    static void Record(const EvaluateTerrainRegionMaterialGroupsBindings& bindings,
        const EvaluateTerrainRegionMaterialGroupsFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
        constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
        constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? context.Index(bindings.reyesDiceQueue) : 0xFFFFFFFFu;
        constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? context.Index(bindings.reyesTessTableConfigs) : 0xFFFFFFFFu;
        constants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? context.Index(bindings.reyesTessTableVertices) : 0xFFFFFFFFu;
        constants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? context.Index(bindings.reyesTessTableTriangles) : 0xFFFFFFFFu;
        constants[VISBUF_REYES_USE_NORMAL_MAPS] = frame.useNormalMaps ? 1u : 0u;
        constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(frame.terrainNormalBlend);
        constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = frame.terrainNormalMipBias;
        constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(frame.objectNormalMapBlend);
        br::render::DispatchComputeIndirect(context, bindings.pipeline, constants, bindings.signature, bindings.arguments,
            0, TerrainRegionMaterialEval::MaxTerrainRegions, bindings.activeCount);
    }

    void ShutdownPass() {
        m_slabResourceGroup.reset();
    }

private:
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesDiceQueueResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesTessTableConfigsResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesTessTableVerticesResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesTessTableTrianglesResource;
    uint32_t m_patchVisibilityIndexBase = 0u;
};
