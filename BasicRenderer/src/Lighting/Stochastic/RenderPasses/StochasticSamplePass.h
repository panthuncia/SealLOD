#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "VirtualGeometry/RayTracing/CLodRayTracingPasses.h"
#include "Render/Runtime/UploadTypes.h"
#include "BasicRenderer/Streaming/LightStateArtifacts.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Lighting/Stochastic/StochasticLightingResources.h"
#include "../shaders/PerPassRootConstants/stochasticLightingRootConstants.h"

namespace br::render {

struct StochasticSampleFrame {
    std::shared_ptr<CLodRayTracingSystem> system;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
    uint32_t flags = 0u;
    uint32_t sampleCount = 1u;
    uint32_t parity = 0u;
    uint32_t downsample = 1u;
};

// What a running average depends on, and how many frames it holds (docs/stochastic_lighting.md "Reference"). The split
// estimator's three passes share one, so they agree on each frame's flags.
struct StochasticSampleState {
    std::mutex mutex;
    std::array<uint64_t, 6> scene{};
    uint64_t accumulatedFrames = 0u;
    uint64_t restarts = 0u;
    uint32_t restartCauses = 0u; // Bit i: scene word i changed, since the last log line.
    uint64_t framesSinceLog = 0u;
    // The frame number this pass last ran at, and last ran with guiding: last frame's surface history and visible lists are
    // this pass's only if it ran (with guiding) the frame before.
    uint64_t lastFrame = UINT64_MAX;
    uint64_t lastGuidedFrame = UINT64_MAX;
    // The frame the passes sharing this state last prepared, and what they prepared for it.
    uint64_t preparedFrame = UINT64_MAX;
    StochasticSampleFrame prepared{};
};

enum class StochasticLightingPassMode : uint8_t {
    Estimator, // A few lights per pixel by importance, one ray each, in three kernels (StochasticSampleKernel).
    Reference, // shaders/Stochastic/stochasticReference.hlsl: every light in the cluster, averaged over frames.
};

// Which kernel a pass runs (docs/stochastic_lighting.md "Kernel split"): the reference's one, or one of the estimator's three.
enum class StochasticSampleKernel : uint8_t {
    Single, // The reference (shaders/Stochastic/stochasticReference.hlsl): every light, its rays and the average in one kernel.
    Select, // shaders/Stochastic/stochasticSelect.hlsl: the samples' slots and the compacted ray list.
    Trace, // shaders/Stochastic/stochasticTrace.hlsl: the indirect arguments, then one thread per ray.
    Shade, // shaders/Stochastic/stochasticShade.hlsl: the visible samples' sum, the running average, the visible lists.
};

struct StochasticSampleBindings {
    org::PipelineState pipeline, argumentsPipeline;
    org::CommandSignatureState signature;
    org::DeclaredViewToken diffuse, specular, accumulationDiffuse, accumulationSpecular;
    // The estimator's guiding: visible lists and surface history, two of each (this frame's and last frame's by parity).
    std::array<org::DeclaredViewToken, 2> visibleLists{}, surfaceHistory{};
    org::DeclaredViewToken slots, pixelWords, rays, rayCounter, rayArguments;
    uint32_t rayCapacity = 0u;
    StochasticSampleKernel kernel = StochasticSampleKernel::Single;
    bool estimator = false;
};

// Spot, point and directional lights at each pixel, shadowed by rays toward their emitters: the estimator's few samples, or
// the reference's every light averaged over frames while the camera, the lights and the BVH stay the same. Writes
// StochasticLighting::Diffuse and ::Specular, which the deferred pass adds, or the denoiser's noisy inputs when it runs. The
// estimator averages over frames too when asked (accumulate), to be compared with the reference. The estimator runs as three
// passes sharing a StochasticSampleState (docs/stochastic_lighting.md "Kernel split").
class StochasticSamplePass
    : public org::TypedRenderGraphPass<StochasticSamplePass, StochasticSampleFrame, StochasticSampleBindings> {
public:
    StochasticSamplePass(std::shared_ptr<CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs,
        std::shared_ptr<StochasticLightingResources> resources, StochasticLightingPassMode mode, bool accumulate,
        StochasticSampleKernel kernel = StochasticSampleKernel::Single, std::shared_ptr<StochasticSampleState> state = nullptr)
        : m_system(std::move(system)), m_slabs(std::move(slabs)), m_resources(std::move(resources))
        , m_state(state ? std::move(state) : std::make_shared<StochasticSampleState>()), m_mode(mode)
        , m_accumulate(accumulate || mode == StochasticLightingPassMode::Reference), m_kernel(kernel) {
        if (m_accumulate && (m_kernel == StochasticSampleKernel::Single || m_kernel == StochasticSampleKernel::Shade)) {
            m_accumulationDiffuse = StochasticLightingResources::MakeTexture(m_resources->width, m_resources->height,
                rhi::Format::R32G32B32A32_Float, 4, "Stochastic diffuse accumulation");
            m_accumulationSpecular = StochasticLightingResources::MakeTexture(m_resources->width, m_resources->height,
                rhi::Format::R32G32B32A32_Float, 4, "Stochastic specular accumulation");
        }
    }

    StochasticSampleBindings Declare(org::PassBuilder& builder) {
        StochasticSampleBindings bindings{};
        bindings.estimator = m_mode == StochasticLightingPassMode::Estimator;
        bindings.kernel = bindings.estimator ? m_kernel : StochasticSampleKernel::Single;
        if (bindings.estimator && bindings.kernel == StochasticSampleKernel::Single) {
            throw std::logic_error("the stochastic estimator runs as its three kernels");
        }
        const auto kernel = bindings.kernel;
        switch (kernel) {
        case StochasticSampleKernel::Single:
            bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticReference.hlsl", L"StochasticReferenceCS", "Stochastic.Reference.PSO"));
            break;
        case StochasticSampleKernel::Select:
            bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticSelect.hlsl", L"StochasticSelectCS", "Stochastic.Select.PSO"));
            break;
        case StochasticSampleKernel::Trace:
            bindings.argumentsPipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticTrace.hlsl", L"StochasticTraceArgsCS", "Stochastic.TraceArgs.PSO"));
            bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticTrace.hlsl", L"StochasticTraceCS", "Stochastic.Trace.PSO"));
            bindings.signature = builder.CommandSignature(br::render::RawDispatchCommandSignature());
            break;
        case StochasticSampleKernel::Shade:
            bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticShade.hlsl", L"StochasticShadeCS", "Stochastic.Shade.PSO"));
            break;
        }
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::InfoBuffer);
        const bool traces = kernel == StochasticSampleKernel::Single || kernel == StochasticSampleKernel::Trace;
        const bool selects = kernel == StochasticSampleKernel::Single || kernel == StochasticSampleKernel::Select;
        const bool shades = kernel == StochasticSampleKernel::Single || kernel == StochasticSampleKernel::Shade;
        if (traces) {
            CLodRayTracingSystem::DeclareTracing(builder);
            if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
            // What the alpha test of a ray's candidate hits reads (as the ray traced sun shadow declares).
            builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases,
                Builtin::PerMaterialDataBuffer, Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer,
                Builtin::PerObjectBuffer, Builtin::CullingCameraBuffer, Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups,
                Builtin::CLod::AssemblyTransforms, Builtin::CLod::AssemblyBoneRemaps, Builtin::CLod::AssemblyBoneRemapIndices,
                Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::Material::TextureStreamingMetadataBuffer);
        }
        if (selects) {
            // The primary surface and its shading (as the deferred pass declares them).
            builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::PerMaterialDataBuffer);
            builder.ShaderResource(Builtin::Light::ActiveLightIndices,
                Builtin::Light::GridCells, Builtin::Light::GridIndices, Builtin::PerMaterialOpenPBRDataBuffer,
                Builtin::Surface::BaseColorOpacity, Builtin::Surface::NormalRoughness, Builtin::Surface::SpecularAo, Builtin::Surface::Emissive,
                Builtin::Surface::Identity, Builtin::Surface::Payload0, Builtin::Surface::Payload1, Builtin::Surface::Records,
                Builtin::OpenPBR::FuzzLTC, Builtin::OpenPBR::IdealMetalEnergyComplement, Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
                Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement);
            builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
                org::SrvView{ static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull) });
            builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
            if (bindings.estimator) builder.ShaderResource(Builtin::Surface::Motion);
        }
        // Denoised, the estimator's raw output goes to the denoiser; otherwise straight to the deferred pass. Selection and the
        // trace write it only for debug probes.
        const bool denoised = bindings.estimator && !m_accumulate && m_resources->Denoised();
        if (denoised) {
            bindings.diffuse = builder.UnorderedAccess(m_resources->noisyDiffuse).View();
            bindings.specular = builder.UnorderedAccess(m_resources->noisySpecular).View();
        }
        else {
            bindings.diffuse = builder.UnorderedAccess(Builtin::StochasticLighting::Diffuse).View();
            bindings.specular = builder.UnorderedAccess(Builtin::StochasticLighting::Specular).View();
        }
        if (m_accumulate && shades) {
            bindings.accumulationDiffuse = builder.UnorderedAccess(m_accumulationDiffuse).View();
            bindings.accumulationSpecular = builder.UnorderedAccess(m_accumulationSpecular).View();
        }
        if (bindings.estimator) {
            for (uint32_t index = 0; index < 2u; ++index) {
                if (selects || shades) bindings.visibleLists[index] = builder.UnorderedAccess(m_resources->visibleLists[index]).View();
                if (selects || traces) bindings.surfaceHistory[index] = builder.UnorderedAccess(m_resources->surfaceHistory[index]).View();
            }
        }
        if (kernel != StochasticSampleKernel::Single) {
            bindings.rayCapacity = m_resources->rayCapacity;
            bindings.pixelWords = builder.UnorderedAccess(m_resources->pixelWords).View();
            if (selects || shades) bindings.slots = builder.UnorderedAccess(m_resources->slots).View();
            if (selects || traces) {
                bindings.rays = builder.UnorderedAccess(m_resources->rays).View();
                bindings.rayCounter = builder.UnorderedAccess(m_resources->rayCounter).View();
            }
            if (traces) {
                bindings.rayArguments = builder.UnorderedAccess(m_resources->rayArgs).View();
                builder.InternalTransition(m_resources->rayArgs, org::ResourceState{ rhi::ResourceAccessType::IndirectArgument,
                    rhi::ResourceLayout::GenericRead, rhi::ResourceSyncState::ExecuteIndirect });
            }
        }
        builder.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    void Update(const org::UpdateExecutionContext&) override {
        if (m_kernel == StochasticSampleKernel::Select && m_resources->rayCounter) {
            // Selection appends from zero.
            const uint32_t zero[2] = {};
            UploadBufferData(zero, sizeof(zero), org::runtime::UploadTarget::FromShared(m_resources->rayCounter), 0);
        }
    }

    StochasticSampleFrame Prepare(const StochasticSampleBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        if (!update) {
            return StochasticSampleFrame{ m_system };
        }
        std::scoped_lock lock(m_state->mutex);
        // The split passes of one frame share one preparation, so they agree on the flags (and the average restarts once).
        if (m_state->preparedFrame != update->frameNumber) {
            m_state->prepared = PrepareLocked(*update);
            m_state->preparedFrame = update->frameNumber;
        }
        return m_state->prepared;
    }

    static void Record(const StochasticSampleBindings& bindings, const StochasticSampleFrame& frame, org::PassRecordContext& recording) {
        if (frame.width == 0u || frame.height == 0u) {
            return;
        }
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        br::render::MiscConstants constants{};
        constants[CLOD_RT_SEC_TLAS_SRV] = frame.system->GetTlasSrv();
        constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.system->GetClasInfoSrv();
        constants[CLOD_RT_SEC_INSTANCE_ROWS_SRV] = frame.system->GetInstanceRowsSrv();
        constants[CLOD_RT_SEC_FRAME] = frame.frame;
        constants[STOCHASTIC_DIFFUSE_UAV] = recording.Index(bindings.diffuse);
        constants[STOCHASTIC_SPECULAR_UAV] = recording.Index(bindings.specular);
        const auto kernel = bindings.kernel;
        if ((frame.flags & STOCHASTIC_FLAG_ACCUMULATE) && (kernel == StochasticSampleKernel::Single || kernel == StochasticSampleKernel::Shade)) {
            constants[STOCHASTIC_ACCUMULATION_DIFFUSE_UAV] = recording.Index(bindings.accumulationDiffuse);
            constants[STOCHASTIC_ACCUMULATION_SPECULAR_UAV] = recording.Index(bindings.accumulationSpecular);
        }
        constants[STOCHASTIC_FLAGS] = frame.flags;
        constants[STOCHASTIC_SAMPLE_COUNT] = frame.sampleCount;
        constants[STOCHASTIC_DOWNSAMPLE_FACTOR] = frame.downsample;
        if (bindings.estimator) {
            const auto index = [&](const org::DeclaredViewToken& token) { return token.layout ? recording.Index(token) : 0u; };
            constants[STOCHASTIC_VISIBLE_LISTS_UAV] = index(bindings.visibleLists[frame.parity]);
            constants[STOCHASTIC_PREVIOUS_VISIBLE_LISTS_UAV] = index(bindings.visibleLists[frame.parity ^ 1u]);
            constants[STOCHASTIC_SURFACE_HISTORY_UAV] = index(bindings.surfaceHistory[frame.parity]);
            constants[STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV] = index(bindings.surfaceHistory[frame.parity ^ 1u]);
            if (kernel != StochasticSampleKernel::Single) {
                constants[STOCHASTIC_SLOTS_UAV] = index(bindings.slots);
                constants[STOCHASTIC_PIXEL_WORDS_UAV] = index(bindings.pixelWords);
                constants[STOCHASTIC_RAYS_UAV] = index(bindings.rays);
                constants[STOCHASTIC_RAY_COUNTER_UAV] = index(bindings.rayCounter);
                constants[STOCHASTIC_RAY_ARGS_UAV] = index(bindings.rayArguments);
                constants[STOCHASTIC_RAY_CAPACITY] = bindings.rayCapacity;
            }
        }
        if (kernel == StochasticSampleKernel::Trace) {
            // The arguments from the rays selection appended, then one thread per ray.
            br::render::DispatchCompute(recording, bindings.argumentsPipeline, constants, 1u);
            recording.BufferBarriers({ bindings.rayCounter.Resource() },
                rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
            recording.BufferBarriers({ bindings.rayArguments.Resource() },
                rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
                rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
            br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.signature, bindings.rayArguments.Resource());
            return;
        }
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        // One thread per sampled pixel: per pixel, or downsampled per 2x2 block.
        const uint32_t sampledWidth = (frame.width + frame.downsample - 1u) / frame.downsample;
        const uint32_t sampledHeight = (frame.height + frame.downsample - 1u) / frame.downsample;
        recording.Commands().Dispatch((sampledWidth + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE,
            (sampledHeight + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE, 1u);
    }

private:
    // One frame's constants and flags, with the frame-to-frame bookkeeping (guiding history, the running average's restarts).
    // Called once per frame, under the state's mutex.
    StochasticSampleFrame PrepareLocked(const UpdateContext& update) const {
        StochasticSampleFrame frame{ m_system };
        frame.width = update.renderResolution.x;
        frame.height = update.renderResolution.y;
        // The frame number, not frameIndex: that is the frame slot, which repeats every few frames.
        frame.frame = static_cast<uint32_t>(update.frameNumber);
        frame.parity = static_cast<uint32_t>(update.frameNumber & 1u);
        frame.sampleCount = std::clamp(SettingsManager::GetInstance().getSettingGetter<uint32_t>("stochasticLightingSamplesPerPixel")(),
            1u, STOCHASTIC_MAX_SAMPLE_COUNT);
        if (m_mode == StochasticLightingPassMode::Estimator) {
            // Reduced resolution (docs/stochastic_lighting.md "Reduced resolution"): a quarter of the pixels, each with twice the
            // samples (4 at the default 2), so the samples per pixel are half the setting's.
            frame.downsample = m_resources->downsample;
            if (frame.downsample > 1u) frame.sampleCount = std::min(frame.sampleCount * 2u, STOCHASTIC_MAX_SAMPLE_COUNT);
            // The kernels' buffers hold slotCapacity samples per sampled pixel (at full resolution the setting's at the graph
            // build; a change of it rebuilds the graph).
            frame.sampleCount = std::min(frame.sampleCount, m_resources->slotCapacity);
            if (!m_accumulate && m_resources->Denoised()) frame.flags |= STOCHASTIC_FLAG_DENOISED;
            if (SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingGuiding")()) {
                frame.flags |= STOCHASTIC_FLAG_GUIDING;
                if (m_state->lastGuidedFrame + 1u == update.frameNumber) frame.flags |= STOCHASTIC_FLAG_HISTORY_VALID;
                m_state->lastGuidedFrame = update.frameNumber;
            }
            m_state->lastFrame = update.frameNumber;
        }
        if (!m_accumulate) {
            return frame;
        }
        frame.flags |= STOCHASTIC_FLAG_ACCUMULATE;
        // Any change restarts the average: the camera (unjittered, since the upscaler's jitter moves every frame and spreads
        // the samples over the pixel anyway), any light's row, and the BVH. The light tables' revision and the resident texture
        // bytes are no use here: both change every frame with nothing visible changing (lights are re-uploaded each frame, and
        // streaming churns). So a texture that streams in sharper does not restart it.
        const auto& stats = m_system->GetStats();
        const std::array<uint64_t, 6> scene = {
            update.hasPrimaryCamera ? HashCamera(update.primaryCamera.info.view, update.primaryCamera.info.unjitteredProjection) : 0u,
            update.lightTables ? HashLightTables(*update.lightTables) : 0u,
            stats.instances, stats.clustersInBvh, stats.clasBuildsTotal ^ (stats.clasFreedTotal << 32u), 0u };
        if (scene != m_state->scene) {
            for (size_t word = 0; word < scene.size(); ++word) {
                if (scene[word] != m_state->scene[word]) m_state->restartCauses |= 1u << word;
            }
            m_state->scene = scene;
            m_state->accumulatedFrames = 0u;
            ++m_state->restarts;
            frame.flags |= STOCHASTIC_FLAG_RESTART_ACCUMULATION;
        }
        ++m_state->accumulatedFrames;
        if (++m_state->framesSinceLog >= 300u) {
            // Causes: 1 camera, 2 lights, 4 BVH instances, 8 BVH clusters, 16 CLAS builds.
            spdlog::info("Stochastic lighting accumulation: {} frames held, {} restarts (causes 0x{:x})",
                m_state->accumulatedFrames, m_state->restarts, m_state->restartCauses);
            m_state->framesSinceLog = 0u;
            m_state->restartCauses = 0u;
        }
        return frame;
    }

    static uint64_t HashCamera(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection) {
        uint64_t hash = 14695981039346656037ull; // FNV-1a over the matrices' bits.
        for (const DirectX::XMMATRIX* matrix : { &view, &projection }) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(matrix);
            for (size_t index = 0; index < sizeof(DirectX::XMMATRIX); ++index) {
                hash = (hash ^ bytes[index]) * 1099511628211ull;
            }
        }
        return hash;
    }

    static uint64_t HashLightTables(const PublishedLightTableState& tables) {
        uint64_t hash = 14695981039346656037ull; // FNV-1a over the published rows and active list.
        for (const auto& image : tables.tableImages) {
            if (!image) continue;
            for (const std::byte value : *image) {
                hash = (hash ^ static_cast<uint8_t>(value)) * 1099511628211ull;
            }
        }
        return hash;
    }

    std::shared_ptr<CLodRayTracingSystem> m_system;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    std::shared_ptr<StochasticLightingResources> m_resources;
    std::shared_ptr<StochasticSampleState> m_state;
    StochasticLightingPassMode m_mode = StochasticLightingPassMode::Reference;
    bool m_accumulate = true;
    StochasticSampleKernel m_kernel = StochasticSampleKernel::Single;
    std::shared_ptr<org::PixelBuffer> m_accumulationDiffuse;
    std::shared_ptr<org::PixelBuffer> m_accumulationSpecular;
};

} // namespace br::render
