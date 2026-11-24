#pragma once

// CORTAO screen-space ambient occlusion (https://github.com/theagentd/CORTAO, MIT): prepass, main pass and filter pass
// (docs/lighting.md "Ambient occlusion"). They fill the screen-space AO slot (GtaoTechnique) when ambientOcclusionTechnique
// is CORTAO, and write Builtin::CORTAO::OutputAO: the world-space bent normal and the visibility.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>

#include <DirectXMath.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Render/Runtime/IDescriptorService.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "PostProcessing/AmbientOcclusion/AmbientOcclusionSettings.h"
#include "PostProcessing/AmbientOcclusion/CORTAOLookupTables.h"
#include "Utilities/Utilities.h"
#include "ThirdParty/CORTAO/CORTAOParameters.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>

namespace br::ambient_occlusion {

// Builtin::CORTAO::ConstantsBuffer: CORTAO's three parameter structs back to back, mirrored by CORTAOConstants in
// shaders/Include/cortaoCommon.hlsli.
struct CORTAOConstants {
    CORTAO::CORTAOPrepassParameters prepass;
    CORTAO::CORTAOMainPassParameters mainPass;
    CORTAO::CORTAOFilterPassParameters filter;
    uint32_t padding = 0;
};
static_assert(offsetof(CORTAOConstants, mainPass) == 32, "cortaoCommon.hlsli expects the main pass parameters at 32 bytes.");
static_assert(offsetof(CORTAOConstants, filter) == 112, "cortaoCommon.hlsli expects the filter parameters at 112 bytes.");
static_assert(sizeof(CORTAOConstants) == 128, "cortaoCommon.hlsli expects 128 bytes of CORTAO constants.");

// The prepass writes at most 8 mips (CORTAO's limit); a small target has fewer.
inline uint32_t CortaoMipCount(DirectX::XMUINT2 resolution) {
    uint32_t largest = std::max(1u, std::max(resolution.x, resolution.y));
    uint32_t mips = 1;
    while (largest > 1u) { largest >>= 1u; ++mips; }
    return std::min(mips, 8u);
}

inline DirectX::XMUINT2 CortaoRenderResolution() {
    return SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
}

// CORTAO has no temporal filter of its own: it leans on the upscaler's. The seed moves every frame only while one runs, and
// without one the larger spatial kernel is used instead.
inline bool CortaoTemporalUpscalerActive() {
    return UpscalingManager::GetInstance().GetCurrentUpscalingMode() != UpscalingMode::None;
}

inline uint32_t CortaoRandomSeed(uint64_t frameNumber) {
    if (!CortaoTemporalUpscalerActive()) return 0x2545F491u;
    uint32_t n = static_cast<uint32_t>(frameNumber) * 0x9E3779B9u + 0x7F4A7C15u;
    n ^= n >> 16u; n *= 0x85EBCA6Bu; n ^= n >> 13u; n *= 0xC2B2AE35u; n ^= n >> 16u;
    return n;
}

inline uint32_t CreateCortaoPointSampler(org::runtime::IDescriptorService& descriptors, rhi::AddressMode addressMode) {
    rhi::SamplerDesc desc;
    desc.minFilter = rhi::Filter::Nearest;
    desc.magFilter = rhi::Filter::Nearest;
    desc.mipFilter = rhi::MipFilter::Nearest;
    desc.addressU = addressMode;
    desc.addressV = addressMode;
    desc.addressW = addressMode;
    desc.mipLodBias = 0.0f;
    desc.maxAnisotropy = 1;
    desc.compareEnable = false;
    desc.borderPreset = rhi::BorderPreset::TransparentBlack; // Off-screen packed data reads 0: no occluder.
    desc.minLod = 0.0f;
    desc.maxLod = 16.0f;
    return descriptors.CreateIndexedSampler(desc);
}

inline br::ComputePipelineDesc CortaoPipeline(const wchar_t* path, const wchar_t* entry, std::string name,
    std::vector<br::PipelineShaderDefine> defines = {}) {
    br::ComputePipelineDesc desc;
    desc.shader = { path, entry, L"cs_6_7" }; // Integer texture sampling and gathers.
    desc.name = std::move(name);
    desc.defines = std::move(defines);
    desc.layout = br::PipelineLayoutRef::StandardGraphics();
    return desc;
}

// ---------------------------------------------------------------------------------------------------------------------
// Prepass: linear depth and a packed normal, mips 0 to mipCount - 1. It also uploads the frame's constants for all three.

struct CORTAOPrepassFrame {
    uint32_t groupsX = 0, groupsY = 0;
};
struct CORTAOPrepassBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken depth, normals;
    std::array<org::DeclaredViewToken, 8> packedMips;
    uint32_t mipCount = 1;
};

