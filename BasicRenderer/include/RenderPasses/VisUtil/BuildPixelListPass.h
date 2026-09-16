#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Materials/TechniqueDescriptor.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/MaterialEvaluationBuildInputs.h"

struct BuildPixelListBindings {
    org::ResourceBindingToken visibleClusters, reyesDiceQueue;
    bool hasReyesDiceQueue = false;
    uint32_t patchVisibilityIndexBase = 0;
};

class BuildPixelListPass : public org::TypedRenderGraphPass<BuildPixelListPass, org::EmptyPassFrameData, BuildPixelListBindings, br::render::PreparedComputeDispatch> {
public:
    explicit BuildPixelListPass(const MaterialEvaluationBuildInputs& inputs)
        : m_visibleClusterResource(inputs.visibleClusters),
          m_reyesDiceQueueResource(inputs.reyesDiceQueue),
          m_patchVisibilityIndexBase(CLodReyesPatchVisibilityIndexBase(inputs.visibleClusterCapacity)) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildPixelListCS",
            {},
            "BuildPixelListPSO");

        if (!m_visibleClusterResource)
            throw std::invalid_argument("BuildPixelListPass requires the published visible-cluster resource");
    }
    BuildPixelListBindings Declare(org::PassBuilder& b) {
        BuildPixelListBindings bindings{b.BindShaderResource(m_visibleClusterResource)};
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = b.BindShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;

        b.WithShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer,
                              Builtin::PerMaterialDataBuffer,
                              "Builtin::VisUtil::MaterialOffsetBuffer")
         .WithUnorderedAccess("Builtin::VisUtil::MaterialWriteCursorBuffer",
                              "Builtin::VisUtil::PixelListBuffer");
		b.WithConstantBuffer(Builtin::PerFrameBuffer)
         .PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    br::render::PreparedComputeDispatch BuildRecipe(const BuildPixelListBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("BuildPixelListPass requires frame context");
        br::render::PreparedComputeDispatch data{};
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.visibleClusters, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? preparation.ResolveView(bindings.reyesDiceQueue, {org::BindlessViewKind::ShaderResource}).index
            : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        uint32_t voxelMaterialBin = 0xFFFFFFFFu;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (materialState) {
            const bool foundVoxelSlot = materialState->TryGetCompileFlagsSlot(
                MaterialCompileFlags::MaterialCompileVoxel, voxelMaterialBin);
            (void)foundVoxelSlot;
        }
        data.constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = voxelMaterialBin;
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        data.groupsX = (resolution.x + 7u) / 8u;
        data.groupsY = (resolution.y + 7u) / 8u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        return {reinterpret_cast<uintptr_t>(m_pso.GetPayload().get()),
            published ? published->materials.revision : 0u, resolution.x, resolution.y};
    }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&,
        const BuildPixelListBindings&,
        const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    PipelineState m_pso;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesDiceQueueResource;
	uint32_t m_patchVisibilityIndexBase = 0u;
};
