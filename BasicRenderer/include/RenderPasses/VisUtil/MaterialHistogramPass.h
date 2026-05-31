#pragma once
#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Materials/TechniqueDescriptor.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

class MaterialHistogramPass : public ComputePass {
public:
    MaterialHistogramPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"MaterialHistogramCS",
            {},
            "MaterialHistogramPSO");

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();

        // Global LOD extension visibility buffer tag
        auto visBufferTag = ecsWorld.component<CLodExtensionVisibilityBufferTag>();

        // Query for entities with the visibility buffer tag
        m_visibleClustersQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClustersBufferTag>()
            .build();

        m_reyesDiceQueueQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesDiceQueueTag>()
            .build();
    }
    void DeclareResourceUsages(ComputePassBuilder* b) override {

        b->WithShaderResource(ECSResourceResolver(m_visibleClustersQuery)); 
    	b->WithShaderResource(ECSResourceResolver(m_reyesDiceQueueQuery));
        b->WithShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer,
                                                            Builtin::PerMaterialDataBuffer,
                                                            Builtin::Material::TextureGroup)
         .WithUnorderedAccess("Builtin::VisUtil::MaterialPixelCountBuffer");
		b->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
        std::vector<GloballyIndexedResource*> visibleClusterResources;
        m_visibleClustersQuery.each([&](flecs::entity e) {
            auto& res = e.get<Components::Resource>();
            auto test = std::static_pointer_cast<GloballyIndexedResource>(res.resource.lock());
            if (test) {
                visibleClusterResources.push_back(test.get());
            }
            const auto capacity = e.get<CLodVisibleClusterCapacity>();
            m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(capacity.maxVisibleClusters);
            });

        if (visibleClusterResources.size() != 1) {
            throw std::runtime_error("BuildPixelListPass: Expected exactly one visible cluster buffer resource.");
        }

        m_visibleClusterBufferSRVIndex = visibleClusterResources[0]->GetSRVInfo(0).slot.index;
        m_reyesDiceQueueBufferSRVIndex = 0xFFFFFFFFu;

        std::vector<GloballyIndexedResource*> reyesDiceQueueResources;
        m_reyesDiceQueueQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto test = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); test) {
                    reyesDiceQueueResources.push_back(test.get());
                }
            }
            });
        if (reyesDiceQueueResources.size() == 1) {
            m_reyesDiceQueueBufferSRVIndex = reyesDiceQueueResources[0]->GetSRVInfo(0).slot.index;
        }
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& pm = PSOManager::GetInstance();
        auto& cl = executionContext.commandList;

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());

        // Set per-pass root constants
        unsigned int miscRootConstants[NumMiscUintRootConstants] = {};
        miscRootConstants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClusterBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_reyesDiceQueueBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
        miscRootConstants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = ctx.materialManager->GetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel);
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, miscRootConstants);

        const uint32_t groupSizeX = 8, groupSizeY = 8;
        uint32_t x = (ctx.renderResolution.x + groupSizeX - 1) / groupSizeX;
        uint32_t y = (ctx.renderResolution.y + groupSizeY - 1) / groupSizeY;
        cl.Dispatch(x, y, 1);
        return {};
    }

    void Cleanup() override {
        m_visibleClustersQuery = {};
        m_reyesDiceQueueQuery = {};
    }

private:
    PipelineState m_pso;
	flecs::query<> m_visibleClustersQuery;
    flecs::query<> m_reyesDiceQueueQuery;
    uint32_t m_visibleClusterBufferSRVIndex = 0;
    uint32_t m_reyesDiceQueueBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_patchVisibilityIndexBase = 0u;
};
