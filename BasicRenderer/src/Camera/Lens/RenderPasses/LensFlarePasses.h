#pragma once

#include <bit>
#include <memory>
#include <string_view>

#include "BuiltinResources.h"
#include "Camera/Lens/GhostFlareManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingPasses.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "../shaders/Include/lens/lensGhostLayout.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// Ghost flares (GhostFlareManager; SARP's docs/lens_simulation.md "Ghost flares"): four passes after the upscaler. Each one's
// Prepare takes the manager's frame configuration (one per frame, so they agree on the layout) and records nothing while no
// lens is on or its ghosts aren't built yet. The graph has them only while lensFlareEnabled is on.

inline constexpr std::string_view kLensFlareTargetName = "Builtin::LensFlare::Target";

struct LensFlareSourcesBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken sources, depth;
    bool rayQuery = false;
};

struct LensFlareSourcesFrame {
    br::lens::GhostFlareFrame config;
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    uint32_t targetWidth = 0, targetHeight = 0;
};

// The sources and sensor frame (lensFlareSources.hlsl). With a ray tracing system (path traced), visibility is by ray query.
class LensFlareSourcesPass : public org::TypedRenderGraphPass<LensFlareSourcesPass, LensFlareSourcesFrame, LensFlareSourcesBindings> {
public:
    LensFlareSourcesPass(std::shared_ptr<br::lens::GhostFlareManager> manager, std::shared_ptr<br::render::CLodRayTracingSystem> system,
        std::shared_ptr<org::ResourceGroup> slabs, uint32_t targetWidth, uint32_t targetHeight)
        : m_manager(std::move(manager)), m_system(std::move(system)), m_slabs(std::move(slabs)), m_targetWidth(targetWidth), m_targetHeight(targetHeight) {}

