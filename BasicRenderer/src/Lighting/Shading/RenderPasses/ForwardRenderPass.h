#pragma once
#include <BasicRenderer/Extensions/IndirectCommand.h>

#include <unordered_map>
#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Scene/Scene.h"
#include "BasicRenderer/Assets/Material.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Scene/ECS/RendererECSManager.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/GraphIntegration/Resolvers/ECSResourceResolver.h"
#include <BasicRenderer/Extensions/Resources/PublishedStateResourceResolver.h>
#include "Scene/Objects/IndirectStateArtifacts.h"
#include "../../../../shaders/PerPassRootConstants/amplificationShaderRootConstants.h"
#include "boost/container_hash/hash.hpp"

struct ForwardRenderFrame {
    struct Draw {
        // The mesh pipeline for this workload's compile flags, and its indirect arguments (counted in place).
        std::shared_ptr<const org::PipelineStatePayload> pipeline;
        org::PreparedResourceReference arguments{};
        uint64_t countOffset = 0;
        uint32_t maximumCount = 0;
    };
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    DirectX::XMUINT2 resolution{};
    std::array<uint32_t, 3> settings{};
    std::vector<Draw> draws;
    bool enabled = false;
};

struct ForwardRenderPassInputs {
    bool wireframe;
    bool meshShaders;
    bool indirect;

    RG_DEFINE_PASS_INPUTS(ForwardRenderPassInputs, &ForwardRenderPassInputs::wireframe, &ForwardRenderPassInputs::meshShaders, &ForwardRenderPassInputs::indirect);
};


struct ForwardRenderBindings {
    org::DeclaredViewToken color, depth;
};

