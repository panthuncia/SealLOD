#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <bit>
#include <cstdlib>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/MeshManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/IndirectCommand.h"
#include "Render/OutputTypes.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/ShaderVariantRequestService.h"
#include "Render/ProducerPassServices.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"

class EvaluateMaterialGroupsPass : public ComputePass {
public:
    EvaluateMaterialGroupsPass(ProducerPassServices& services, bool terrainRvtEnabled)
        : m_services(services), m_terrainRvtEnabled(terrainRvtEnabled) {
        if (!m_services.IsValid()) throw std::invalid_argument("EvaluateMaterialGroupsPass requires producer services");
        if (const char* path = std::getenv("SARP_MATERIAL_PIPELINE_READBACK_PATH")) {
            m_materialPixelTelemetryEnabled = path[0] != '\0';
        }
        auto& ecsWorld = m_services.ecs->GetWorld();

        // Global LOD extension visibility buffer tag
        auto visBufferTag = ecsWorld.component<CLodExtensionVisibilityBufferTag>();

        // Query for entities with the visibility buffer tag
        m_visibleClustersQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClustersBufferTag>()
            .build();

        m_visibleClusterTransformIndicesQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClusterTransformIndicesBufferTag>()
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
            auto getter = m_services.settings->getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
            if (auto* mm = getter()()) {
                if (auto* pool = mm->GetCLodPagePool()) {
                    m_slabResourceGroup = pool->GetSlabResourceGroup();
                }
            }
        } catch (...) {}
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        // TODO(async-state-coherence): CLOD visibility/mesh metadata is still
        // sourced from live manager resources while draw records, object data,
        // and material tables may come from PublishedRendererState. Make the
        // static/draw transaction select exact generations of both sides before
        // activation; GPU barriers alone cannot make mixed generations coherent.
        b->WithShaderResource(ECSResourceResolver(m_visibleClustersQuery));
        b->WithShaderResource(ECSResourceResolver(m_visibleClusterTransformIndicesQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesDiceQueueQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableConfigsQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableVerticesQuery));
    	b->WithShaderResource(ECSResourceResolver(m_reyesTessTableTrianglesQuery));

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
            throw std::runtime_error("EvaluateMaterialGroupsPass: Expected exactly one visible cluster buffer resource.");
        }

        m_visibleClusterResource = visibleClusterResources[0];
        m_visibleClusterTransformIndicesResource = nullptr;
        m_reyesDiceQueueResource = nullptr;
        m_reyesTessTableConfigsResource = nullptr;
        m_reyesTessTableVerticesResource = nullptr;
        m_reyesTessTableTrianglesResource = nullptr;

        std::vector<GloballyIndexedResource*> visibleClusterTransformIndexResources;
        m_visibleClusterTransformIndicesQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    visibleClusterTransformIndexResources.push_back(resource.get());
                }
            }
        });
        if (visibleClusterTransformIndexResources.size() != 1) {
            throw std::runtime_error("EvaluateMaterialGroupsPass: Expected exactly one visible cluster transform-index resource.");
        }
        m_visibleClusterTransformIndicesResource = visibleClusterTransformIndexResources[0];

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
        m_visibleClusterTransformIndicesBufferSRVIndex = m_visibleClusterTransformIndicesResource
            ? m_visibleClusterTransformIndicesResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
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
        if (!renderContext) return {};
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& psoMgr = *m_services.pipelines;

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(psoMgr.GetComputeRootSignature().GetHandle());

        // Execute one indirect compute per compile-flag slot captured by this
        // frame's immutable renderer-state snapshot.
        const auto materialState = ctx.publishedRendererState
            ? ctx.publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        if (!materialState) return {};
        const auto& sig = m_services.commandSignatures->GetMaterialEvaluationCommandSignature();
		m_publishedMaterialDescriptors[0] = materialState->baseTable && materialState->baseTable->resource
			? materialState->baseTable->resource->GetSRVInfo(0).slot.index : 0xFFFFFFFFu;
		m_publishedMaterialDescriptors[1] = materialState->evalTable && materialState->evalTable->resource
			? materialState->evalTable->resource->GetSRVInfo(0).slot.index : 0xFFFFFFFFu;
		m_publishedMaterialDescriptors[2] = materialState->openPbrTable && materialState->openPbrTable->resource
			? materialState->openPbrTable->resource->GetSRVInfo(0).slot.index : 0xFFFFFFFFu;
		const auto materialRevision = ctx.publishedRendererState->materials.revision;
		if (materialRevision != m_lastLoggedMaterialRevision) {
			m_lastLoggedMaterialRevision = materialRevision;
			std::string compileFlagMapping;
			for (std::size_t index = 0; index < materialState->activeCompileFlags.size() &&
				index < materialState->activeCompileFlagSlots.size(); ++index) {
				if (!compileFlagMapping.empty()) compileFlagMapping += ',';
				compileFlagMapping += fmt::format("{}:0x{:X}",
					materialState->activeCompileFlagSlots[index],
					static_cast<std::uint64_t>(materialState->activeCompileFlags[index]));
			}
			spdlog::info(
				"Material evaluation binding: stateEpoch={} revision={} descriptors(base={} eval={} openpbr={}) resources(base={} eval={} openpbr={}) rows(base={} eval={} openpbr={}) activeVariants={} slotFlags='{}'",
				ctx.publishedRendererState->epoch, materialRevision,
				m_publishedMaterialDescriptors[0], m_publishedMaterialDescriptors[1], m_publishedMaterialDescriptors[2],
				materialState->baseTable && materialState->baseTable->resource ? materialState->baseTable->resource->GetGlobalResourceID() : 0u,
				materialState->evalTable && materialState->evalTable->resource ? materialState->evalTable->resource->GetGlobalResourceID() : 0u,
				materialState->openPbrTable && materialState->openPbrTable->resource ? materialState->openPbrTable->resource->GetGlobalResourceID() : 0u,
				materialState->baseTable ? materialState->baseTable->elementCount : 0u,
				materialState->evalTable ? materialState->evalTable->elementCount : 0u,
				materialState->openPbrTable ? materialState->openPbrTable->elementCount : 0u,
				materialState->activeCompileFlags.size(), compileFlagMapping);
		}

        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        auto argBuf = m_materialEvalCmds->GetAPIResource();
        RefreshDescriptorIndices();

        const bool terrainRegionMaterialEvaluation =
            m_services.settings->getSettingGetter<bool>("enableTerrainRegionMaterialEvaluation")();
        const auto outputType = m_services.settings->getSettingGetter<unsigned int>("outputType")();
		for (std::size_t activeIndex = 0; activeIndex < materialState->activeCompileFlags.size(); ++activeIndex) {
            const MaterialCompileFlags flags = materialState->activeCompileFlags[activeIndex];
            if (terrainRegionMaterialEvaluation &&
                (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0) {
                continue;
            }
            if (activeIndex >= materialState->activeCompileFlagSlots.size()) {
                continue;
            }
            const unsigned int slot = materialState->activeCompileFlagSlots[activeIndex];
            if (slot >= materialState->compileFlagSlotsUsed) continue;
            MaterialCompileFlags shaderKey = GetMaterialEvaluationShaderKey(flags);
            if (outputType == OutputType::COLOR) {
                shaderKey |= MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
            }
            const PipelineState* pso = psoMgr.TryGetMaterialEvalPSO(shaderKey);
            if (!pso) {
                continue;
            }

            cl.BindPipeline(pso->GetAPIPipelineState().GetHandle());
            BindMaterialResourceDescriptorIndices(cl, pso->GetResourceDescriptorSlots());

            // Set per-pass root constants
            unsigned int miscRootConstants[NumMiscUintRootConstants] = {};
            miscRootConstants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClusterBufferSRVIndex;
            miscRootConstants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = m_visibleClusterTransformIndicesBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_reyesDiceQueueBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_reyesTessTableConfigsBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_reyesTessTableVerticesBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_reyesTessTableTrianglesBufferSRVIndex;
            miscRootConstants[VISBUF_REYES_USE_NORMAL_MAPS] = CLodReyesUseNormalMaps() ? 1u : 0u;
            miscRootConstants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesTerrainNormalBlend());
            miscRootConstants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = CLodReyesTerrainNormalMipBias();
            miscRootConstants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesObjectNormalMapBlend());
            miscRootConstants[VISBUF_MATERIAL_PIXEL_TELEMETRY_ENABLED] = m_materialPixelTelemetryEnabled ? 1u : 0u;
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
        m_visibleClusterTransformIndicesQuery = {};
        m_reyesDiceQueueQuery = {};
        m_reyesTessTableConfigsQuery = {};
        m_reyesTessTableVerticesQuery = {};
        m_reyesTessTableTrianglesQuery = {};
        m_slabResourceGroup.reset();
        m_materialEvalCmds = nullptr;
    }