    LensFlareSourcesBindings Declare(org::PassBuilder& builder) {
        LensFlareSourcesBindings bindings{};
        bindings.rayQuery = m_system != nullptr;
        if (bindings.rayQuery) {
            auto desc = CLodRtComputePipeline(L"Shaders/PostProcessing/lensFlareSources.hlsl", L"LensFlareSourcesCS", "LensFlare.Sources.RayQuery.PSO");
            desc.defines.push_back({ L"LENS_FLARE_RAY_QUERY", L"1" });
            bindings.pipeline = builder.Pipeline(desc);
            br::render::CLodRayTracingSystem::DeclareTracing(builder);
            if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
            builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::PerMaterialDataBuffer,
                Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer,
                Builtin::CullingCameraBuffer, Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups, Builtin::CLod::AssemblyTransforms,
                Builtin::CLod::AssemblyBoneRemaps, Builtin::CLod::AssemblyBoneRemapIndices, Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::Material::TextureStreamingMetadataBuffer);
        }
        else {
            bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/PostProcessing/lensFlareSources.hlsl", L"LensFlareSourcesCS",
                "LensFlare.Sources.PSO"));
            bindings.depth = builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture).View();
        }
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::InfoBuffer, Builtin::Light::ActiveLightIndices, Builtin::RayTracing::LensData);
        // The frame's focus (LensFocusPass writes it before the path tracer; this runs after the upscaler).
        builder.ShaderResource(Builtin::RayTracing::LensFocus);
        builder.ShaderResource(org::ResourceIdentifier{ br::lens::kLensFlareGhostsName });
        bindings.sources = builder.UnorderedAccess(org::ResourceIdentifier{ br::lens::kLensFlareSourcesName }).View();
        return bindings;
    }

    LensFlareSourcesFrame Prepare(const LensFlareSourcesBindings&, const org::PassPrepareContext& preparation) const {
        LensFlareSourcesFrame frame{};
        const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        if (!context) return frame;
        frame.config = m_manager->FrameConfig(context->frameNumber);
        frame.system = context->clodRayTracingSystem;
        frame.targetWidth = m_targetWidth;
        frame.targetHeight = m_targetHeight;
        return frame;
    }

    static void Record(const LensFlareSourcesBindings& bindings, const LensFlareSourcesFrame& frame, org::PassRecordContext& recording) {
        const auto& config = frame.config;
        if (!config.active) return;
        br::render::MiscConstants constants{};
        constants[CLOD_RT_SEC_TLAS_SRV] = 0xFFFFFFFFu;
        constants[LENS_FLARE_RC_SOURCES_UAV] = recording.Index(bindings.sources);
        constants[LENS_FLARE_RC_DEPTH_SRV] = bindings.rayQuery ? 0xFFFFFFFFu : recording.Index(bindings.depth);
        constants[LENS_FLARE_RC_MAX_LIGHTS] = config.sourceSlots - 1u;
        constants[LENS_FLARE_RC_WAVELENGTHS] = config.wavelengthSlots;
        constants[LENS_FLARE_RC_GRID] = config.grid;
        constants[LENS_FLARE_RC_INTENSITY] = std::bit_cast<uint32_t>(config.intensity);
        constants[LENS_FLARE_RC_SELF_TEST] = static_cast<uint32_t>(config.selfTest);
        constants[LENS_FLARE_RC_TARGET_WIDTH] = frame.targetWidth;
        constants[LENS_FLARE_RC_TARGET_HEIGHT] = frame.targetHeight;
        constants[LENS_FLARE_RC_TEST_SOURCE] = config.testSource ? 1u : 0u;
        constants[LENS_FLARE_RC_TEST_X] = std::bit_cast<uint32_t>(config.testDirection[0]);
        constants[LENS_FLARE_RC_TEST_Y] = std::bit_cast<uint32_t>(config.testDirection[1]);
        constants[LENS_FLARE_RC_TEST_Z] = std::bit_cast<uint32_t>(config.testDirection[2]);
        constants[LENS_FLARE_RC_TEST_DISTANCE] = std::bit_cast<uint32_t>(config.testDistance);
        constants[LENS_FLARE_RC_TEST_R] = std::bit_cast<uint32_t>(config.testIrradiance[0]);
        constants[LENS_FLARE_RC_TEST_G] = std::bit_cast<uint32_t>(config.testIrradiance[1]);
        constants[LENS_FLARE_RC_TEST_B] = std::bit_cast<uint32_t>(config.testIrradiance[2]);
        constants[LENS_FLARE_RC_GHOSTS_SLOTS] = config.ghostSlots;
        if (bindings.rayQuery && frame.system) {
            // Without a TLAS yet the shader counts every source as seen.
            std::scoped_lock lock(frame.system->FrameOperationMutex());
            if (frame.system->HasTlas()) {
                constants[CLOD_RT_SEC_TLAS_SRV] = frame.system->GetTlasSrv();
                constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.system->GetClasInfoSrv();
                constants[CLOD_RT_SEC_INSTANCE_ROWS_SRV] = frame.system->GetInstanceRowsSrv();
            }
            br::render::DispatchCompute(recording, bindings.pipeline, constants, 1u);
            return;
        }
        br::render::DispatchCompute(recording, bindings.pipeline, constants, 1u);
    }

private:
    std::shared_ptr<br::lens::GhostFlareManager> m_manager;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    uint32_t m_targetWidth = 0, m_targetHeight = 0;
};

struct LensFlareTraceBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken vertices;
};

struct LensFlareTraceFrame {
    br::lens::GhostFlareFrame config;
};

// The lattice of rays per source, ghost and wavelength (lensFlareTrace.hlsl).
class LensFlareTracePass : public org::TypedRenderGraphPass<LensFlareTracePass, LensFlareTraceFrame, LensFlareTraceBindings> {
public:
    explicit LensFlareTracePass(std::shared_ptr<br::lens::GhostFlareManager> manager) : m_manager(std::move(manager)) {}

