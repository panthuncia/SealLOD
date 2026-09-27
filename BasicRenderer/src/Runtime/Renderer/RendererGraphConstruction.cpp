//
// Created by matth on 6/25/2024.
//

#include <BasicRenderer/Renderer.h>
#include "Runtime/Publication/PersistentRendererPublication.h"

#define _USE_MATH_DEFINES
#include <optional>
#include <math.h>
#include <atlbase.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <array>
#include <stacktrace>
#include <thread>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <typeindex>
#include <utility>

#include <rhi_debug.h>
#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>
#include "Utilities/Utilities.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Resources/ResourceManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Runtime/Frame/RendererFrameInputs.h"
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include "Materials/TextureStreaming/TextureImageTableArtifacts.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include <BasicRenderer/Streaming/PoseState.h>
#include "BasicRenderer/Streaming/LightStateArtifacts.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Lighting/Shading/RenderPasses/ForwardRenderPass.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "Lighting/Environment/RenderPasses/SkyboxRenderPass.h"
#include "Lighting/Environment/RenderPasses/EnvironmentFilterPass.h"
#include "Diagnostics/RenderPasses/DebugSpheresPass.h"
#include "Diagnostics/RenderPasses/DebugSkeletonPass.h"
#include "RenderPasses/Base/ComputePass.h"
#include "RenderPasses/Base/CopyPass.h"
#include "PostProcessing/Downsampling/RenderPasses/Downsample.h"
#include "PostProcessing/ToneMapping/RenderPasses/Tonemapping.h"
#include "PostProcessing/Upscaling/RenderPasses/UpscalingPass.h"
#include "PostProcessing/Upscaling/RenderPasses/DilateMotionVectorsPass.h"
#include "PostProcessing/Exposure/RenderPasses/luminanceHistogram.h"
#include "PostProcessing/Exposure/RenderPasses/luminanceHistogramAverage.h"
#include "VirtualGeometry/Rasterization/RenderPasses/ClearVisibilityBufferPass.h"
#include "Diagnostics/RenderPasses/DebugResolvePass.h"
#include "Diagnostics/Menu/RenderPasses/MenuRenderPass.h"
#include "Runtime/Frame/RenderPasses/PresentPass.h"
#include "Scene/Views/RenderPasses/PrimaryCameraUploadPass.h"
#include "Resources/TextureDescription.h"
#include "Diagnostics/Menu/Menu.h"
#include "Runtime/Device/DeletionManager.h"
#include "Runtime/Device/DescriptorHeapManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Scene/Objects/ObjectManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Lighting/Lights/LightManager.h"
#include "Lighting/Environment/EnvironmentManager.h"
#include "Animation/Skeletons/SkeletonManager.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Scene/Views/ViewManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Materials/MaterialManager.h"
#include "Materials/TextureStreaming/TextureStreamingManager.h"
#include "Terrain/Residency/TerrainManager.h"
#include "Assets/Textures/TextureFactory.h"
#include "Utilities/MathUtils.h"
#include <BasicScene/MovementState.h>
#include "ThirdParty/XeGTAO.h"
#include "Lighting/Environment/EnvironmentManager.h"
#if BASICRENDERER_HAS_INTEROP_VALIDATION
#include "Validation/SARPInteropValidation.h"
#endif
#include "PostProcessing/ToneMapping/TonemapTypes.h"
#include "../../../generated/BuiltinResources.h"
#include "Resources/ResourceIdentifier.h"
#include "Runtime/GraphIntegration/RenderGraphBuildHelper.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "Runtime/IO/DirectStorageManager.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "Runtime/GraphIntegration/IOExtension.h"
#include "VirtualGeometry/GraphIntegration/CLodExtension.h"
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Diagnostics/RenderPasses/DebugGridPass.h"
#include "Runtime/GraphIntegration/ReadbackCaptureExtension.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/ScopedActiveGraphServices.h"
#include "Resources/Resource.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "Resources/ReadbackRequest.h"
#include "Resources/DynamicResource.h"
#include "Resources/ExternalTextureResource.h"
#include "Render/MemoryIntrospectionBackend.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"
#include "Runtime/Scheduling/TbbTaskService.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include "BasicRenderer/Pipeline/MeshDrawWorkload.h"
#include "Scene/Objects/IndirectStateArtifacts.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include "VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.h"
#include "Terrain/Residency/TerrainStateArtifacts.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include "VirtualGeometry/Streaming/Publication/GeometryResidencyStateArtifacts.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"
#include "Scene/Objects/ObjectBufferStateArtifacts.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasTelemetry.h"
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"

namespace {
}