private:
    ProducerPassServices& m_services;
	std::array<unsigned int, 3> m_publishedMaterialDescriptors{
		0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	std::uint64_t m_lastLoggedMaterialRevision = std::numeric_limits<std::uint64_t>::max();
    bool m_materialPixelTelemetryEnabled = false;
    void BindMaterialResourceDescriptorIndices(
        rhi::CommandList& commandList,
        const PipelineResources& resources) {
        unsigned int indices[org::shaderapi::kNumResourceDescriptorIndicesRootConstants] = {};
        int indexCount = 0;
        for (const auto& binding : resources.mandatoryResourceDescriptorSlots) {
            const bool allowMissing =
                !m_terrainRvtEnabled && binding.name.starts_with("Builtin::Terrain::Rvt");
			unsigned int descriptor =
				m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(binding, allowMissing);
			if (binding.name == Builtin::PerMaterialDataBuffer) descriptor = m_publishedMaterialDescriptors[0];
			else if (binding.name == "Builtin::PerMaterialEvalDataBuffer") descriptor = m_publishedMaterialDescriptors[1];
			else if (binding.name == Builtin::PerMaterialOpenPBRDataBuffer) descriptor = m_publishedMaterialDescriptors[2];
			indices[indexCount++] = descriptor;
        }
        for (const auto& binding : resources.optionalResourceDescriptorSlots) {
            indices[indexCount++] =
                m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(binding, true);
        }
        if (indexCount > 0) {
            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                org::shaderapi::kResourceDescriptorIndicesRootParameter,
                0,
                indexCount,
                indices);
        }
    }

    bool m_terrainRvtEnabled = false;
    Resource* m_materialEvalCmds;
    flecs::query<> m_visibleClustersQuery;
    flecs::query<> m_visibleClusterTransformIndicesQuery;
    flecs::query<> m_reyesDiceQueueQuery;
    flecs::query<> m_reyesTessTableConfigsQuery;
    flecs::query<> m_reyesTessTableVerticesQuery;
    flecs::query<> m_reyesTessTableTrianglesQuery;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    GloballyIndexedResource* m_visibleClusterResource = nullptr;
    GloballyIndexedResource* m_visibleClusterTransformIndicesResource = nullptr;
    GloballyIndexedResource* m_reyesDiceQueueResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableConfigsResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableVerticesResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableTrianglesResource = nullptr;
    uint32_t m_visibleClusterBufferSRVIndex = 0;
    uint32_t m_visibleClusterTransformIndicesBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesDiceQueueBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_reyesTessTableConfigsBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableVerticesBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableTrianglesBufferSRVIndex = 0xFFFFFFFFu;
};