    LensFlareTraceBindings Declare(org::PassBuilder& builder) {
        LensFlareTraceBindings bindings{};
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/PostProcessing/lensFlareTrace.hlsl", L"LensFlareTraceCS", "LensFlare.Trace.PSO"));
        builder.ShaderResource(Builtin::RayTracing::LensData);
        builder.ShaderResource(org::ResourceIdentifier{ br::lens::kLensFlareGhostsName });
        builder.ShaderResource(org::ResourceIdentifier{ br::lens::kLensFlareSourcesName });
        bindings.vertices = builder.UnorderedAccess(m_manager->EnsureVertexBuffer()).View();
        return bindings;
    }

    LensFlareTraceFrame Prepare(const LensFlareTraceBindings&, const org::PassPrepareContext& preparation) const {
        LensFlareTraceFrame frame{};
        if (const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr) {
            frame.config = m_manager->FrameConfig(context->frameNumber);
        }
        return frame;
    }

    static void Record(const LensFlareTraceBindings& bindings, const LensFlareTraceFrame& frame, org::PassRecordContext& recording) {
        const auto& config = frame.config;
        if (!config.active) return;
        br::render::MiscConstants constants{};
        constants[LENS_FLARE_RC_VERTICES] = recording.Index(bindings.vertices);
        constants[LENS_FLARE_RC_SOURCE_SLOTS] = config.sourceSlots;
        constants[LENS_FLARE_RC_GHOSTS] = config.ghostSlots;
        constants[LENS_FLARE_RC_WAVELENGTHS] = config.wavelengthSlots;
        constants[LENS_FLARE_RC_GRID] = config.grid;
        const uint32_t vertices = (config.grid + 1u) * (config.grid + 1u);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (vertices + LENS_FLARE_TRACE_GROUP - 1u) / LENS_FLARE_TRACE_GROUP,
            config.InstanceCount());
    }

private:
    std::shared_ptr<br::lens::GhostFlareManager> m_manager;
};

struct LensFlareDrawBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken vertices, target;
};

struct LensFlareDrawFrame {
    br::lens::GhostFlareFrame config;
    uint32_t width = 0, height = 0;
};

// The lattice cells as quads, added into the flare target (lensFlareDraw.hlsl).
class LensFlareDrawPass : public org::TypedRenderGraphPass<LensFlareDrawPass, LensFlareDrawFrame, LensFlareDrawBindings> {
public:
    explicit LensFlareDrawPass(std::shared_ptr<br::lens::GhostFlareManager> manager) : m_manager(std::move(manager)) {}

    LensFlareDrawBindings Declare(org::PassBuilder& builder) {
        LensFlareDrawBindings bindings{};
        bindings.pipeline = builder.Pipeline(PipelineDesc());
        builder.ShaderResource(org::ResourceIdentifier{ br::lens::kLensFlareSourcesName });
        bindings.vertices = builder.ShaderResource(m_manager->EnsureVertexBuffer()).View();
        bindings.target = builder.RenderTarget(org::ResourceIdentifier{ kLensFlareTargetName });
        return bindings;
    }

    LensFlareDrawFrame Prepare(const LensFlareDrawBindings& bindings, const org::PassPrepareContext& preparation) const {
        LensFlareDrawFrame frame{};
        if (const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr) {
            frame.config = m_manager->FrameConfig(context->frameNumber);
        }
        const auto& description = preparation.Describe(bindings.target);
        frame.width = description.texture.width;
        frame.height = description.texture.height;
        return frame;
    }