void Renderer::CreateRenderGraph() {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph");
    if (!IsSceneReadyForFrame()) {
        rebuildRenderGraph = true;
        return;
    }

    auto primaryCameraEntity = GetValidatedPrimaryRenderCamera(true);
    if (!primaryCameraEntity) {
        spdlog::warn("Renderer: primary camera bridge is not ready during render graph creation. Deferring rebuild.");
        rebuildRenderGraph = true;
        return;
    }

    {
        BT_ZONE_SCOPE("Renderer::CreateRenderGraph::StallPipeline");
        StallPipeline();
    }

    // Render-graph queue timelines are recreated below. Descriptor retirement
    // snapshots contain non-owning timeline handles, so consume all pending
    // releases and clear the snapshot while those timelines are still alive.
    org::DescriptorHeapManager::GetInstance().DrainDeferredReleasesAfterDeviceIdle();
	PSOManager::GetInstance().DrainRetiredLivePipelinesAfterDeviceIdle();

    // TODO: Find a better way to handle resources like this
    // TODO: this access pattern is stupid
    auto primaryViewID = primaryCameraEntity.get<Components::RenderViewRef>().viewID;
    auto primaryCamera = m_pViewManager->Get(primaryViewID);

    // TODO: Primary camera and current environment will change, and I'd rather not recompile the graph every time that happens.
    // How should we manage swapping out their resources? DynamicResource could work, but the ResourceGroup/independantly managed resource
    // part of the compiler would need to become aware of DynamicResource.

    // TODO: Some of these resources don't really need to be recreated (GTAO, etc.)
    // Instead, just create them externally and register them

        if (!currentRenderGraph)
        {
		currentRenderGraph = std::make_unique<org::RenderGraph>(DeviceManager::GetInstance().GetDevice(), DeviceManager::GetInstance().GetBackend());
		if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
			currentRenderGraph->RegisterBackendDevice(DeviceManager::GetInstance().GetPeerBackend(), DeviceManager::GetInstance().GetPeerDevice());
		}
        if (auto* uploadService = currentRenderGraph->GetUploadService()) {
            uploadService->Initialize();
        }
        if (m_uploadPolicyService) {
            m_uploadPolicyService->SetUploadService(currentRenderGraph->RetainUploadService());
            org::runtime::SetActiveUploadPolicyService(m_uploadPolicyService.get());
        }
        if (auto* descriptorService = currentRenderGraph->GetDescriptorService()) {
            descriptorService->Initialize();
        }
        }
        if (m_clodRayTracingSystem && currentRenderGraph)
            m_clodRayTracingSystem->SetUploadService(currentRenderGraph->RetainUploadService());
        if (m_pSkeletonManager && currentRenderGraph)
            m_pSkeletonManager->SetUploadService(currentRenderGraph->RetainUploadService());
        if (currentRenderGraph)
            ::ResourceManager::GetInstance().SetUploadService(currentRenderGraph->RetainUploadService());

        if (!m_renderGraphRuntimeInitialized) {
        currentRenderGraph->GetMemorySnapshotProvider().SetProvider(
            org::memory::CreateECSMemorySnapshotProvider(RendererECSManager::GetInstance().GetWorld()));
        Menu::GetInstance().SetRenderGraph(currentRenderGraph.get());

        if (auto* textureFactory = m_pTextureFactory.get()) {
            textureFactory->SetUploadService(currentRenderGraph->RetainUploadService());
            textureFactory->SetReadbackService(currentRenderGraph->GetReadbackServiceOwner());
        }
        if (m_pEnvironmentManager)
            m_pEnvironmentManager->SetUploadService(currentRenderGraph->RetainUploadService());
        if (m_pMaterialManager) {
			m_pMaterialManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService());
			m_pMaterialManager->SetDescriptorService(currentRenderGraph->RetainDescriptorService());
		}
		if (m_pObjectManager)
			m_pObjectManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				m_numFramesInFlight);
		if (m_pMeshManager)
			m_pMeshManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				m_numFramesInFlight);
		if (m_pIndirectCommandBufferManager)
			m_pIndirectCommandBufferManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService());
		if (m_sceneIngestionServices.execution.generation) {
			m_sceneIngestionServices.execution.generation->uploads.store(
				currentRenderGraph->RetainUploadService(), std::memory_order_release);
			m_sceneIngestionServices.execution.generation->descriptors.store(
				currentRenderGraph->RetainDescriptorService(), std::memory_order_release);
		}
		if (m_pTerrainManager)
			m_pTerrainManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				currentRenderGraph->RetainDescriptorService());


        RendererECSManager::GetInstance().CreateRenderPhaseEntity(Engine::Primary::CLodTransparentPass);
		m_renderGraphRuntimeInitialized = true;
    }

    if (m_pipelineExtensionsDirty) {
        currentRenderGraph->ClearExtensions();
        RegisterPipelineExtensions();
        m_pipelineExtensionsDirty = false;
    }

    auto& newGraph = currentRenderGraph;
	org::runtime::ScopedActiveGraphServices activeGraphServices(
		newGraph->GetUploadService(), newGraph->GetDescriptorService());
    const auto probeGraphBuildPhase = [&](const char* phase) {
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    };

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::ResetForRebuild");
    newGraph->ResetForRebuild();
    // CreateRenderGraph stalls every queue before teardown. ResetForRebuild
    // can retire another wave of resources/descriptors after the pre-reset
    // descriptor drain above. Consume that wave before draining API objects:
    // destroying those resources can enqueue their native resources in
    // DeletionManager. If it is left until normal frame maintenance, those
    // native objects survive for numFramesInFlight + 1 frames and can retain
    // stale graph backing through the rebuilt graph's first frames.
    org::DescriptorHeapManager::GetInstance().DrainDeferredReleasesAfterDeviceIdle();
	PSOManager::GetInstance().DrainRetiredLivePipelinesAfterDeviceIdle();

    // Everything released above is GPU-idle, so it is safe (and important) to
    // release its native objects before materializing the candidate graph.
    // Otherwise two complete alias-pool generations overlap for the normal
    // frames-in-flight retirement delay and can exhaust VRAM.
    org::DeletionManager::GetInstance().DrainAll();
    }
    probeGraphBuildPhase("CreateRenderGraph after ResetForRebuild");

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::RegisterProvidersAndExtensions");
    newGraph->RegisterProvider(m_pMeshManager.get());
    newGraph->RegisterProvider(m_pObjectManager.get());
    newGraph->RegisterProvider(m_pViewManager.get());
    newGraph->RegisterProvider(m_pLightManager.get());
    newGraph->RegisterProvider(m_pEnvironmentManager.get());
	newGraph->RegisterProvider(m_pMaterialManager.get());
    newGraph->RegisterProvider(m_pTerrainManager.get());
	newGraph->RegisterProvider(m_pSkeletonManager.get());
    newGraph->RegisterProvider(&m_coreResourceProvider);
    newGraph->PrepareExtensionsForBuild();
    }
    probeGraphBuildPhase("CreateRenderGraph after PrepareExtensionsForBuild");

    // The primary camera is high-frequency frame data, not renderer-state
    // publication. Insert its graphics-queue upload before all pipeline passes;
    // their camera SRV declarations establish the required RAW dependencies.
    newGraph->BuildPass<br::render::PrimaryCameraUploadPass>(
        "PrimaryCameraUploadPass", m_pViewManager->GetCameraBuffer(),
        m_pViewManager->GetCullingCameraBuffer(), m_numFramesInFlight);

    auto& depth = primaryCameraEntity.get<Components::DepthMap>();
    std::shared_ptr<org::PixelBuffer> depthTexture = depth.depthMap;

    const bool terrainRvtEnabled = m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>();
    m_materialEvaluationInputs = {};
    m_materialEvaluationInputs.pipelines = &PSOManager::GetInstance();
    m_materialEvaluationInputs.commandSignatures = &CommandSignatureManager::GetInstance();
    m_materialEvaluationInputs.clodSlabResources = m_pMeshManager
        ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr;
    {
        auto& world = RendererECSManager::GetInstance().GetWorld();
        const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();
        const auto capture = [&](auto componentTag, auto&& accept) {
            world.query_builder<>()
                .with<CLodExtensionTypeTag>(visibilityTag)
                .with(componentTag)
                .build()
                .each([&](flecs::entity entity) {
                    if (const auto resource = entity.try_get<Components::Resource>(); resource) {
                        if (auto retained = std::static_pointer_cast<org::GloballyIndexedResource>(
                                resource->resource.lock())) {
                            accept(entity, std::move(retained));
                        }
                    }
                });
        };
        capture(world.component<VisibleClustersBufferTag>(), [&](flecs::entity entity, auto resource) {
            m_materialEvaluationInputs.visibleClusters = std::move(resource);
            if (const auto capacity = entity.try_get<CLodVisibleClusterCapacity>(); capacity)
                m_materialEvaluationInputs.visibleClusterCapacity = capacity->maxVisibleClusters;
        });
        capture(world.component<VisibleClustersCounterTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.visibleClusterCounter = std::move(resource);
        });
        capture(world.component<VisibleClusterTransformIndicesBufferTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.visibleClusterTransformIndices = std::move(resource);
        });
        capture(world.component<CLodReyesDiceQueueTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesDiceQueue = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableConfigsTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableConfigs = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableVerticesTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableVertices = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableTrianglesTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableTriangles = std::move(resource);
        });
    }
    br::pipeline::PipelineBuildContext buildContext(
        *newGraph,
        m_pipelineRecipe.Bindings(),
        [&](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions&) {
            using enum br::pipeline::TechniqueId;
            switch (id) {
            case FrameResources:
                newGraph->RegisterResource(Builtin::PrimaryCamera::DepthTexture, depthTexture);
                newGraph->RegisterResource(Builtin::PrimaryCamera::LinearDepthMap, depth.linearDepthMap);
                newGraph->RegisterResource(
                    Builtin::PrimaryCamera::ProjectedDepthTexture,
                    m_visibilityRendering ? depth.projectedDepthMap : depthTexture);
                newGraph->RegisterResource(Builtin::Backbuffer, m_dynamicBackbuffer);
                newGraph->RegisterResource(Builtin::PresentationColor, m_dynamicPresentationColor);
                newGraph->RegisterResource(Builtin::PerFrameBuffer, ::ResourceManager::GetInstance().GetPerFrameBuffer());
                break;
            case BrdfIntegration:
                BuildBRDFIntegrationPass(newGraph.get());
                break;
            case Environment: {
                if (m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap)) m_defaultEnvironmentCubemap.reset();
                if (m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap)) m_defaultEnvironmentPrefilteredCubemap.reset();
                if ((!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap) && !m_defaultEnvironmentCubemap) ||
                    (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap) && !m_defaultEnvironmentPrefilteredCubemap)) {
                    CreateDefaultEnvironmentResources();
                }
                BuildEnvironmentPipeline(newGraph.get());
                auto currentCubemap = m_defaultEnvironmentCubemap;
                auto currentPrefiltered = m_defaultEnvironmentPrefilteredCubemap;
                if (m_currentEnvironment && m_currentEnvironment->GetEnvironmentCubemap() &&
                    m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr() &&
                    m_currentEnvironment->GetEnvironmentPrefilteredCubemap()) {
                    currentCubemap = m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr();
                    currentPrefiltered = m_currentEnvironment->GetEnvironmentPrefilteredCubemap();
                    m_warnedUsingFallbackEnvironment = false;
                }
                else if (!m_warnedUsingFallbackEnvironment) {
                    spdlog::warn("Renderer: no valid environment is active. Using fallback blank cubemaps.");
                    m_warnedUsingFallbackEnvironment = true;
                }
                const auto registerEnvironment = [&](org::ResourceIdentifier resourceId, std::shared_ptr<org::Resource> fallback) {
                    if (const auto* binding = m_pipelineRecipe.Bindings().Find(resourceId)) {
                        const auto& contract = binding->contract;
                        if (contract.initialAccess != rhi::ResourceAccessType::None ||
                            contract.initialLayout != rhi::ResourceLayout::Undefined ||
                            contract.initialSync != rhi::ResourceSyncState::None) {
                            if (auto* tracker = binding->resource->GetStateTracker()) {
                                tracker->Reset(org::RangeSpec{}, org::ResourceState{ contract.initialAccess, contract.initialLayout, contract.initialSync });
                            }
                        }
                        newGraph->RegisterResource(resourceId, binding->resource);
                    }
                    else {
                        newGraph->RegisterResource(resourceId, std::move(fallback));
                    }
                };
                registerEnvironment(Builtin::Environment::CurrentCubemap, currentCubemap);
                registerEnvironment(Builtin::Environment::CurrentPrefilteredCubemap, currentPrefiltered);
                if (m_blueNoiseTexture) newGraph->RegisterResource(Builtin::Noise::BlueNoise2D, m_blueNoiseTexture);
                if (m_openPBRLookupResources.idealDielectricEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricEnergyComplement, m_openPBRLookupResources.idealDielectricEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricAverageEnergyComplement, m_openPBRLookupResources.idealDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricReflectionRatio) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricReflectionRatio, m_openPBRLookupResources.idealDielectricReflectionRatio);
                if (m_openPBRLookupResources.opaqueDielectricEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement, m_openPBRLookupResources.opaqueDielectricEnergyComplement);
                if (m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement, m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealMetalEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealMetalEnergyComplement, m_openPBRLookupResources.idealMetalEnergyComplement);
                if (m_openPBRLookupResources.idealMetalAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealMetalAverageEnergyComplement, m_openPBRLookupResources.idealMetalAverageEnergyComplement);
                if (m_openPBRLookupResources.fuzzLTC) newGraph->RegisterResource(Builtin::OpenPBR::FuzzLTC, m_openPBRLookupResources.fuzzLTC);
                break;
            }
            case ClusterLod:
            case ClusterLodAlpha:
            case ClusterLodShadow:
            case ClusterLodVoxel:
                break; // These techniques own ordered graph extensions.
            case CanonicalSurfaceResources: {
                const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
                org::TextureDescription desc;
                desc.channels = 2;
                desc.format = rhi::Format::R32G32_UInt;
                desc.hasRTV = desc.hasSRV = desc.hasUAV = desc.hasNonShaderVisibleUAV = true;
                desc.allowAlias = true;
                desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                auto visibilityBuffer = org::PixelBuffer::CreateSharedUnmaterialized(desc);
                visibilityBuffer->SetName("Visibility Buffer");
                org::memory::SetResourceUsageHint(*visibilityBuffer, "Canonical surface visibility");
                newGraph->RegisterResource(Builtin::PrimaryCamera::VisibilityTexture, visibilityBuffer);
                m_pViewManager->AttachVisibilityBuffer(primaryViewID, visibilityBuffer);
                CreateCanonicalSurfaceResources(newGraph.get());
                CreateDebugVisualizationResources(newGraph.get());
                if (m_visibilityRendering) {
                    newGraph->BuildPass<ClearVisibilityBufferPass>("ClearVisibilityBufferPass");
                    newGraph->SetPassTechnique("ClearVisibilityBufferPass", "Primary Visibility::Canonical Surface Construction");
                }
                break;
            }
            case VisibilityMaterialBinning:
                RegisterVisUtilResources(newGraph.get(), false);
                BuildVisibilityMaterialBinningPipeline(newGraph.get(), m_materialEvaluationInputs);
                break;
            case TerrainRvt:
                RegisterVisUtilResources(
                    newGraph.get(), true, false,
                    &m_producerPersistentState->terrainRvtResources);
                BuildTerrainRvtPipeline(newGraph.get());
                break;
            case TerrainRegionMaterialEvaluation:
                BuildTerrainRegionMaterialEvaluationPipeline(newGraph.get(), m_materialEvaluationInputs);
                break;
            case MaterialEvaluation:
                BuildMaterialEvaluationPipeline(newGraph.get(), m_materialEvaluationInputs, terrainRvtEnabled);
                break;
            case Gtao:
                RegisterGTAOResources(newGraph.get());
                BuildGTAOPipeline(newGraph.get(), primaryCameraEntity.try_get<Components::Camera>());
                break;
            case ClusteredLighting:
                BuildLightClusteringPipeline(newGraph.get());
                break;
            case PrimaryLighting:
                BuildPrimaryPass(newGraph.get(), m_currentEnvironment.get(),
                    m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap));
                break;
            case Reflections: {
                const bool rayTraced = m_rayTracedReflections && DeviceManager::GetInstance().GetCLodRayTracingSupported();
                if (rayTraced) BuildRayTracedReflectionPasses(newGraph.get());
                else if (m_screenSpaceReflections) BuildSSRPasses(newGraph.get());
                break;
            }
            case Exposure: {
                auto adapted = CreateIndexedStructuredBuffer(1, sizeof(float), true, false);
                adapted->SetName("Adapted Luminance");
                org::memory::SetResourceUsageHint(*adapted, "Post-Processing resources");
                newGraph->RegisterResource(Builtin::PostProcessing::AdaptedLuminance, adapted);
                auto histogram = CreateIndexedStructuredBuffer(256, sizeof(uint32_t), true, false);
                histogram->SetName("Luminance Histogram Buffer");
                org::memory::SetResourceUsageHint(*histogram, "Post-Processing resources");
                newGraph->RegisterResource(Builtin::PostProcessing::LuminanceHistogram, histogram);
				auto& histogramBuilder = newGraph->BuildPass<LuminanceHistogramPass>("luminanceHistogramPass");
#if BASICRENDERER_HAS_INTEROP_VALIDATION
				br::validation::SARPInteropValidation::ApplyPassPolicy(
					"luminanceHistogramPass", histogramBuilder, DeviceManager::GetInstance().GetPeerBackend());
#endif
                newGraph->SetPassTechnique("luminanceHistogramPass", "Post Process::Exposure");
                newGraph->BuildPass<LuminanceHistogramAveragePass>("LuminanceAveragePass");
                newGraph->SetPassTechnique("LuminanceAveragePass", "Post Process::Exposure");
                break;
            }
            case Upscaling:
                if (UpscalingManager::GetInstance().GetCurrentUpscalingMode() == UpscalingMode::DLSS &&
                    SettingsManager::GetInstance().getSettingGetter<bool>("enableDilatedMotionVectors")()) {
                    newGraph->BuildPass<DilateMotionVectorsPass>("DilateMotionVectorsPass");
                    newGraph->SetPassTechnique("DilateMotionVectorsPass", "Post Process::Upscaling");
                }
                newGraph->BuildPass<UpscalingPass>("UpscalingPass",
                    br::render::UpscalingGenerationService::CaptureCurrent());
                newGraph->SetPassTechnique("UpscalingPass", "Post Process::Upscaling");
                break;
            case Bloom:
				BuildBloomPipeline(newGraph.get());
                break;
            case Tonemapping:
                newGraph->BuildPass<TonemappingPass>(
                    "TonemappingPass",
                    m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
                newGraph->SetPassTechnique("TonemappingPass", "Post Process::Tonemapping");
                break;
            case DebugOutput: {
                auto debugSnapshots = br::render::DebugSceneSnapshotService::Create(
                    RendererECSManager::GetInstance().GetWorld());
                const bool skeletons = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")() ==
                    static_cast<unsigned int>(OutputType::SKELETONS) && DeviceManager::GetInstance().GetMeshShadersSupported();
                if (skeletons) {
                    newGraph->BuildPass<DebugSkeletonPass>("DebugSkeletonPass", debugSnapshots);
                    newGraph->SetPassTechnique("DebugSkeletonPass", "Debug::Visualization");
                }
                else {
                    newGraph->BuildPass<DebugResolvePass>("DebugResolvePass");
                    newGraph->SetPassTechnique("DebugResolvePass", "Debug::Visualization");
                }
                if (getDrawBoundingSpheres()) {
                    newGraph->BuildPass<DebugSpherePass>("DebugSpherePass", debugSnapshots);
                    newGraph->SetPassTechnique("DebugSpherePass", "Debug::Visualization");
                }
                break;
            }
            case DebugUi:
                newGraph->BuildPass<MenuRenderPass>("MenuRenderPass");
                newGraph->SetPassTechnique("MenuRenderPass", "Debug::UI");
                break;
            case DepthHistory:
                BuildLinearDepthHistoryCopyPass(newGraph.get(), &m_depthHistory);
                break;
            case Present:
                newGraph->BuildPass<PresentationReadyPass>("PresentationReadyPass");
                newGraph->SetPassTechnique("PresentationReadyPass", "Frame::Present");
                break;
            }
        });

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::BuildTechniques");
	for (const auto& entry : m_pipelineRecipe.Techniques()) {
		entry.technique->Build(buildContext);
		probeGraphBuildPhase(("CreateRenderGraph after technique " + std::to_string(static_cast<uint32_t>(entry.id))).c_str());
	}
    }

    probeGraphBuildPhase("CreateRenderGraph before CompileStructural");

    newGraph->SetStructuralMaterializeCheckpointCallback([](std::string_view passName) {
        if (!passName.starts_with("CLodShadow::")) {
            return;
        }

        std::string phase = "CreateRenderGraph after structural pass ";
        phase += passName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    newGraph->SetStructuralMaterializeResourceCheckpointCallback([](std::string_view passName, std::string_view resourceName) {
        if (passName != "CLodShadow::VirtualShadowSetupPass") {
            return;
        }

        std::string phase = "CreateRenderGraph after structural resource ";
        phase += passName;
        phase += " :: ";
        phase += resourceName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    //newGraph->SetMinimumAutomaticSchedulingQueues(QueueKind::Compute, 3);

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::CompileStructural");
	spdlog::info("Renderer::CreateRenderGraph entering CompileStructural");
    newGraph->CompileStructural();
	spdlog::info("Renderer::CreateRenderGraph leaving CompileStructural");
    }
    if (const auto* persistent = std::getenv("SARP_PERSISTENT_GRAPH"); persistent && persistent[0] == '1') {
        // Segment placement is explicit: passes whose touched-resource set is
        // only known per frame cannot live in the persistent main executable.
        newGraph->SetPersistentSegment("Builtin::Uploads", org::RenderGraph::PersistentSegmentKind::Pre);
        newGraph->SetPersistentSegment("CLod::AsyncUpload", org::RenderGraph::PersistentSegmentKind::Tail);
        newGraph->SetPersistentSegment("Builtin::Readbacks", org::RenderGraph::PersistentSegmentKind::Tail);
        newGraph->SetPersistentSegment("CLod::StreamingReadbackCopy", org::RenderGraph::PersistentSegmentKind::Tail);
        newGraph->SetPersistentExecutionEnabled(true);
        spdlog::info("Persistent render graph execution enabled (SARP_PERSISTENT_GRAPH=1)");
    }
    probeGraphBuildPhase("CreateRenderGraph after CompileStructural");
    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::Setup");
	spdlog::info("Renderer::CreateRenderGraph entering Setup");
    newGraph->Setup();
	spdlog::info("Renderer::CreateRenderGraph leaving Setup");
    }
    probeGraphBuildPhase("CreateRenderGraph after Setup");

	rebuildRenderGraph = false;
}


void Renderer::ToggleMeshShaders(bool useMeshShaders) {
    // We need to:
    // 1. Remove all meshes in the global mesh library from the mesh manager
	// 2. Re-add them to the mesh manager
    // 3. Get all objects with mesh instances by querying the ECS
	// 4. Remove and re-add all instances to the mesh manager
	// 5. Remove and re-add all objects from the object manager to rebuild indirect draw info

    auto& world = RendererECSManager::GetInstance().GetWorld();
	auto& meshLibrary = world.get_mut<Components::GlobalMeshLibrary>().meshes;

	// Remove all meshes from the mesh manager
	for (auto& meshPair : meshLibrary) {
		auto& mesh = meshPair.second;
		m_pMeshManager->RemoveMesh(mesh.lock().get());
	}
	// Re-add them to the mesh manager
	for (auto& meshPair : meshLibrary) {
		auto& mesh = meshPair.second;
        auto ptr = mesh.lock();
        m_pMeshManager->AddMesh(ptr, useMeshShaders);
	}

	// Get all active objects with mesh instances by querying the ECS
    auto query = world.query_builder<Components::RenderableObject, Components::ObjectDrawInfo>().with<Components::Active>()
        .build();

    world.defer_begin();
    query.each([&](flecs::entity entity, Components::RenderableObject& object, const Components::ObjectDrawInfo& drawInfo) {
        auto meshInstances = entity.try_get<Components::MeshInstances>();

        if (meshInstances) {
            for (auto& meshInstance : meshInstances->meshInstances) {
                m_pMeshManager->RemoveMeshInstance(meshInstance.get());
                m_pMeshManager->AddMeshInstance(meshInstance.get(), useMeshShaders);
            }
        }

		// Remove and re-add all objects from the object manager to rebuild indirect draw info
		m_pObjectManager->RemoveObject(&drawInfo);
		auto newDrawInfo = m_pObjectManager->AddObject(object.perObjectCB, meshInstances);
		object.perObjectCB.normalMatrixBufferIndex = newDrawInfo.normalMatrixIndex;
		entity.set<Components::ObjectDrawInfo>(newDrawInfo);
		entity.add<Components::RenderTransformUpdated>();
            });
    world.defer_end();
}


void Renderer::LoadPipeline(HWND hwnd, UINT x_res, UINT y_res) {
    UINT dxgiFactoryFlags = 0;
	RECT clientRect{};
	if (hwnd && GetClientRect(hwnd, &clientRect)) {
		const UINT clientWidth = static_cast<UINT>((std::max)(clientRect.right - clientRect.left, 0L));
		const UINT clientHeight = static_cast<UINT>((std::max)(clientRect.bottom - clientRect.top, 0L));
		if (clientWidth != 0 && clientHeight != 0 && (clientWidth != x_res || clientHeight != y_res)) {
			spdlog::info(
				"Renderer: using actual client extent {}x{} for initial swapchain instead of requested {}x{}",
				clientWidth,
				clientHeight,
				x_res,
				y_res);
			x_res = clientWidth;
			y_res = clientHeight;
			SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ x_res, y_res });
		}
	}

    DeviceManager::GetInstance().Initialize();

	auto device = DeviceManager::GetInstance().GetDevice();

    auto result = device.CreateSwapchain(hwnd, x_res, y_res, rhi::Format::R8G8B8A8_UNorm, m_numFramesInFlight, m_allowTearing, m_swapChain);

    UpscalingManager::GetInstance().InitializeAdapter();

    // Create RTV descriptor heap
	rhi::DescriptorHeapDesc rtvHeapDesc = {};
    rtvHeapDesc.capacity = m_numFramesInFlight;
    rtvHeapDesc.type = rhi::DescriptorHeapType::RTV;
	rtvHeapDesc.shaderVisible = false;
	rtvHeapDesc.debugName = "RTV Descriptor Heap";
    result = device.CreateDescriptorHeap(rtvHeapDesc, rtvHeap);

    rtvDescriptorSize = device.GetDescriptorHandleIncrementSize(rhi::DescriptorHeapType::RTV);

    // Create frame resources
    renderTargets.resize(m_numFramesInFlight);
    for (UINT n = 0; n < m_numFramesInFlight; n++) {        
        renderTargets[n] = m_swapChain->Image(n);
    }

    // Wrap swapchain images for render-graph tracking
    m_backbufferResources.resize(m_numFramesInFlight);
    for (UINT n = 0; n < m_numFramesInFlight; n++) {
        m_backbufferResources[n] = std::make_shared<org::ExternalTextureResource>(
            renderTargets[n], x_res, y_res, rhi::Format::R8G8B8A8_UNorm);
        m_backbufferResources[n]->SetName("Backbuffer " + std::to_string(n));
    }
    m_dynamicBackbuffer = std::make_shared<org::DynamicResource>(m_backbufferResources[0]);
    m_dynamicBackbuffer->SetName("Backbuffer");

    CreateRTVs();
    m_swapChainReady = true;
    m_loggedSwapChainNotReady = false;

    // Create command allocator

	m_commandAllocators.resize(m_numFramesInFlight);
	m_commandLists.resize(m_numFramesInFlight);
    for (int i = 0; i < m_numFramesInFlight; i++) {
        rhi::CommandAllocatorPtr commandAllocator;
        rhi::CommandListPtr commandList;
        result = device.CreateCommandAllocator(rhi::QueueKind::Graphics, commandAllocator);
        result = device.CreateCommandList(rhi::QueueKind::Graphics, commandAllocator.Get(), commandList);
		m_commandAllocators[i] = std::move(commandAllocator);
		m_commandLists[i] = std::move(commandList);
        m_commandLists[i]->End();
    }

    // Create per-frame fence information
	m_frameFenceValues.resize(m_numFramesInFlight);
	for (int i = 0; i < m_numFramesInFlight; i++) {
		m_frameFenceValues[i] = 0;
	}

    m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());
    m_preparationFrameIndex = m_frameIndex;
    if (m_dynamicBackbuffer && m_frameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[m_frameIndex]);
    }

    result = device.CreateTimeline(m_frameFence);
    result = device.CreateTimeline(m_readbackFence);
    result = device.CreateTimeline(m_copyReadbackFence);
    result = device.CreateTimeline(m_legacyReadbackFence);
}