class CORTAOPrepass : public org::TypedRenderGraphPass<CORTAOPrepass, CORTAOPrepassFrame, CORTAOPrepassBindings> {
public:
    void Initialize() {
        m_constantsHandle = m_resourceRegistryView->RequestHandle(Builtin::CORTAO::ConstantsBuffer);
    }

    void Update(const org::UpdateExecutionContext& updateExecutionContext) override {
        const auto* update = updateExecutionContext.hostData->Get<UpdateContext>();
        if (update == nullptr || !update->hasPrimaryCamera) {
            return;
        }
        auto& settings = SettingsManager::GetInstance();
        const auto& camera = update->primaryCamera.info;

        CORTAO::CORTAOConfigurationParameters configuration{};
        configuration.width = update->renderResolution.x;
        configuration.height = update->renderResolution.y;
        configuration.mipCount = CortaoMipCount(update->renderResolution);
        configuration.randomSeed = CortaoRandomSeed(update->frameNumber);
        // D3D texture v runs down while NDC y runs up.
        configuration.flipY = true;
        configuration.isPerspective = true;
        // Row-vector matrices, as stored: the off-centre (jitter) terms are in [2][0] and [2][1], where CORTAO reads them.
        DirectX::XMFLOAT4X4 projection, inverseProjection;
        DirectX::XMStoreFloat4x4(&projection, camera.jitteredProjection);
        DirectX::XMStoreFloat4x4(&inverseProjection, camera.projectionInverse);
        std::memcpy(configuration.projectionMatrix, projection.m, sizeof(configuration.projectionMatrix));
        std::memcpy(configuration.inverseProjectionMatrix, inverseProjection.m, sizeof(configuration.inverseProjectionMatrix));
        configuration.nearPlane = camera.zNear;
        configuration.farPlane = std::numeric_limits<float>::infinity();
        configuration.maxRadius = std::max(1.0e-4f, settings.getSettingGetter<float>(CortaoRadiusSettingName)());
        configuration.maxScreenspaceRadiusHeightRatio = std::clamp(settings.getSettingGetter<float>(CortaoScreenRadiusRatioSettingName)(), 0.01f, 1.0f);
        configuration.mipBias = settings.getSettingGetter<float>(CortaoMipBiasSettingName)();
        configuration.maxSlopeCompensation = std::max(1.0f, settings.getSettingGetter<float>(CortaoSlopeCompensationSettingName)());
        configuration.backfaceThreshold = settings.getSettingGetter<float>(CortaoBackfaceThresholdSettingName)();
        configuration.backfaceBoost = std::clamp(settings.getSettingGetter<float>(CortaoBackfaceBoostSettingName)(), 0.0f, 1.0f);
        configuration.filterDepthSensitivity = std::max(0.0f, settings.getSettingGetter<float>(CortaoFilterDepthSensitivitySettingName)());
        configuration.filterNormalSensitivity = std::max(0.0f, settings.getSettingGetter<float>(CortaoFilterNormalSensitivitySettingName)());

        CORTAOConstants constants{};
        CORTAO::populatePrepassParameters(configuration, &constants.prepass);
        CORTAO::populateMainPassParameters(configuration, &constants.mainPass);
        CORTAO::populateFilterPassParameters(configuration, &constants.filter);
        // The prepass reads the linear depth map, so cortao_loadDepth hands it 1 / linear depth (0 for the sky) and the
        // prepass's linearization, 1 / (depth * scale + offset), is the identity.
        constants.prepass.depthScale = 1.0f;
        constants.prepass.depthOffset = 0.0f;

        UploadBufferData(&constants, sizeof(constants), org::runtime::UploadTarget::FromHandle(m_constantsHandle), 0);
    }

    CORTAOPrepassBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::CORTAO::ConstantsBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        CORTAOPrepassBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(CortaoPipeline(L"shaders/CORTAOPrepass.hlsl", L"CSCortaoPrepass", "CORTAO Prepass"));
        bindings.depth = builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })).View();
        bindings.normals = builder.ShaderResource(Builtin::Surface::NormalRoughness).View();
        bindings.mipCount = CortaoMipCount(CortaoRenderResolution());
        std::array<org::UavView, 8> views{};
        for (uint32_t mip = 0; mip < bindings.mipCount; ++mip) views[mip].mip = mip;
        auto packed = builder.UnorderedAccess(Builtin::CORTAO::PackedData, std::span<const org::UavView>(views.data(), bindings.mipCount));
        for (uint32_t mip = 0; mip < bindings.mipCount; ++mip) bindings.packedMips[mip] = packed.View(mip);
        return bindings;
    }

    CORTAOPrepassFrame Prepare(const CORTAOPrepassBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        return { (update->renderResolution.x + 7u) / 8u, (update->renderResolution.y + 7u) / 8u };
    }

    static void Record(const CORTAOPrepassBindings& bindings, const CORTAOPrepassFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[UintRootConstant0] = context.Index(bindings.depth);
        constants[UintRootConstant1] = context.Index(bindings.normals);
        // Mips past mipCount are never written (the prepass stops at mipCount); they repeat the last one.
        for (uint32_t mip = 0; mip < 8u; ++mip) {
            constants[UintRootConstant2 + mip] = context.Index(bindings.packedMips[std::min(mip, bindings.mipCount - 1u)]);
        }
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

private:
    org::ResourceRegistry::RegistryHandle m_constantsHandle;
};

// ---------------------------------------------------------------------------------------------------------------------
// Main pass: the occlusion rays, CORTAO_SAMPLE_COUNT samples per pixel (the cortaoSampleCount setting, one pipeline per
// preset). Owns the lookup tables, uploaded once.

struct CORTAOMainFrame {
    uint32_t groupsX = 0, groupsY = 0;
};
struct CORTAOMainBindings {
    br::PipelineState pipeline;
    uint32_t borderSampler = 0;
    org::DeclaredViewToken packed, normals, occluderTable, bentNormalTable, output;
};

class CORTAOMainPass : public org::TypedRenderGraphPass<CORTAOMainPass, CORTAOMainFrame, CORTAOMainBindings> {
public:
    CORTAOMainPass() {
        m_borderSampler = CreateCortaoPointSampler(DescriptorService(), rhi::AddressMode::Border);
        const auto occluderWords = CortaoOccluderLookupTableWords();
        const auto bentNormalWords = CortaoBentNormalLookupTableWords();
        m_occluderTable = CreateIndexedStructuredBuffer(occluderWords.size(), sizeof(uint32_t));
        m_occluderTable->SetName("CORTAO occluder lookup table");
        m_bentNormalTable = CreateIndexedStructuredBuffer(bentNormalWords.size(), sizeof(uint32_t));
        m_bentNormalTable->SetName("CORTAO bent normal lookup table");
    }

    void Update(const org::UpdateExecutionContext&) override {
        if (m_tablesUploaded) return;
        const auto occluderWords = CortaoOccluderLookupTableWords();
        const auto bentNormalWords = CortaoBentNormalLookupTableWords();
        UploadBufferData(occluderWords.data(), occluderWords.size_bytes(), org::runtime::UploadTarget::FromShared(m_occluderTable), 0);
        UploadBufferData(bentNormalWords.data(), bentNormalWords.size_bytes(), org::runtime::UploadTarget::FromShared(m_bentNormalTable), 0);
        m_tablesUploaded = true;
    }

    CORTAOMainBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::CORTAO::ConstantsBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        const uint32_t sampleCount = ClampCortaoSampleCount(
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(CortaoSampleCountSettingName)());
        CORTAOMainBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(CortaoPipeline(L"shaders/CORTAOMain.hlsl", L"CSCortaoMain",
            "CORTAO Main (" + std::to_string(sampleCount) + " samples)",
            { { L"CORTAO_SAMPLE_COUNT", std::to_wstring(sampleCount) } }));
        bindings.borderSampler = m_borderSampler;
        bindings.packed = builder.ShaderResource(Builtin::CORTAO::PackedData).View();
        bindings.normals = builder.ShaderResource(Builtin::Surface::NormalRoughness).View();
        bindings.occluderTable = builder.ShaderResource(m_occluderTable).View();
        bindings.bentNormalTable = builder.ShaderResource(m_bentNormalTable).View();
        bindings.output = builder.UnorderedAccess(Builtin::CORTAO::WorkingAO).View();
        return bindings;
    }

    CORTAOMainFrame Prepare(const CORTAOMainBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        return { (update->renderResolution.x + 15u) / 16u, (update->renderResolution.y + 15u) / 16u };
    }

    static void Record(const CORTAOMainBindings& bindings, const CORTAOMainFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[UintRootConstant0] = context.Index(bindings.packed);
        constants[UintRootConstant1] = context.Index(bindings.normals);
        constants[UintRootConstant2] = context.Index(bindings.occluderTable);
        constants[UintRootConstant3] = context.Index(bindings.bentNormalTable);
        constants[UintRootConstant4] = bindings.borderSampler;
        constants[UintRootConstant5] = context.Index(bindings.output);
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

private:
    uint32_t m_borderSampler = 0;
    std::shared_ptr<org::Buffer> m_occluderTable;
    std::shared_ptr<org::Buffer> m_bentNormalTable;
    bool m_tablesUploaded = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// Filter pass: the sparse blur, 2x2 pixels per thread, into Builtin::CORTAO::OutputAO. The large kernel runs when no
// upscaler accumulates the noise.

struct CORTAOFilterFrame {
    bool largeKernel = false;
    uint32_t groupsX = 0, groupsY = 0;
};
struct CORTAOFilterBindings {
    br::PipelineState smallPipeline, largePipeline;
    uint32_t borderSampler = 0;
    org::DeclaredViewToken packed, working, output;
};

class CORTAOFilterPass : public org::TypedRenderGraphPass<CORTAOFilterPass, CORTAOFilterFrame, CORTAOFilterBindings> {
public:
    CORTAOFilterPass() {
        m_borderSampler = CreateCortaoPointSampler(DescriptorService(), rhi::AddressMode::Border);
    }

    CORTAOFilterBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::CORTAO::ConstantsBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        CORTAOFilterBindings bindings{};
        bindings.smallPipeline = br::render::DeclarePipeline(CortaoPipeline(L"shaders/CORTAOFilter.hlsl", L"CSCortaoFilter", "CORTAO Filter",
            { { L"CORTAO_LARGE_FILTER", L"0" } }));
        bindings.largePipeline = br::render::DeclarePipeline(CortaoPipeline(L"shaders/CORTAOFilter.hlsl", L"CSCortaoFilter", "CORTAO Filter (large)",
            { { L"CORTAO_LARGE_FILTER", L"1" } }));
        bindings.borderSampler = m_borderSampler;
        bindings.packed = builder.ShaderResource(Builtin::CORTAO::PackedData).View();
        bindings.working = builder.ShaderResource(Builtin::CORTAO::WorkingAO).View();
        bindings.output = builder.UnorderedAccess(Builtin::CORTAO::OutputAO).View();
        return bindings;
    }

    CORTAOFilterFrame Prepare(const CORTAOFilterBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        // Thread t writes the 2x2 block ending at pixel 2t + 1, so res / 2 + 1 threads cover a dimension.
        CORTAOFilterFrame frame{};
        frame.largeKernel = !CortaoTemporalUpscalerActive();
        frame.groupsX = (update->renderResolution.x / 2u + 1u + 7u) / 8u;
        frame.groupsY = (update->renderResolution.y / 2u + 1u + 7u) / 8u;
        return frame;
    }

    static void Record(const CORTAOFilterBindings& bindings, const CORTAOFilterFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[UintRootConstant0] = context.Index(bindings.packed);
        constants[UintRootConstant1] = context.Index(bindings.working);
        constants[UintRootConstant2] = bindings.borderSampler;
        constants[UintRootConstant3] = context.Index(bindings.output);
        br::render::DispatchCompute(context, frame.largeKernel ? bindings.largePipeline : bindings.smallPipeline, constants,
            frame.groupsX, frame.groupsY);
    }

private:
    uint32_t m_borderSampler = 0;
};

}
