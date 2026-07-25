#pragma once

#include <unordered_map>
#include <functional>

#include "RenderPasses/Base/RenderPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Mesh/Mesh.h"
#include "Scene/Scene.h"
#include "Materials/Material.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/MeshManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Mesh/MeshInstance.h"
#include "Managers/LightManager.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/ECSResourceResolver.h"
#include "../../shaders/PerPassRootConstants/amplificationShaderRootConstants.h"
#include "boost/container_hash/hash.hpp"

struct ForwardRenderPassInputs {
    bool wireframe;
    bool meshShaders;
    bool indirect;

    RG_DEFINE_PASS_INPUTS(ForwardRenderPassInputs, &ForwardRenderPassInputs::wireframe, &ForwardRenderPassInputs::meshShaders, &ForwardRenderPassInputs::indirect);
};


class ForwardRenderPass : public RenderPass {
public:
    ForwardRenderPass()
    {
        auto& settingsManager = SettingsManager::GetInstance();
        getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");
        getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
        getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
        m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
        m_clusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting")();
    }

    ~ForwardRenderPass() {
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
		auto inputs = Inputs<ForwardRenderPassInputs>();
		m_wireframe = inputs.wireframe;
		m_meshShaders = inputs.meshShaders;
		m_indirect = inputs.indirect;

        builder->WithShaderResource(
            Builtin::CameraBuffer,
            Builtin::Environment::PrefilteredCubemapsGroup,
            Builtin::Light::ActiveLightIndices,
            Builtin::Light::InfoBuffer,
            Builtin::Light::PointLightCubemapBuffer,
            Builtin::Light::DirectionalLightCascadeBuffer,
            Builtin::Light::SpotLightMatrixBuffer,
            Builtin::Environment::InfoBuffer,
            Builtin::Environment::CurrentCubemap,
            Builtin::NormalMatrixBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureGroup,
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
            Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement)
            .WithRenderTarget(Builtin::Color::HDRColorTarget)
            .WithDepthReadWrite(Builtin::PrimaryCamera::DepthTexture)
            .IsGeometryPass();

        if (getShadowsEnabled()) {
            builder->WithShaderResource(Builtin::Shadows::CLodClipmapInfo,
                Builtin::Shadows::CLodCompactMainCamera,
                Builtin::Shadows::CLodCompactShadowCameras,
                Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::Shadows::CLodPageMetadata,
                Builtin::Shadows::CLodPageTable,
                Builtin::Shadows::CLodPhysicalPages);
        }

        builder->WithUnorderedAccess(Builtin::DebugVisualization);
        if (m_clusteredLightingEnabled) {
            builder->WithShaderResource(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer);
        }

        if (m_gtaoEnabled) {
            builder->WithShaderResource(Builtin::GTAO::OutputAOTerm);
        }
        if (m_meshShaders) {
            //builder->WithShaderResource(MESH_RESOURCE_IDFENTIFIERS, Builtin::PrimaryCamera::MeshletBitfield);
            if (m_indirect) { // Indirect draws only supported with mesh shaders, becasue I'm not writing a separate codepath for doing it the bad way
                auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
                auto forwardPassEntity = RendererECSManager::GetInstance().GetRenderPhaseEntity(Engine::Primary::ForwardPass);
				flecs::query<> indirectQuery = ecsWorld.query_builder<>()
                    .with<Components::IsIndirectArguments>()
					.with<Components::ParticipatesInPass>(forwardPassEntity) // Query for command lists that participate in this pass
                    //.cached().cache_kind(flecs::QueryCacheAll)
                    .build();
                builder->WithIndirectArguments(ECSResourceResolver(indirectQuery));
            }
        }
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
        RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
        if (getShadowsEnabled()) {
            RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
        }

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
        m_meshInstancesQuery = ecsWorld.query_builder<Components::ObjectDrawInfo, Components::PerPassMeshes>()
            .with<Components::ParticipatesInPass>(RendererECSManager::GetInstance().GetRenderPhaseEntity(Engine::Primary::ForwardPass))
            .cached().cache_kind(flecs::QueryCacheAll)
            .build();

        // Setup resources
        m_pPrimaryDepthBuffer = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::DepthTexture);
        m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Color::HDRColorTarget);

        //if (m_meshShaders)
            //m_primaryCameraMeshletBitfield = m_resourceRegistryView->RequestPtr<DynamicGloballyIndexedResource>(Builtin::PrimaryCamera::MeshletBitfield);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;

        SetupCommonState(context, commandList);
        SetCommonRootConstants(context, commandList);


        if (m_meshShaders) {
            if (m_indirect) {
                // Indirect drawing
                ExecuteMeshShaderIndirect(context, commandList);
            }
            else {
                // Regular mesh shader drawing
                ExecuteMeshShader(context, commandList);
            }
        }
        else {
            // Regular forward rendering
            ExecuteRegular(context, commandList);
        }
        return {};
    }

    void Cleanup() override {
    }