    static void Record(const LensFlareDrawBindings& bindings, const LensFlareDrawFrame& frame, org::PassRecordContext& recording) {
        const auto& config = frame.config;
        if (!config.active) return;
        br::render::MiscConstants constants{};
        constants[LENS_FLARE_RC_VERTICES] = recording.Index(bindings.vertices);
        constants[LENS_FLARE_RC_SOURCE_SLOTS] = config.sourceSlots;
        constants[LENS_FLARE_RC_GHOSTS] = config.ghostSlots;
        constants[LENS_FLARE_RC_WAVELENGTHS] = config.wavelengthSlots;
        constants[LENS_FLARE_RC_GRID] = config.grid;
        rhi::ColorAttachment color = br::render::ColorTarget(recording, bindings.target, rhi::LoadOp::Clear);
        color.clear.format = rhi::Format::R16G16B16A16_Float;
        for (float& channel : color.clear.rgba) channel = 0.0f;
        rhi::PassBeginInfo begin{};
        begin.colors = { &color, 1 };
        begin.width = frame.width;
        begin.height = frame.height;
        begin.debugName = "Lens flare ghosts";
        auto& commands = recording.Commands();
        commands.BeginPass(begin);
        commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
        recording.BindGraphicsPipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        commands.Draw(config.grid * config.grid * 6u, config.InstanceCount(), 0u, 0u);
        commands.EndPass();
    }

private:
    static org::GraphicsPipelineDesc PipelineDesc() {
        org::GraphicsPipelineDesc desc;
        desc.name = "LensFlare.Draw.PSO";
        desc.vertex = { L"shaders/PostProcessing/lensFlareDraw.hlsl", L"LensFlareVS", L"vs_6_6" };
        desc.pixel = { L"shaders/PostProcessing/lensFlareDraw.hlsl", L"LensFlarePS", L"ps_6_6" };
        rhi::RasterState raster{};
        raster.fill = rhi::FillMode::Solid;
        raster.cull = rhi::CullMode::None; // folded cells face either way
        raster.frontCCW = false;
        desc.raster = raster;
        rhi::BlendState blendState{};
        blendState.numAttachments = 1;
        auto& blend = blendState.attachments[0];
        blend.enable = true;
        blend.srcColor = rhi::BlendFactor::One;
        blend.dstColor = rhi::BlendFactor::One;
        blend.colorOp = rhi::BlendOp::Add;
        blend.srcAlpha = rhi::BlendFactor::Zero;
        blend.dstAlpha = rhi::BlendFactor::One;
        blend.alphaOp = rhi::BlendOp::Add;
        blend.writeMask = rhi::ColorWriteEnable::All;
        desc.blend = blendState;
        rhi::RenderTargets targets{};
        targets.count = 1;
        targets.formats[0] = rhi::Format::R16G16B16A16_Float;
        desc.renderTargets = targets;
        desc.sample = rhi::SampleDesc{ 1, 0 };
        desc.topology = rhi::PrimitiveTopology::TriangleList;
        return desc;
    }

    std::shared_ptr<br::lens::GhostFlareManager> m_manager;
};

struct LensFlareCompositeBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken flare, target;
};

struct LensFlareCompositeFrame {
    bool active = false;
    uint32_t width = 0, height = 0;
};

// Adds the flare target into the upscaled HDR colour (lensFlareComposite.hlsl), before bloom and tonemapping.
class LensFlareCompositePass : public org::TypedRenderGraphPass<LensFlareCompositePass, LensFlareCompositeFrame, LensFlareCompositeBindings> {
public:
    explicit LensFlareCompositePass(std::shared_ptr<br::lens::GhostFlareManager> manager) : m_manager(std::move(manager)) {}

    LensFlareCompositeBindings Declare(org::PassBuilder& builder) {
        LensFlareCompositeBindings bindings{};
        bindings.pipeline = builder.Pipeline(PipelineDesc());
        bindings.flare = builder.ShaderResource(org::ResourceIdentifier{ kLensFlareTargetName }).View();
        bindings.target = builder.RenderTarget(Subresources(Builtin::PostProcessing::UpscaledHDR, org::Mip{ 0, 1 }));
        return bindings;
    }

    LensFlareCompositeFrame Prepare(const LensFlareCompositeBindings& bindings, const org::PassPrepareContext& preparation) const {
        LensFlareCompositeFrame frame{};
        if (const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr) {
            frame.active = m_manager->FrameConfig(context->frameNumber).active;
        }
        const auto& description = preparation.Describe(bindings.target);
        frame.width = description.texture.width;
        frame.height = description.texture.height;
        return frame;
    }

