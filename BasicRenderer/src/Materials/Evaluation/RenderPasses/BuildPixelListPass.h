#pragma once
#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"

struct BuildPixelListBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibleClusters, reyesDiceQueue, sideBand;
    bool hasSideBand = false;
    bool hasReyesDiceQueue = false;
    uint32_t patchVisibilityIndexBase = 0;
};

struct BuildPixelListInvocation {
    uint32_t voxelMaterialBin = UINT32_MAX;
    uint32_t groupsX = 0, groupsY = 0;
    bool operator==(const BuildPixelListInvocation&) const = default;
};

class BuildPixelListPass : public org::TypedRenderGraphPass<BuildPixelListPass, BuildPixelListInvocation, BuildPixelListBindings> {
public:
    explicit BuildPixelListPass(const MaterialEvaluationBuildInputs& inputs)
        : m_visibleClusterResource(inputs.visibleClusters),
          m_reyesDiceQueueResource(inputs.reyesDiceQueue),
          m_sideBandResource(inputs.visibleClusterTransformIndices),
          m_patchVisibilityIndexBase(CLodReyesPatchVisibilityIndexBase(inputs.visibleClusterCapacity)) {
        if (!m_visibleClusterResource)
            throw std::invalid_argument("BuildPixelListPass requires the published visible-cluster resource");
    }
    BuildPixelListBindings Declare(org::PassBuilder& b) {
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"BuildPixelListCS", "BuildPixelListPSO"));
        BuildPixelListBindings bindings{m_pipeline, b.ShaderResource(m_visibleClusterResource)};
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = b.ShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        if (m_sideBandResource) {
            bindings.sideBand = b.ShaderResource(m_sideBandResource);
            bindings.hasSideBand = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;

        b.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases,
                              Builtin::PerMaterialDataBuffer,
                              "Builtin::VisUtil::MaterialOffsetBuffer");
        b.UnorderedAccess("Builtin::VisUtil::MaterialWriteCursorBuffer",
            "Builtin::VisUtil::PixelListBuffer");
		b.ConstantBuffer(Builtin::PerFrameBuffer);
        b.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    // Kept recordings (M6 step 7): the voxel material bin and the dispatch follow the published material state and the render
    // resolution, taken from the frame's host data with no side effect (RenderGraphPass::TakeFrameValues) before Prepare reads
    // the same.
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        m_voxelMaterialBin = 0xFFFFFFFFu;
        m_renderResolution = {};
        const auto* update = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        const auto* render = execution.hostData ? execution.hostData->Get<RenderContext>() : nullptr;
        if (!update && !render) return;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (materialState) (void)materialState->TryGetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel, m_voxelMaterialBin);
        m_renderResolution = update ? update->renderResolution : render->renderResolution;
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        br::render::AppendRevision(out, m_voxelMaterialBin);
        br::render::AppendRevision(out, m_renderResolution.x);
        br::render::AppendRevision(out, m_renderResolution.y);
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
        constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
            bindings.hasSideBand ? context.Index(bindings.sideBand) : 0xFFFFFFFFu;
        constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = invocation.voxelMaterialBin;
        br::render::DispatchCompute(context, bindings.pipeline, constants, invocation.groupsX, invocation.groupsY);
    }

private:
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesDiceQueueResource;
    std::shared_ptr<org::GloballyIndexedResource> m_sideBandResource;
	uint32_t m_patchVisibilityIndexBase = 0u;
    br::PipelineState m_pipeline;
    uint32_t m_voxelMaterialBin = 0xFFFFFFFFu;
    DirectX::XMUINT2 m_renderResolution{};
};