void Renderer::RegisterPipelineExtensions() {
    currentRenderGraph->RegisterExtension(std::make_unique<RenderGraphIOExtension>(
        std::make_shared<br::render::RenderGraphIOService>(
            *m_pTextureFactory,
            currentRenderGraph->RetainUploadService(),
            *m_pReadbackManager,
            *m_pMaterialManager)),
        "BuiltinIO");
    currentRenderGraph->RegisterExtension(std::make_unique<ReadbackCaptureExtension>(
        currentRenderGraph->GetReadbackServiceOwner()),
        "BuiltinReadbackCapture");

    if (!m_producerPersistentState->clodStreaming) m_producerPersistentState->clodStreaming = std::make_shared<CLodStreamingSystem>();
    if (!m_producerPersistentState->virtualShadowCasters) m_producerPersistentState->virtualShadowCasters = std::make_shared<VirtualShadowCasterRegistry>();
    auto clodStreamingSystem = m_producerPersistentState->clodStreaming;
    clodStreamingSystem->SetGeometryStorage(&m_pMeshManager->GetCLodGeometryStorage());
    auto virtualShadowCasters = m_producerPersistentState->virtualShadowCasters;
    br::pipeline::PipelineBuildContext extensionContext(
        *currentRenderGraph,
        m_pipelineRecipe.Bindings(),
        {},
        [this, clodStreamingSystem, virtualShadowCasters](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions& optionsVariant) {
            CLodExtensionType extensionType{};
            const char* extensionId = nullptr;
            switch (id) {
            case br::pipeline::TechniqueId::ClusterLod:
                extensionType = CLodExtensionType::VisiblityBuffer;
                extensionId = "CLodOpaque";
                break;
            case br::pipeline::TechniqueId::ClusterLodAlpha:
                extensionType = CLodExtensionType::AlphaBlend;
                extensionId = "CLodAlpha";
                break;
            case br::pipeline::TechniqueId::ClusterLodShadow:
                extensionType = CLodExtensionType::Shadow;
                extensionId = "CLodShadow";
                break;
            default:
                return;
            }

            const auto& options = std::get<br::pipeline::ClusterLodOptions>(optionsVariant);
            const bool voxelRasterizationEnabled =
                m_pipelineRecipe.Contains<br::pipeline::ClusterLodVoxelTechnique>();
            const uint32_t voxelRasterWorkCapacity = voxelRasterizationEnabled
                ? m_pipelineRecipe.Options<br::pipeline::ClusterLodVoxelTechnique>().workRecordCapacity
                : 0u;
            const uint32_t maxClusters = std::clamp(
                SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodVisibleClusterCapacitySettingName)(),
                CLodMinVisibleClusterCapacity,
                CLodMaxVisibleClusterCapacity);
            currentRenderGraph->RegisterExtension(
                std::make_unique<CLodExtension>(
                    extensionType,
                    maxClusters,
                    CLodExtensionOptions{
                        .enableReyes = options.reyes == br::pipeline::ReyesMode::Enabled,
                        .enableVoxelRasterization = voxelRasterizationEnabled,
                        .voxelRasterWorkCapacity = voxelRasterWorkCapacity,
                        .streamingSystem = clodStreamingSystem,
                        .virtualShadowCasters = virtualShadowCasters,
                        .persistentState = m_producerPersistentState,
                        .slabResourceGroup = m_pMeshManager
                            ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr }),
                extensionId);
        });
    // Recipe extensions may register resources consumed by technique extensions
    // (ProceduralWind visibility is consumed by CLod shadow traversal). Register
    // producers first; structural insertion constraints are resolved only after
    // every extension has been gathered, so this does not require their target
    // passes to have been materialized yet.
    for (const auto& [id, factory] : m_pipelineRecipe.Extensions()) {
		if (id == "SARPGrass" && br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_DISABLE_SARP_GRASS_EXTENSION")) {
			spdlog::info("Render-graph isolation: SARPGrass extension disabled");
			continue;
		}
        auto extension = factory();
        if (!extension) {
            throw std::runtime_error("Pipeline extension factory returned null: " + id);
        }
        if (auto* casterProvider = dynamic_cast<IVirtualShadowCasterProvider*>(extension.get())) {
            virtualShadowCasters->Register(*casterProvider);
        }
        currentRenderGraph->RegisterExtension(std::move(extension), id);
    }
    for (const auto& entry : m_pipelineRecipe.Techniques()) {
        entry.technique->RegisterExtensions(extensionContext);
    }
}