    static void Record(const LensFlareCompositeBindings& bindings, const LensFlareCompositeFrame& frame, org::PassRecordContext& recording) {
        if (!frame.active) return;
        br::render::MiscConstants constants{};
        constants[LENS_FLARE_RC_FLARE_SRV] = recording.Index(bindings.flare);
        br::render::DrawFullscreen(recording, bindings.pipeline, constants, br::render::ColorTarget(recording, bindings.target), frame.width, frame.height,
            "Lens flare composite");
    }

private:
    static org::GraphicsPipelineDesc PipelineDesc() {
        org::GraphicsPipelineDesc desc;
        desc.name = "LensFlare.Composite.PSO";
        desc.vertex = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        desc.pixel = { L"shaders/PostProcessing/lensFlareComposite.hlsl", L"LensFlareCompositePS", L"ps_6_6" };
        rhi::RasterState raster{};
        raster.fill = rhi::FillMode::Solid;
        raster.cull = rhi::CullMode::None;
        raster.frontCCW = false;
        desc.raster = raster;
        rhi::BlendState blendState{};
        blendState.numAttachments = 1;
        auto& blend = blendState.attachments[0];
        blend.enable = true;
        blend.srcColor = rhi::BlendFactor::One;
        blend.dstColor = rhi::BlendFactor::One;
        blend.colorOp = rhi::BlendOp::Add;
        blend.srcAlpha = rhi::BlendFactor::Zero;
        blend.dstAlpha = rhi::BlendFactor::One;
        blend.alphaOp = rhi::BlendOp::Add;
        blend.writeMask = rhi::ColorWriteEnable::All;
        desc.blend = blendState;
        rhi::RenderTargets targets{};
        targets.count = 1;
        targets.formats[0] = rhi::Format::R16G16B16A16_Float;
        desc.renderTargets = targets;
        desc.sample = rhi::SampleDesc{ 1, 0 };
        desc.topology = rhi::PrimitiveTopology::TriangleStrip;
        return desc;
    }

    std::shared_ptr<br::lens::GhostFlareManager> m_manager;
};

// Builds the four passes after the upscaler: the flare target is `divisor` times smaller than the output (the ghosts are
// soft), and `system` (path traced, with a BVH) makes the source visibility a ray query instead of a depth test.
inline void BuildLensFlarePasses(org::RenderGraph* graph, const std::shared_ptr<br::lens::GhostFlareManager>& manager,
    std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs, uint32_t outputWidth, uint32_t outputHeight,
    uint32_t divisor) {
    const uint32_t width = std::max(1u, outputWidth / std::max(divisor, 1u)), height = std::max(1u, outputHeight / std::max(divisor, 1u));
    org::TextureDescription description;
    description.channels = 4;
    description.format = rhi::Format::R16G16B16A16_Float;
    description.hasRTV = description.hasSRV = true;
    description.allowAlias = true;
    description.imageDimensions.emplace_back(width, height, 0, 0);
    auto target = org::PixelBuffer::CreateSharedUnmaterialized(description);
    target->SetName("Lens flare target");
    graph->RegisterResource(org::ResourceIdentifier{ kLensFlareTargetName }, target);
    graph->BuildPass<LensFlareSourcesPass>("LensFlareSourcesPass", manager, std::move(system), std::move(slabs), width, height);
    graph->SetPassTechnique("LensFlareSourcesPass", "Post Process::Upscaling");
    graph->BuildPass<LensFlareTracePass>("LensFlareTracePass", manager);
    graph->SetPassTechnique("LensFlareTracePass", "Post Process::Upscaling");
    graph->BuildPass<LensFlareDrawPass>("LensFlareDrawPass", manager);
    graph->SetPassTechnique("LensFlareDrawPass", "Post Process::Upscaling");
    graph->BuildPass<LensFlareCompositePass>("LensFlareCompositePass", manager);
    graph->SetPassTechnique("LensFlareCompositePass", "Post Process::Upscaling");
}
