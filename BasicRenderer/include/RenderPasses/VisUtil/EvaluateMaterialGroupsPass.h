#pragma once
#include <vector>
#include <cstdint>
#include <bit>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/IndirectCommand.h"
#include "Render/OutputTypes.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/ShaderVariantRequestService.h"
#include "Render/MaterialEvaluationBuildInputs.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "RenderPasses/PreparedComputeDispatch.h"

struct EvaluateMaterialGroupsBindings {
    org::ResourceBindingToken visibleClusters, visibleClusterTransformIndices;
    org::ResourceBindingToken reyesDiceQueue, reyesTessTableConfigs;
    org::ResourceBindingToken reyesTessTableVertices, reyesTessTableTriangles;
    bool hasReyesDiceQueue = false, hasReyesTessTables = false;
    uint32_t patchVisibilityIndexBase = 0;
};

class EvaluateMaterialGroupsPass : public org::TypedRenderGraphPass<EvaluateMaterialGroupsPass,
    org::EmptyPassFrameData, EvaluateMaterialGroupsBindings, br::render::PreparedComputeIndirectSequence> {
public:
    EvaluateMaterialGroupsPass(MaterialEvaluationBuildInputs inputs, bool terrainRvtEnabled)
        : m_inputs(std::move(inputs)), m_terrainRvtEnabled(terrainRvtEnabled) {
        if (!m_inputs.IsValid()) throw std::invalid_argument("EvaluateMaterialGroupsPass requires captured build inputs");

        m_visibleClusterResource = m_inputs.visibleClusters;
        m_visibleClusterTransformIndicesResource = m_inputs.visibleClusterTransformIndices;
        m_reyesDiceQueueResource = m_inputs.reyesDiceQueue;
        m_reyesTessTableConfigsResource = m_inputs.reyesTessTableConfigs;
        m_reyesTessTableVerticesResource = m_inputs.reyesTessTableVertices;
        m_reyesTessTableTrianglesResource = m_inputs.reyesTessTableTriangles;
        m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(m_inputs.visibleClusterCapacity);
        m_slabResourceGroup = m_inputs.clodSlabResources;
    }

    EvaluateMaterialGroupsBindings Declare(org::PassBuilder& builder) {
        EvaluateMaterialGroupsBindings bindings{};
        bindings.visibleClusters = builder.BindShaderResource(m_visibleClusterResource);
        bindings.visibleClusterTransformIndices = builder.BindShaderResource(m_visibleClusterTransformIndicesResource);
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = builder.BindShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        if (m_reyesTessTableConfigsResource && m_reyesTessTableVerticesResource && m_reyesTessTableTrianglesResource) {
            bindings.reyesTessTableConfigs = builder.BindShaderResource(m_reyesTessTableConfigsResource);
            bindings.reyesTessTableVertices = builder.BindShaderResource(m_reyesTessTableVerticesResource);
            bindings.reyesTessTableTriangles = builder.BindShaderResource(m_reyesTessTableTrianglesResource);
            bindings.hasReyesTessTables = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;

        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->WithShaderResource("Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::PrimaryCamera::LinearDepthMap,
            //Builtin::PrimaryCamera::VisibleClusterTable,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
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
            Builtin::Terrain::TextureGroup,
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
            Builtin::PerMaterialOpenPBRDataBuffer)
            .WithUnorderedAccess(Builtin::Surface::BaseColorOpacity,
                Builtin::Surface::NormalRoughness,
                Builtin::Surface::SpecularAo,
                Builtin::Surface::Emissive,
                Builtin::Surface::Motion,
                Builtin::Surface::Payload0,
                Builtin::Surface::Payload1,
                Builtin::Surface::Identity,
                Builtin::Surface::Records,
                Builtin::DebugVisualization,
				Builtin::Material::TextureStreamingFeedbackBuffer)
    	.WithConstantBuffer(Builtin::PerFrameBuffer);

        if (m_terrainRvtEnabled) {
            b->WithShaderResource(
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
                Builtin::Terrain::RvtMaterialAtlas)
                .WithUnorderedAccess(
                    Builtin::Terrain::RvtRequestMasks,
                    Builtin::Terrain::RvtRequestList,
                    Builtin::Terrain::RvtCounters,
                    Builtin::Terrain::RvtStats);
        }
        b->WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
        return bindings;
    }

    void Initialize() {
        m_materialEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }


    br::render::PreparedComputeIndirectSequence BuildRecipe(const EvaluateMaterialGroupsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto materialState = context->publishedRendererState
            ? context->publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (!materialState) return {};
        br::render::PreparedComputeIndirectSequence data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.commandSignature = preparation.CaptureCommandSignature(m_inputs.commandSignatures->CaptureMaterialEvaluationCommandSignature());
        data.argumentsReference = preparation.CaptureResource(m_materialEvalCmds->GetGlobalResourceID());
        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        const bool terrainEvaluation = context->terrainRegionMaterialEvaluationEnabled;
        const auto outputType = context->outputType;
        for (std::size_t activeIndex = 0; activeIndex < materialState->activeCompileFlags.size(); ++activeIndex) {
            const auto flags = materialState->activeCompileFlags[activeIndex];
            if (terrainEvaluation && (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0) continue;
            if (activeIndex >= materialState->activeCompileFlagSlots.size()) continue;
            const unsigned int slot = materialState->activeCompileFlagSlots[activeIndex];
            if (slot >= materialState->compileFlagSlotsUsed) continue;
            auto shaderKey = GetMaterialEvaluationShaderKey(flags);
            if (outputType == OutputType::COLOR) shaderKey |= MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
            const PipelineState* pso = m_inputs.pipelines->TryGetMaterialEvalPSO(shaderKey);
            if (!pso) continue;
            const uint64_t argOffset = static_cast<uint64_t>(slot) * stride;
            if (auto* buffer = dynamic_cast<BufferBase*>(m_materialEvalCmds);
                buffer && argOffset + stride > buffer->GetBufferSize()) continue;
            auto capture = preparation;
            capture.captureDescriptorIndices = [this](const PipelineResources& resources) {
                return CaptureMaterialResourceDescriptorIndices(resources);
            };
            auto program = capture.CaptureProgramBinding(*pso);
            br::render::PreparedComputeIndirectSequence::Step step{};
            step.program = program.program;
            step.descriptorIndices = std::move(program.descriptorIndices);
            step.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(
                bindings.visibleClusters, {org::BindlessViewKind::ShaderResource}).index;
            step.constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = preparation.ResolveView(
                bindings.visibleClusterTransformIndices, {org::BindlessViewKind::ShaderResource}).index;
            step.constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
                ? preparation.ResolveView(bindings.reyesDiceQueue, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
            step.constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
            step.constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
                ? preparation.ResolveView(bindings.reyesTessTableConfigs, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
            step.constants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
                ? preparation.ResolveView(bindings.reyesTessTableVertices, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
            step.constants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
                ? preparation.ResolveView(bindings.reyesTessTableTriangles, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
            step.constants[VISBUF_REYES_USE_NORMAL_MAPS] = CLodReyesUseNormalMaps() ? 1u : 0u;
            step.constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesTerrainNormalBlend());
            step.constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = CLodReyesTerrainNormalMipBias();
            step.constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesObjectNormalMapBlend());
            step.argumentsOffset = argOffset; data.steps.push_back(std::move(step));
        }
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto materialState = context->publishedRendererState
            ? context->publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(),
            context->publishedRendererState ? context->publishedRendererState->materials.revision : 0u,
            static_cast<uint64_t>(context->outputType), context->terrainRegionMaterialEvaluationEnabled};
        if (materialState) for (const auto flags : materialState->activeCompileFlags) {
            auto key = GetMaterialEvaluationShaderKey(flags);
            if (context->outputType == OutputType::COLOR) key |= MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
            const auto* pso = m_inputs.pipelines->TryGetMaterialEvalPSO(key);
            revision.push_back(reinterpret_cast<uintptr_t>(pso ? pso->GetPayload().get() : nullptr));
        }
        return revision;
    }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeIndirectSequence&,
        const EvaluateMaterialGroupsBindings&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeIndirectSequence& recipe, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirectSequence(recipe, recording);
    }

    void ShutdownPass() {
        m_slabResourceGroup.reset();
        m_materialEvalCmds = nullptr;
    }

private:
    MaterialEvaluationBuildInputs m_inputs;
    std::vector<unsigned int> CaptureMaterialResourceDescriptorIndices(const PipelineResources& resources) const {
        std::vector<unsigned int> indices;
        indices.reserve(resources.mandatoryResourceDescriptorSlots.size() + resources.optionalResourceDescriptorSlots.size());
        for (const auto& binding : resources.mandatoryResourceDescriptorSlots) {
            const bool allowMissing = !m_terrainRvtEnabled && binding.name.starts_with("Builtin::Terrain::Rvt");
            indices.push_back(m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(binding, allowMissing));
        }
        for (const auto& binding : resources.optionalResourceDescriptorSlots)
            indices.push_back(m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(binding, true));
        return indices;
    }

    bool m_terrainRvtEnabled = false;
    Resource* m_materialEvalCmds;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterTransformIndicesResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesDiceQueueResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableConfigsResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableVerticesResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableTrianglesResource;
    uint32_t m_patchVisibilityIndexBase = 0u;
};
