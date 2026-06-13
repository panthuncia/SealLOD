#pragma once
#include <vector>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/MeshManager.h"
#include "Render/RenderContext.h"
#include "Render/IndirectCommand.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/ShaderVariantRequestService.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"

class EvaluateMaterialGroupsPass : public ComputePass {
public:
    EvaluateMaterialGroupsPass() {
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

        m_reyesTessTableConfigsQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableConfigsTag>()
            .build();

        m_reyesTessTableVerticesQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableVerticesTag>()
            .build();

        m_reyesTessTableTrianglesQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableTrianglesTag>()
            .build();

        // Retrieve the page pool slab ResourceGroup for render graph tracking.
        try {
            auto getter = SettingsManager::GetInstance().getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
            if (auto* mm = getter()()) {
                if (auto* pool = mm->GetCLodPagePool()) {
                    m_slabResourceGroup = pool->GetSlabResourceGroup();
                }
            }
        } catch (...) {}
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(ECSResourceResolver(m_visibleClustersQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesDiceQueueQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableConfigsQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableVerticesQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableTrianglesQuery));

        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->WithShaderResource("Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
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
            Builtin::CLod::Offsets,
			Builtin::CLod::GroupChunks,
			Builtin::CLod::Groups,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::MeshMetadata,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::PerMaterialOpenPBRDataBuffer)
            .WithUnorderedAccess(Builtin::GBuffer::Normals,
                Builtin::GBuffer::Albedo,
                Builtin::GBuffer::Coat,
                Builtin::GBuffer::Emissive,
                Builtin::GBuffer::Fuzz,
                Builtin::GBuffer::MetallicRoughness,
                Builtin::GBuffer::MotionVectors,
				Builtin::DebugVisualization,
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats,
				Builtin::Material::TextureStreamingFeedbackBuffer)
    	.WithConstantBuffer(Builtin::PerFrameBuffer);
        b->WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

    void Setup() override {
        RefreshResourcePointers();
        RefreshDescriptorIndices();
        m_materialEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

    void RefreshResourcePointers() {
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

        m_visibleClusterResource = visibleClusterResources[0];
        m_reyesDiceQueueResource = nullptr;
        m_reyesTessTableConfigsResource = nullptr;
        m_reyesTessTableVerticesResource = nullptr;
        m_reyesTessTableTrianglesResource = nullptr;

        std::vector<GloballyIndexedResource*> reyesDiceQueueResources;
        m_reyesDiceQueueQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto test = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); test) {
                    reyesDiceQueueResources.push_back(test.get());
                }
            }
            });
        if (reyesDiceQueueResources.size() == 1) {
            m_reyesDiceQueueResource = reyesDiceQueueResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableConfigResources;
        m_reyesTessTableConfigsQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableConfigResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableConfigResources.size() == 1) {
            m_reyesTessTableConfigsResource = reyesTessTableConfigResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableVertexResources;
        m_reyesTessTableVerticesQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableVertexResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableVertexResources.size() == 1) {
            m_reyesTessTableVerticesResource = reyesTessTableVertexResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableTriangleResources;
        m_reyesTessTableTrianglesQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableTriangleResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableTriangleResources.size() == 1) {
            m_reyesTessTableTrianglesResource = reyesTessTableTriangleResources[0];
        }
    }

    void RefreshDescriptorIndices() {
        if (m_visibleClusterResource) {
            m_visibleClusterBufferSRVIndex = m_visibleClusterResource->GetSRVInfo(0).slot.index;
        }
        m_reyesDiceQueueBufferSRVIndex = m_reyesDiceQueueResource
            ? m_reyesDiceQueueResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableConfigsBufferSRVIndex = m_reyesTessTableConfigsResource
            ? m_reyesTessTableConfigsResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableVerticesBufferSRVIndex = m_reyesTessTableVerticesResource
            ? m_reyesTessTableVerticesResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableTrianglesBufferSRVIndex = m_reyesTessTableTrianglesResource
            ? m_reyesTessTableTrianglesResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& psoMgr = PSOManager::GetInstance();

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(psoMgr.GetComputeRootSignature().GetHandle());

        // Execute one indirect compute per active material slot.
        const auto& active = ctx.materialManager->GetActiveCompileFlags();
        const auto& sig = CommandSignatureManager::GetInstance().GetMaterialEvaluationCommandSignature();

        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        auto argBuf = m_materialEvalCmds->GetAPIResource();
        RefreshDescriptorIndices();

        const bool terrainRegionMaterialEvaluation =
            SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRegionMaterialEvaluation")();
		for (MaterialCompileFlags flags : active) { // TODO: cache on material flag changes
            if (terrainRegionMaterialEvaluation &&
                (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0) {
                continue;
            }
			unsigned int slot = 0u;
            if (!ctx.materialManager->TryGetCompileFlagsSlot(flags, slot) ||
                slot >= ctx.materialManager->GetCompileFlagsSlotsUsed()) {
                continue;
            }
            const MaterialCompileFlags shaderKey = GetMaterialEvaluationShaderKey(flags);
            const PipelineState* pso = psoMgr.TryGetMaterialEvalPSO(shaderKey);
            if (!pso) {
                continue;
            }

            cl.BindPipeline(pso->GetAPIPipelineState().GetHandle());
            BindResourceDescriptorIndices(cl, pso->GetResourceDescriptorSlots());

            // Set per-pass root constants
            unsigned int miscRootConstants[NumMiscUintRootConstants] = {};
            miscRootConstants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClusterBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_reyesDiceQueueBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_reyesTessTableConfigsBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_reyesTessTableVerticesBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_reyesTessTableTrianglesBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_USE_NORMAL_MAPS] =
                SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseNormalMapsSettingName)() ? 1u : 0u;
            cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, miscRootConstants);

            const uint64_t argOffset = static_cast<uint64_t>(slot) * stride;
            if (auto* bufferBase = dynamic_cast<BufferBase*>(m_materialEvalCmds)) {
                if (argOffset + stride > bufferBase->GetBufferSize()) {
                    spdlog::error(
                        "EvaluateMaterialGroupsPass: skipping undersized material eval args flags=0x{:X} slot={} offset={} stride={} backingBytes={}",
                        static_cast<uint64_t>(flags),
                        slot,
                        argOffset,
                        stride,
                        bufferBase->GetBufferSize());
                    continue;
                }
            }
            cl.ExecuteIndirect(
                sig.GetHandle(),
                argBuf.GetHandle(), argOffset,
                rhi::ResourceHandle{}, 0, // no count buffer
                1                         // single command
            );
        }

        return {};
    }

    void Cleanup() override {
        m_visibleClustersQuery = {};
        m_reyesDiceQueueQuery = {};
        m_reyesTessTableConfigsQuery = {};
        m_reyesTessTableVerticesQuery = {};
        m_reyesTessTableTrianglesQuery = {};
        m_slabResourceGroup.reset();
        m_materialEvalCmds = nullptr;
    }

private:
    Resource* m_materialEvalCmds;
    flecs::query<> m_visibleClustersQuery;
    flecs::query<> m_reyesDiceQueueQuery;
    flecs::query<> m_reyesTessTableConfigsQuery;
    flecs::query<> m_reyesTessTableVerticesQuery;
    flecs::query<> m_reyesTessTableTrianglesQuery;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    GloballyIndexedResource* m_visibleClusterResource = nullptr;
    GloballyIndexedResource* m_reyesDiceQueueResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableConfigsResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableVerticesResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableTrianglesResource = nullptr;
    uint32_t m_visibleClusterBufferSRVIndex = 0;
    uint32_t m_reyesDiceQueueBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_reyesTessTableConfigsBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableVerticesBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableTrianglesBufferSRVIndex = 0xFFFFFFFFu;
};
