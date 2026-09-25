#pragma once

#include <unordered_map>
#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
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
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Render/IndirectStateArtifacts.h"
#include "../../shaders/PerPassRootConstants/amplificationShaderRootConstants.h"
#include "boost/container_hash/hash.hpp"

namespace br::render {
struct PreparedForwardIndirect {
    struct Draw {
        org::PreparedProgramBinding program{};
        org::PreparedResourceReference arguments{};
        uint64_t countOffset = 0;
        uint32_t maximumCount = 0;
    };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    rhi::CommandSignatureHandle commandSignature{};
    org::PreparedDescriptorReference color{}, depth{};
    DirectX::XMUINT2 resolution{};
    std::array<unsigned int, 3> settings{};
    std::vector<Draw> draws;
    bool enabled = false;
};

inline void RecordPreparedForwardIndirect(const PreparedForwardIndirect& data, org::RecordingContext& recording) {
    if (!data.enabled) return;
    auto& commands = recording.Commands();
    if (data.resourceHeap.valid())
        commands.SetDescriptorHeaps(data.resourceHeap,
            data.samplerHeap.valid() ? std::optional{data.samplerHeap} : std::nullopt);
    rhi::ColorAttachment color{}; color.rtv = recording.Resolve(data.color); color.loadOp = rhi::LoadOp::Load; color.storeOp = rhi::StoreOp::Store;
    rhi::DepthAttachment depth{}; depth.dsv = recording.Resolve(data.depth); depth.depthLoad = rhi::LoadOp::Load; depth.depthStore = rhi::StoreOp::Store;
    depth.stencilLoad = rhi::LoadOp::DontCare; depth.stencilStore = rhi::StoreOp::DontCare;
    rhi::PassBeginInfo pass{}; pass.colors = {&color, 1}; pass.depth = &depth;
    pass.width = data.resolution.x; pass.height = data.resolution.y; pass.debugName = "Forward Render Pass";
    commands.BeginPass(pass); commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList); commands.BindLayout(data.layout);
    commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, MiscEnableShadows, 3, data.settings.data());
    for (const auto& draw : data.draws) {
        commands.BindLayout(recording.ResolveLayout(draw.program.program));
        commands.BindPipeline(recording.Resolve(draw.program.program));
        if (!draw.program.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::AllGraphics, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(draw.program.descriptorIndices.size()), draw.program.descriptorIndices.data());
        const auto arguments = recording.Resolve(draw.arguments).GetHandle();
        commands.ExecuteIndirect(data.commandSignature, arguments, 0, arguments,
            draw.countOffset, draw.maximumCount);
    }
    commands.EndPass();
}
}

struct ForwardRenderPassInputs {
    bool wireframe;
    bool meshShaders;
    bool indirect;

    RG_DEFINE_PASS_INPUTS(ForwardRenderPassInputs, &ForwardRenderPassInputs::wireframe, &ForwardRenderPassInputs::meshShaders, &ForwardRenderPassInputs::indirect);
};


struct ForwardRenderBindings {
    org::ResourceBindingToken color, depth;
};

class ForwardRenderPass
    : public org::TypedRenderGraphPass<ForwardRenderPass,
        br::render::PreparedForwardIndirect, ForwardRenderBindings> {
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
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
            Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement)
            .IsGeometryPass();

        ForwardRenderBindings bindings{
            builder->BindRenderTarget(Builtin::Color::HDRColorTarget),
            builder->BindDepthReadWrite(Builtin::PrimaryCamera::DepthTexture) };

        if (m_shadowsEnabled) {
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
                br::render::PublishedResourceQuery query{};
                query.owner = br::render::PublishedFragmentKind::IndirectWorkloads;
                query.usage = br::render::PublishedResourceUsage::IndirectArguments;
                query.renderPhaseHash = RenderPhase{ Engine::Primary::ForwardPass }.hash;
                builder->WithIndirectArguments(PublishedStateResourceResolver(
                    br::render::PublishedStateSource::ProcessSource(), query));
            }
        }
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
        return bindings;
    }

    void Initialize() {
        RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
        if (m_shadowsEnabled) {
            RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
        }

        //if (m_meshShaders)
            //m_primaryCameraMeshletBitfield = m_resourceRegistryView->RequestPtr<DynamicGloballyIndexedResource>(Builtin::PrimaryCamera::MeshletBitfield);
    }

    br::render::PreparedForwardIndirect Prepare(const ForwardRenderBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto published = context->publishedRendererState
            ? context->publishedRendererState->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>() : nullptr;
        if (!published) return {};
        br::render::PreparedForwardIndirect data{};
        data.enabled = true;
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetRootSignature().GetHandle();
        data.commandSignature = preparation.CaptureCommandSignature(
            CommandSignatureManager::GetInstance().CaptureDispatchMeshCommandSignature());
        data.color = preparation.CaptureView(bindings.color, {org::BindlessViewKind::RenderTarget});
        data.depth = preparation.CaptureView(bindings.depth, {org::BindlessViewKind::DepthStencil});
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
            auto payload = PSOManager::GetInstance().GetMeshPSO(
                context->globalPSOFlags, workload->key.compileFlags, m_wireframe).GetPayload();
            br::render::PreparedForwardIndirect::Draw draw{};
            draw.program = preparation.CaptureProgramBinding(std::move(payload));
            draw.arguments = preparation.CaptureResource(workload->indirectArguments->GetGlobalResourceID());
            draw.countOffset = workload->indirectArguments->GetUAVCounterOffset(); draw.maximumCount = workload->count;
            data.draws.push_back(std::move(draw));
        }
        return data;
    }
    static void Record(const ForwardRenderBindings&, const br::render::PreparedForwardIndirect& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedForwardIndirect(data, recording);
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
