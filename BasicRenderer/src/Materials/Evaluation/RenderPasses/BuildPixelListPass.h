#pragma once
#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"

struct BuildPixelListBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken visibleClusters, reyesDiceQueue;
    bool hasReyesDiceQueue = false;
    uint32_t patchVisibilityIndexBase = 0;
};

struct BuildPixelListInvocation {
    uint32_t voxelMaterialBin = UINT32_MAX;
    uint32_t groupsX = 0, groupsY = 0;
};

class BuildPixelListPass : public org::TypedRenderGraphPass<BuildPixelListPass, BuildPixelListInvocation, BuildPixelListBindings> {
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
        BuildPixelListBindings bindings{m_pso, b.ShaderResource(m_visibleClusterResource)};
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = b.ShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;

        b.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer,
                              Builtin::PerMaterialDataBuffer,
                              "Builtin::VisUtil::MaterialOffsetBuffer");
        b.UnorderedAccess("Builtin::VisUtil::MaterialWriteCursorBuffer",
            "Builtin::VisUtil::PixelListBuffer");
		b.ConstantBuffer(Builtin::PerFrameBuffer);
        b.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    BuildPixelListInvocation Prepare(const BuildPixelListBindings&, const org::PassPrepareContext& preparation) const {
        if (!preparation.preparationData) throw std::logic_error("BuildPixelListPass requires frame context");
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("BuildPixelListPass requires frame context");
        BuildPixelListInvocation invocation;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (materialState) {
            const bool found = materialState->TryGetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel,invocation.voxelMaterialBin);
            (void)found;
        }
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        invocation.groupsX = (resolution.x + 7u) / 8u;
        invocation.groupsY = (resolution.y + 7u) / 8u;
        return invocation;
    }
    static void Record(const BuildPixelListBindings& bindings, const BuildPixelListInvocation& invocation,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
        constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? context.Index(bindings.reyesDiceQueue)
            : 0xFFFFFFFFu;
        constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = invocation.voxelMaterialBin;
        br::render::DispatchCompute(context, bindings.pipeline, constants, invocation.groupsX, invocation.groupsY);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesDiceQueueResource;
	uint32_t m_patchVisibilityIndexBase = 0u;
};
