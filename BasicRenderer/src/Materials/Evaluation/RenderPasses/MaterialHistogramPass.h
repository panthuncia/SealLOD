#pragma once
#include <atomic>

#include <spdlog/spdlog.h>

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

struct MaterialHistogramBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken visibleClusters, reyesDiceQueue;
    bool hasReyesDiceQueue = false;
    uint32_t patchVisibilityIndexBase = 0;
};
struct MaterialHistogramFrame {
    uint32_t voxelMaterialBin = 0xFFFFFFFFu;
    uint32_t groupsX = 0, groupsY = 0;
};

class MaterialHistogramPass : public org::TypedRenderGraphPass<MaterialHistogramPass, MaterialHistogramFrame, MaterialHistogramBindings> {
public:
    explicit MaterialHistogramPass(const MaterialEvaluationBuildInputs& inputs)
        : m_visibleClusterResource(inputs.visibleClusters),
          m_reyesDiceQueueResource(inputs.reyesDiceQueue),
          m_patchVisibilityIndexBase(CLodReyesPatchVisibilityIndexBase(inputs.visibleClusterCapacity)) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"MaterialHistogramCS",
            {},
            "MaterialHistogramPSO");

        if (!m_visibleClusterResource)
            throw std::invalid_argument("MaterialHistogramPass requires the published visible-cluster resource");
    }
    MaterialHistogramBindings Declare(org::PassBuilder& b) {
        MaterialHistogramBindings bindings{m_pso, b.ShaderResource(m_visibleClusterResource)};
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = b.ShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
        // This pass buckets every visibility pixel by material compile-flag
        // variant. Without the dice queue and patch base it cannot tell a Reyes
        // patch pixel from an ordinary cluster pixel, and folds the Reyes pixels
        // into the regular variant, which then shades them as clusters using an
        // out-of-range cluster index.
        {
            static std::atomic<std::uint32_t> loggedHistogramReyes{ 0 };
            if (loggedHistogramReyes.fetch_add(1, std::memory_order_relaxed) < 8u) {
                spdlog::info(
                    "MaterialHistogram Reyes bindings: diceQueue={} patchIndexBase={} diceQueueResource={}",
                    bindings.hasReyesDiceQueue,
                    m_patchVisibilityIndexBase,
                    m_reyesDiceQueueResource != nullptr);
            }
        }
	    b.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer,
                              Builtin::PerMaterialDataBuffer);
        b.UnorderedAccess("Builtin::VisUtil::MaterialPixelCountBuffer");
		b.ConstantBuffer(Builtin::PerFrameBuffer);
        b.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    MaterialHistogramFrame Prepare(const MaterialHistogramBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialHistogramPass requires frame context");
        MaterialHistogramFrame frame;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (materialState) {
            const bool foundVoxelSlot = materialState->TryGetCompileFlagsSlot(
                MaterialCompileFlags::MaterialCompileVoxel, frame.voxelMaterialBin);
            (void)foundVoxelSlot;
        }
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        frame.groupsX = (resolution.x + 7u) / 8u;
        frame.groupsY = (resolution.y + 7u) / 8u;
        return frame;
    }

    static void Record(const MaterialHistogramBindings& bindings, const MaterialHistogramFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
        constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? context.Index(bindings.reyesDiceQueue)
            : 0xFFFFFFFFu;
        constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        {
            // These are the two values VisUtil.hlsl tests to decide whether a
            // pixel is a Reyes patch. Logged at record time because that is
            // the value actually delivered to the shader, which is not
            // necessarily what Declare bound.
            static std::atomic<std::uint32_t> loggedHistRecipe{ 0 };
            if (loggedHistRecipe.fetch_add(1, std::memory_order_relaxed) < 8u) {
                spdlog::info("MaterialHistogram recipe: diceQueueDescriptor={} patchIndexBase={} visibleClusters={}",
                    constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX],
                    constants[VISBUF_REYES_PATCH_INDEX_BASE],
                    constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX]);
            }
        }
        constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = frame.voxelMaterialBin;
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesDiceQueueResource;
    uint32_t m_patchVisibilityIndexBase = 0u;
};