private:
    // Common setup code that doesn't change between techniques
    void SetupCommonState(const RenderContext& context, rhi::CommandList& commandList) {

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		rhi::PassBeginInfo passInfo{};
		rhi::ColorAttachment colorAttachment{};
		colorAttachment.rtv = m_pHDRTarget->GetRTVInfo(0).slot;
		colorAttachment.loadOp = rhi::LoadOp::Load;
		colorAttachment.storeOp = rhi::StoreOp::Store;
		colorAttachment.clear = m_pHDRTarget->GetClearColor();
		passInfo.colors = { &colorAttachment, 1 };
		rhi::DepthAttachment depthAttachment{};
		depthAttachment.dsv = m_pPrimaryDepthBuffer->GetDSVInfo(0).slot;
		depthAttachment.depthLoad = rhi::LoadOp::Load;
		depthAttachment.depthStore = rhi::StoreOp::Store;
		depthAttachment.stencilLoad = rhi::LoadOp::DontCare;
		depthAttachment.stencilStore = rhi::StoreOp::DontCare;
		depthAttachment.clear = m_pPrimaryDepthBuffer->GetClearColor();
		passInfo.depth = &depthAttachment;
		passInfo.width = context.renderResolution.x;
		passInfo.height = context.renderResolution.y;
		passInfo.debugName = "Forward Render Pass";
		commandList.BeginPass(passInfo);

        commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
		commandList.BindLayout(PSOManager::GetInstance().GetRootSignature().GetHandle());
    }

    void SetCommonRootConstants(const RenderContext& context, rhi::CommandList& commandList) {
        unsigned int settings[] = { getShadowsEnabled(), getPunctualLightingEnabled(), m_gtaoEnabled };

        if (m_meshShaders) {
            //misc[MESHLET_CULLING_BITFIELD_BUFFER_SRV_DESCRIPTOR_INDEX] = m_primaryCameraMeshletBitfield->GetResource()->GetSRVInfo(0).slot.index;
        }
		commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscEnableShadows, 3, settings);
    }

    void ExecuteRegular(const RenderContext& context, rhi::CommandList& commandList) {
        // Regular forward rendering using DrawIndexedInstanced
        auto& psoManager = PSOManager::GetInstance();

        m_meshInstancesQuery.each([&](flecs::entity e, Components::ObjectDrawInfo drawInfo, Components::PerPassMeshes meshInstancesComponent) {
			auto& meshes = meshInstancesComponent.meshesByPass[m_renderPhase.hash]; // Pull out only the meshes for this render phase

            unsigned int perObjectIndex = drawInfo.perObjectCBIndex;
            commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscPerObjectBufferIndex, 1, &perObjectIndex);

            for (auto& pMesh : meshes) {
                auto& mesh = *pMesh->GetMesh();
                auto& pso = psoManager.GetPSO(context.globalPSOFlags | mesh.material->GetPSOFlags(), mesh.material->Technique().compileFlags, m_wireframe);
                BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());
				commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());

                unsigned int perMeshIndices[] = {
                    static_cast<uint32_t>(mesh.GetPerMeshBufferView()->GetOffset() / sizeof(PerMeshCB)),
                    static_cast<uint32_t>(pMesh->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB))
                };
				commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscPerMeshBufferIndex, 2, perMeshIndices);

				//commandList.SetIndexBuffer(mesh.GetIndexBufferView());
				//commandList.DrawIndexed(mesh.GetIndexCount(), 1, 0, 0, 0);
            }
            });
    }

    void ExecuteMeshShader(const RenderContext& context, rhi::CommandList& commandList) {
        // Mesh shading path using DispatchMesh
        auto& psoManager = PSOManager::GetInstance();

        m_meshInstancesQuery.each([&](flecs::entity e, Components::ObjectDrawInfo drawInfo, Components::PerPassMeshes perPassMeshes) {
            auto& meshes = perPassMeshes.meshesByPass[m_renderPhase.hash];

            unsigned int perObjectIndex = drawInfo.perObjectCBIndex;
            commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscPerObjectBufferIndex, 1, &perObjectIndex);

            for (auto& pMesh : meshes) {
                auto& mesh = *pMesh->GetMesh();
                auto& pso = psoManager.GetMeshPSO(context.globalPSOFlags | mesh.material->GetPSOFlags(), mesh.material->Technique().compileFlags, m_wireframe);
                BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());
				commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());

                unsigned int perMeshIndices[] = {
                    static_cast<uint32_t>(mesh.GetPerMeshBufferView()->GetOffset() / sizeof(PerMeshCB)),
                    static_cast<uint32_t>(pMesh->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB))
                };
				commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscPerMeshBufferIndex, 2, perMeshIndices);

                // Mesh shaders use DispatchMesh
                //commandList.DispatchMesh(mesh.GetMeshletCount(), 1, 1);
            }
            });
    }

    void ExecuteMeshShaderIndirect(const RenderContext& context, rhi::CommandList& commandList) {
        // Mesh shading with ExecuteIndirect
        auto& psoManager = PSOManager::GetInstance();

        auto commandSignature = CommandSignatureManager::GetInstance().GetDispatchMeshCommandSignature();
		auto manager = context.indirectCommandBufferManager;

        auto primaryViewID = context.primaryViewID;
        auto workloads = manager->GetBuffersForRenderPhase(primaryViewID, Engine::Primary::ForwardPass);

		for (auto& workload : workloads) {

            auto& indirectBuffer = workload.second;
			auto materialCompileFlags = workload.first;
            auto& pso = psoManager.GetMeshPSO(context.globalPSOFlags, materialCompileFlags, m_wireframe);
            BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());
			commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());

            auto apiResource = workload.second.buffer->GetAPIResource();
            const auto commandCount = workload.second.count;
            if (commandCount == 0u) {
                continue;
            }
            if (const auto backing = std::dynamic_pointer_cast<Buffer>(workload.second.buffer->GetResource())) {
                const auto requiredBytes = static_cast<uint64_t>(commandCount) * sizeof(DispatchMeshIndirectCommand);
                if (backing->GetSize() < requiredBytes) {
                    spdlog::error(
                        "ForwardRenderPass: skipping indirect workload with undersized args flags={} count={} bytes={} required={}",
                        static_cast<uint64_t>(materialCompileFlags),
                        commandCount,
                        backing->GetSize(),
                        requiredBytes);
                    continue;
                }
            }

			commandList.ExecuteIndirect(
				commandSignature.GetHandle(), 
                apiResource.GetHandle(), 
                0, 
                apiResource.GetHandle(), 
                workload.second.buffer->GetResource()->GetUAVCounterOffset(),
                commandCount);
        }
    }

private:
    flecs::query<Components::ObjectDrawInfo, Components::PerPassMeshes> m_meshInstancesQuery;
    bool m_wireframe;
    bool m_meshShaders;
    bool m_indirect;
    bool m_gtaoEnabled = true;
    bool m_clusteredLightingEnabled = true;

	RenderPhase m_renderPhase = Engine::Primary::ForwardPass;

    DynamicGloballyIndexedResource* m_primaryCameraMeshletBitfield = nullptr;
    DynamicGloballyIndexedResource* m_primaryCameraMeshletCullingBitfieldBuffer = nullptr;
    PixelBuffer* m_pPrimaryDepthBuffer = nullptr;
    PixelBuffer* m_pHDRTarget = nullptr;

    std::function<bool()> getImageBasedLightingEnabled;
    std::function<bool()> getPunctualLightingEnabled;
    std::function<bool()> getShadowsEnabled;
};