class ForwardRenderPass
    : public org::TypedRenderGraphPass<ForwardRenderPass,
        ForwardRenderFrame, ForwardRenderBindings> {
public:
    ForwardRenderPass()
    {
        auto& settingsManager = SettingsManager::GetInstance();
        m_imageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting")();
        m_punctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting")();
        m_shadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows")();
        m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
        m_clusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting")();
    }

    ~ForwardRenderPass() {
    }

    ForwardRenderBindings Declare(org::PassBuilder& declaration) {
		auto* builder = &declaration;
		auto inputs = Inputs<ForwardRenderPassInputs>();
		m_wireframe = inputs.wireframe;
		m_meshShaders = inputs.meshShaders;
		m_indirect = inputs.indirect;

        builder->ShaderResource(
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
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
            Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement);
        builder->ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        builder->IsGeometryPass();

        ForwardRenderBindings bindings{
            builder->RenderTarget(Builtin::Color::HDRColorTarget).View(),
            builder->DepthReadWrite(Builtin::PrimaryCamera::DepthTexture).View() };

        if (m_shadowsEnabled) {
            builder->ShaderResource(Builtin::Shadows::CLodClipmapInfo,
                Builtin::Shadows::CLodCompactMainCamera,
                Builtin::Shadows::CLodCompactShadowCameras,
                Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::Shadows::CLodPageMetadata,
                Builtin::Shadows::CLodPhysicalPages);
            builder->ShaderResource(Builtin::Shadows::CLodPageTable,
                org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        }

        builder->UnorderedAccess(Builtin::DebugVisualization);
        if (m_clusteredLightingEnabled) {
            builder->ShaderResource(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer);
        }

        if (m_gtaoEnabled) {
            builder->ShaderResource(Builtin::GTAO::OutputAOTerm);
        }
        if (m_meshShaders) {
            // Meshlet bitfield usage is declared by the selected mesh workload.
            if (m_indirect) { // Indirect draws only supported with mesh shaders, becasue I'm not writing a separate codepath for doing it the bad way
                br::render::PublishedResourceQuery query{};
                query.owner = br::render::PublishedFragmentKind::IndirectWorkloads;
                query.usage = br::render::PublishedResourceUsage::IndirectArguments;
                query.renderPhaseHash = RenderPhase{ Engine::Primary::ForwardPass }.hash;
                builder->IndirectArguments(PublishedStateResourceResolver(
                    br::render::PublishedStateSource::ProcessSource(), query));
            }
        }
		builder->ConstantBuffer(Builtin::PerFrameBuffer);
        return bindings;
    }

    void Initialize() {
        //if (m_meshShaders)
            //m_primaryCameraMeshletBitfield = m_resourceRegistryView->RequestPtr<DynamicGloballyIndexedResource>(Builtin::PrimaryCamera::MeshletBitfield);
    }

    ForwardRenderFrame Prepare(const ForwardRenderBindings&,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto published = context->publishedRendererState
            ? context->publishedRendererState->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>() : nullptr;
        if (!published) return {};
        ForwardRenderFrame data{};
        data.enabled = true;
        data.commandSignature = CommandSignatureManager::GetInstance().CaptureDispatchMeshCommandSignature();
        data.resolution = context->renderResolution;
        data.settings = {context->lighting.shadowsEnabled,
            context->lighting.punctualLightingEnabled, context->lighting.gtaoEnabled};
        if (!m_meshShaders || !m_indirect)
            return data;
        const auto workloads = published->Find(context->primaryViewID, Engine::Primary::ForwardPass, false);
        data.draws.reserve(workloads.size());
        for (const auto* workload : workloads) {
            if (!workload || !workload->indirectArguments || workload->count == 0u) continue;
            if (const auto backing = std::dynamic_pointer_cast<org::Buffer>(workload->indirectArguments)) {
                const auto requiredBytes = static_cast<uint64_t>(workload->count) * sizeof(DispatchMeshIndirectCommand);
                if (backing->GetSize() < requiredBytes) continue;
            }
            ForwardRenderFrame::Draw draw{};
            draw.pipeline = PSOManager::GetInstance().GetMeshPSO(
                context->globalPSOFlags, workload->key.compileFlags, m_wireframe).GetPayload();
            draw.arguments = preparation.CaptureResource(workload->indirectArguments->GetGlobalResourceID());
            draw.countOffset = workload->indirectArguments->GetUAVCounterOffset(); draw.maximumCount = workload->count;
            data.draws.push_back(std::move(draw));
        }
        return data;
    }
    static void Record(const ForwardRenderBindings& bindings, const ForwardRenderFrame& data,
        org::PassRecordContext& recording) {
        if (!data.enabled) return;
        auto& commands = recording.Commands();
        rhi::ColorAttachment color{}; color.rtv = recording.View(bindings.color); color.loadOp = rhi::LoadOp::Load; color.storeOp = rhi::StoreOp::Store;
        rhi::DepthAttachment depth{}; depth.dsv = recording.View(bindings.depth); depth.depthLoad = rhi::LoadOp::Load; depth.depthStore = rhi::StoreOp::Store;
        depth.stencilLoad = rhi::LoadOp::DontCare; depth.stencilStore = rhi::StoreOp::DontCare;
        rhi::PassBeginInfo pass{}; pass.colors = {&color, 1}; pass.depth = &depth;
        pass.width = data.resolution.x; pass.height = data.resolution.y; pass.debugName = "Forward Render Pass";
        commands.BeginPass(pass); commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
        if (!data.draws.empty()) recording.Retain(data.commandSignature);
        for (const auto& draw : data.draws) {
            recording.BindGraphicsPipeline(draw.pipeline);
            recording.PushConstants(MiscUintRootSignatureIndex, data.settings, MiscEnableShadows);
            const auto arguments = recording.Resolve(draw.arguments).GetHandle();
            commands.ExecuteIndirect(data.commandSignature->Get().GetHandle(), arguments, 0, arguments,
                draw.countOffset, draw.maximumCount);
        }
        commands.EndPass();
    }

private:
    bool m_wireframe;
    bool m_meshShaders;
    bool m_indirect;
    bool m_gtaoEnabled = true;
    bool m_clusteredLightingEnabled = true;

    bool m_imageBasedLightingEnabled = true;
    bool m_punctualLightingEnabled = true;
    bool m_shadowsEnabled = true;
};
