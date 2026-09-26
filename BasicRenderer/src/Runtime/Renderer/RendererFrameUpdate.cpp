//
// Created by matth on 6/25/2024.
//

#include <BasicRenderer/Renderer.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"
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
#include "Lighting/Shading/RenderPasses/ForwardRenderPass.h"
#include "Lighting/Environment/RenderPasses/EnvironmentFilterPass.h"
#include "ThirdParty/XeGTAO.h"
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
#include "Runtime/GraphIntegration/StateProducerRegistrations.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
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
#include "Lighting/Environment/EnvironmentManager.h"
#if BASICRENDERER_HAS_INTEROP_VALIDATION
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


void Renderer::Update(float elapsedSeconds) {
    if (m_deterministicSamplingMode) {
        elapsedSeconds = 0.0f;
    }
    BT_ZONE_SCOPE("Renderer::Update");
    // Capture input before camera application clears the per-frame mouse delta.
    // Used only by the opt-in camera telemetry stream below.
    const auto cameraTelemetryMovement = movementState;
    const float cameraTelemetryPitch = verticalAngle;
    const float cameraTelemetryYaw = horizontalAngle;
    // The previous accepted owner remains retained by its queued graph work.
    // Clear the renderer-side alias so any early return applies backpressure
    // instead of rendering the preceding logical frame twice.
    m_frameInputs.reset();
    org::BufferBase::ScopedBackingMutation frameBoundaryBackingMutation;

	std::vector<PSOManager::PipelineRetirementPoint> pipelineRetirementPoints;
	for (const auto& point : org::DescriptorHeapManager::GetInstance().GetQueueFenceSnapshot()) {
		pipelineRetirementPoints.push_back({ point.timeline, point.value });
	}
	PSOManager::GetInstance().PublishPendingLivePipelines(std::move(pipelineRetirementPoints));
	PSOManager::GetInstance().CollectRetiredLivePipelines();

    BeginFrameTaskGraphCapture();

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        try {
            stageFn();
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Renderer::Update stage '") + stageName + "' failed: " + error.what());
        }
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };

    runCapturedStage("PublishDeferredBackingResizesEarly", []() {
        BT_ZONE_SCOPE("Renderer::Update::PublishDeferredBackingResizesEarly");
        // Publication is opportunistic at the frame boundary. Waiting here
        // defeats the asynchronous resize path and can park the render thread
        // behind backing creation for tens of milliseconds.
        (void)org::PublishReadyDeferredBackingResizes(false);
    });

    if (!IsSceneReadyForFrame()) {
        return;
    }

    if (m_shaderReloadRequested) {
        runCapturedStage("ShaderReload", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::ShaderReload");
            spdlog::info("Renderer: draining GPU work before shader reload.");
            StallPipeline();
            std::string rebuildError;
            if (PSOManager::GetInstance().RebuildAllPipelines(rebuildError)) {
                rebuildRenderGraph = true;
            } else {
                spdlog::error("Renderer: global PSO rebuild failed: {}", rebuildError);
            }
            m_shaderReloadRequested = false;
        });
    }

    runCapturedStage("SceneExplorerEdits", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::SceneExplorerEdits");
        if (!m_externalSceneMode) {
            FlushPendingSceneExplorerEdits();
        }
    });
    if (m_externalSceneMode) {
        runCapturedStage("ExternalScene", []() {
            BT_ZONE_SCOPE("Renderer::Update::ExternalScene");
        });
    } else if (m_sceneRenderOverlapEnabled) {
        if (NeedsSceneSnapshotBootstrap()) {
            runCapturedStage("BootstrapSceneSnapshot", [&]() {
                BT_ZONE_SCOPE("Renderer::Update::BootstrapSceneSnapshot");
                ApplyPrimaryCameraInput(elapsedSeconds);
                RunGameUpdateStage(elapsedSeconds);
                RunTransformPropagationStage();
                BootstrapCommittedSceneSnapshot();
            });
        } else {
            runCapturedStage("CommitSceneSnapshot", [&]() {
                BT_ZONE_SCOPE("Renderer::Update::CommitSceneSnapshot");
                CommitCompletedSceneSnapshot();
            });
        }
    } else {
        runCapturedStage("SynchronousSceneUpdate", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::SynchronousSceneUpdate");
            ApplyPrimaryCameraInput(elapsedSeconds);
            RunGameUpdateStage(elapsedSeconds);
            RunTransformPropagationStage();
            RunSceneBridgeSyncStage();
        });
    }

    runCapturedStage("AnimationUpdate", [&]() {
		m_pSkeletonManager->BeginFrame(m_totalFramesRendered);
        if (m_externalSceneMode) {
            m_pSkeletonManager->UpdateAllDirtyInstances();
        } else {
            RunAnimationUpdateStage(elapsedSeconds);
        }
    });
    // Flush deferred functions before rebuilding the render graph so that
    // deferred state changes (e.g. environment creation from SetEnvironmentInternal)
    // are visible when the graph is constructed.
    if (!m_preFrameDeferredFunctions.empty()) {
        runCapturedStage("DeferredWorkEarly", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::DeferredWorkEarly");
            m_preFrameDeferredFunctions.flush();
        });
    }
    SyncOpenRenderGraphSettings(m_numFramesInFlight);
    const bool asyncFrameQueue = SettingsManager::GetInstance()
        .getSettingGetter<int>("experimentalAsyncCompileMode")() == 2;
    if (!asyncFrameQueue) m_preparationFrameIndex = m_frameIndex;
    ApplyPendingPipelineReplacement();

    if (rebuildRenderGraph) {
        runCapturedStage("RenderGraphBuild", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::RenderGraphBuild");
            try {
		        CreateRenderGraph();
                m_pipelineRollbackRecipe.reset();
            }
            catch (const std::exception& error) {
                if (!m_pipelineRollbackRecipe) {
                    throw;
                }
                HandlePipelineReplacementFailure(error);
                CreateRenderGraph();
            }
        });
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after RenderGraphBuild");
    }
    runCapturedStage("RenderResourceSync", [&]() {
        RunRenderResourceSyncStage();
    });

    auto& world = RendererECSManager::GetInstance().GetWorld();

    auto camera = GetValidatedPrimaryRenderCamera(false);
    if (!camera) {
        camera = GetValidatedPrimaryRenderCamera(true);
    }
    if (!camera) {
        spdlog::warn("Renderer: bridged primary camera is unavailable after scene sync. Skipping frame update work.");
        return;
    }
    unsigned int cameraIndex = m_pViewManager->Get(camera.get<Components::RenderViewRef>().viewID)->gpu.cameraBufferIndex;

    runCapturedStage("WaitForFrame", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::WaitForFrame");
        WaitForFrame(m_preparationFrameIndex);
        if (m_pObjectManager) {
            const std::uint64_t retireDelayFrames = static_cast<std::uint64_t>(m_numFramesInFlight) + 1u;
            const std::uint64_t safeFrameNumber = m_totalFramesRendered > retireDelayFrames
                ? m_totalFramesRendered - retireDelayFrames
                : 0u;
            m_pObjectManager->PublishDeferredRetireCompletedFrame(safeFrameNumber, retireDelayFrames);
        }
        org::DescriptorHeapManager::GetInstance().ProcessDeferredReleases(m_preparationFrameIndex);
        RendererECSManager::GetInstance().FlushDeferredWorldOperations();

		// Retire upload pages only after the previous use of this frame slot has
		// completed, and before CompileFrame can assign newly recorded uploads to
		// the slot.  Doing this in FrameMaintenance (after RenderGraph::Update)
		// recycled current-frame staging pages before RenderGraph::Execute had
		// submitted their copies.
		if (currentRenderGraph) {
			if (auto* uploadService = currentRenderGraph->GetUploadService()) {
				uploadService->ProcessDeferredReleases(m_preparationFrameIndex);
			}
		}
        });

    runCapturedStage("CommitPublishedRendererState", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState");
        try {
            if (m_rendererStatePublisher) {
                auto commit = [&] {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::PublisherCommit");
                    return m_rendererStatePublisher->Commit(m_preparationFrameIndex);
                }();
				if (m_asyncStateGraph && m_asyncStateGraph->TraceActive()) {
					m_asyncStateGraph->TraceEvent(
						commit.committed ? br::render::AsyncStateGraphTraceEventID::ManifestCommitAccepted :
							br::render::AsyncStateGraphTraceEventID::ManifestCommitUnchanged,
						{ br::render::ArtifactKind::FrameManifest, 0, 0 },
						commit.state ? commit.state->epoch : 0, 0,
						{ { m_preparationFrameIndex } });
					if (commit.committed && commit.state) {
						for (std::size_t index = 0; index < br::render::kPublishedFragmentCount; ++index) {
							const auto& fragment = commit.state->Fragment(
								static_cast<br::render::PublishedFragmentKind>(index));
							if (!fragment.publicationRoot) continue;
							m_asyncStateGraph->TraceEvent(br::render::AsyncStateGraphTraceEventID::ManifestFragmentCommitted,
								fragment.publicationRoot.address, fragment.publicationRoot.revision,
								fragment.publicationRoot.generation,
								{ { commit.state->epoch, m_preparationFrameIndex, index } });
						}
					}
				}
                {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::InstallFrameLease");
                m_context.publishedRendererState = commit.state;
                m_context.publishedManifestLease = commit.lease;
                }
                if (commit.committed && commit.state) {
                    // Versioned table families need the same publication
                    // acknowledgement as object/material tables. Without it,
                    // their bounded backing rings retain the original active
                    // generation forever; after a few camera updates the next
                    // view build suspends permanently on ring exhaustion.
                    const auto acknowledgeTables = [](const auto& families, const auto& state) {
                        if (!state) return;
                        const auto count = (std::min)(families.size(), state->tableVersions.size());
                        for (std::size_t i = 0; i < count; ++i) {
                            if (families[i]) families[i]->Acknowledge(state->tableVersions[i]);
                        }
                    };
                    acknowledgeTables(m_lightTableFamilies,
                        commit.state->lights.payload.Get<br::render::PublishedLightTableState>());
                    const auto poseState = commit.state->poses.payload.Get<br::render::PublishedPoseState>();
                    acknowledgeTables(m_poseTableFamilies, poseState);
                    if (poseState && m_pSkeletonManager) {
                        for (const auto& version : poseState->tableVersions)
                            m_pSkeletonManager->AcknowledgeInverseBindGraphState(version);
                    }
                }
                if (m_rendererStateRequests) {
                    m_rendererStateRequests->RefreshPublication();
                }
                if (commit.committed && commit.state && m_asyncStateGraph) {
                    auto publishedVersions = std::make_shared<
                        std::vector<br::render::ArtifactVersionID>>();
                    for (std::size_t index = 0; index < br::render::kPublishedFragmentCount; ++index) {
                        const auto& fragment = commit.state->Fragment(
                            static_cast<br::render::PublishedFragmentKind>(index));
                        if (fragment.publicationBundle) {
                            for (const auto& version : fragment.publicationBundle->versions) {
                                if (version && !std::ranges::contains(*publishedVersions, version))
                                    publishedVersions->push_back(version);
                            }
                        } else if (fragment.publicationRoot) {
                            publishedVersions->push_back(fragment.publicationRoot);
                        }
                    }
                    // The acknowledgement is deliberately level-triggered: a
                    // version whose node is not yet in a publishable state is
                    // skipped by the graph and must be re-acknowledged by the next
                    // commit. Sending only the delta against the previous commit
                    // turns such a skip into a permanent loss of publication.
                    // MarkPublished itself now ignores already-published versions.
                    const bool acknowledgementSubmitted =
                        TaskSchedulerManager::GetInstance().Submit(
                        m_rendererStateCommitScope, TaskLane::FrameCritical,
                        TaskDomain::GraphPublication, "RendererStatePublisher::MarkPublished",
                        [stateGraph = m_asyncStateGraph.get(), publishedVersions](
                            const br::TaskContext& context) {
                            if (!context.StopRequested() && stateGraph) {
                                stateGraph->MarkPublished(*publishedVersions);
                            }
                        });
                    if (!acknowledgementSubmitted) {
                        spdlog::warn("Renderer-state graph publication acknowledgement was rejected");
                    }
                }
                // Every commit, not only those with deferred cleanup: a manifest
                // that installs covering geometry may carry no deferred work.
                if (commit.committed && commit.state && m_pObjectManager) {
                    m_pObjectManager->ObserveResidentGeometry(*commit.state);
                }
                if (commit.HasDeferredWork()) {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ScheduleDeferredCommit");
                    auto* objectManager = commit.committed ? m_pObjectManager.get() : nullptr;
					auto* meshManager = commit.committed ? m_pMeshManager.get() : nullptr;
					auto* materialManager = commit.committed ? m_pMaterialManager.get() : nullptr;
                    auto committedState = commit.committed ? commit.state : nullptr;
                    // Publication acknowledgement advances object-buffer mutation
                    // coverage and wakes the next admissible graph cut.  Keeping it
                    // in Background/Cleanup allowed an import-time cleanup flood to
                    // strand otherwise committed renderer state indefinitely.  Run
                    // the deferred handoff off-thread in the serialized renderer-
                    // state domain, where streaming work has bounded precedence.
                    const bool submitted = TaskSchedulerManager::GetInstance().Submit(
                        m_rendererStateCommitScope, TaskLane::Streaming, TaskDomain::RendererState,
                        "RendererStatePublisher::DeferredCommit",
						[commit = std::move(commit), objectManager, meshManager, materialManager,
                            committedState = std::move(committedState)](
                            const br::TaskContext& context) mutable {
                            auto deferred = std::move(commit);
                            auto state = std::move(committedState);
                            if (context.StopRequested()) return;
                            if (objectManager && state) {
                                objectManager->AcknowledgePublishedBufferState(state);
                            }
							if (meshManager && state) {
								meshManager->AcknowledgePublishedBufferState(state);
							}
							if (materialManager && state) {
								materialManager->AcknowledgePublishedTextureImageTable(state);
							}
                            deferred.RunDeferred();
                        });
                    if (!submitted) {
                        spdlog::warn("Renderer-state deferred commit cleanup was rejected by the scheduler");
                    }
                }
            } else {
                m_context.publishedRendererState.reset();
                m_context.publishedManifestLease.reset();
            }
        } catch (const std::exception& exception) {
            spdlog::critical("CommitPublishedRendererState: publisher Commit failed: {}", exception.what());
            throw;
        }
		if (m_pMaterialManager) {
			BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ActivateMaterials");
			(void)m_pMaterialManager->TryActivatePublishedMaterialState(
				m_context.publishedRendererState);
		}
		if (m_sceneIngestionServices.sceneAssetRequests) {
			BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ActivateTerrain");
			(void)m_sceneIngestionServices.sceneAssetRequests->ActivatePublishedTerrainState(
				m_context.publishedRendererState);
		}
    });

	// Final material bindings are published only after the reusable frame slot is
	// known idle. The transfer service pumps its own graphics work here, before
	// material-buffer uploads are captured by CompileFrame.
	if (m_pMaterialManager) {
		runCapturedStage("MaterialTexturePublication", [&]() {
			BT_ZONE_SCOPE("Renderer::Update::MaterialTexturePublication");
			m_pMaterialManager->ProcessPendingMaterialUpdates(m_totalFramesRendered + 1u);
		});
	}

    if (m_dynamicBackbuffer && m_swapChainReady && m_frameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[m_frameIndex]);
    }

    auto& resourceManager = ::ResourceManager::GetInstance();
    auto res = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    runCapturedStage("PerFrameBuffer", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::PerFrameBuffer");
        resourceManager.UpdatePerFrameBuffer(cameraIndex, m_pLightManager->GetNumLights(), { res.x, res.y }, m_lightClusterSize, static_cast<uint32_t>(m_totalFramesRendered));
    });

    static const basic_telemetry::Callsite captureFrameInputsCallsite("Renderer::Update::CaptureFrameInputs");
    std::optional<basic_telemetry::Scope> captureFrameInputsScope;
    captureFrameInputsScope.emplace(captureFrameInputsCallsite);
    const Components::DrawStats& drawStats = world.get<Components::DrawStats>();
    auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    auto outputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    UpdateContext updateData{};
    updateData.publishedRendererState = m_context.publishedRendererState;
    updateData.publishedManifestLease = m_context.publishedManifestLease;
    updateData.drawStats = drawStats;
    {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::StreamingStats");
        updateData.materialTextureStreamingStats = m_pMaterialManager
            ? m_pMaterialManager->GetMaterialTextureStreamingStats()
            : MaterialTextureStreamingStats{};
    }
    updateData.environmentWork = m_environmentWorkServices;
    {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::EnvironmentTelemetry");
        m_environmentWorkServices.PublishTelemetry();
    }
    updateData.clodRayTracingSystem = m_clodRayTracingSystem;
    updateData.terrainRegionMaterialEvaluationEnabled =
        SettingsManager::GetInstance().getSettingGetter<bool>(
            "enableTerrainRegionMaterialEvaluation")();
    updateData.outputType = SettingsManager::GetInstance()
        .getSettingGetter<unsigned int>("outputType")();
    updateData.tonemapType = SettingsManager::GetInstance()
        .getSettingGetter<unsigned int>("tonemapType")();
    updateData.lightClusterSize = SettingsManager::GetInstance()
        .getSettingGetter<DirectX::XMUINT3>("lightClusterSize")();
    updateData.lighting = {
        .imageBasedLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableImageBasedLighting")(),
        .punctualLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enablePunctualLighting")(),
        .shadowsEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableShadows")(),
        .gtaoEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableGTAO")(),
        .clusteredLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableClusteredLighting")() };
    {
    BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::Settings");
    updateData.proceduralWind = {
        .displacementScale = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindDisplacementScaleSettingName)(),
        .innerRadius = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindInnerRadiusSettingName)(),
        .outerRadius = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindOuterRadiusSettingName)(),
        .skeletonLodQualityCurve = SettingsManager::GetInstance().getSettingGetter<std::vector<float>>(
            ProceduralWindSkeletonLodQualityCurveSettingName)(),
        .skeletonLodStaticCutoff = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodStaticCutoffSettingName)(),
        .skeletonLodHysteresis = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodHysteresisSettingName)(),
        .forcedSkeletonLod = SettingsManager::GetInstance().getSettingGetter<int32_t>(
            ProceduralWindForcedSkeletonLodSettingName)(),
        .skeletonLodCapacityTarget = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodCapacityTargetSettingName)(),
        .skeletonLodLateReserve = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodLateReserveSettingName)(),
        .occlusionCullingEnabled = SettingsManager::GetInstance().getSettingGetter<bool>(
            "enableOcclusionCulling")() };
    }
    const auto publishedMaterialState = updateData.publishedRendererState
        ? updateData.publishedRendererState->materials.payload
            .Get<br::render::PublishedMaterialState>()
        : nullptr;
    if (publishedMaterialState) {
        updateData.preparedRasterBucketFlags = publishedMaterialState->rasterBucketFlags;
        updateData.preparedRasterBucketCount =
            static_cast<uint32_t>(updateData.preparedRasterBucketFlags.size());
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedRasterBuckets");
    }
    updateData.windPaletteService = m_pSkeletonManager;
    updateData.textureDescriptorHeap = m_context.textureDescriptorHeap;
    updateData.samplerDescriptorHeap = m_context.samplerDescriptorHeap;
    updateData.rtvHeap = rtvHeap->GetHandle();
    updateData.primaryCamera = camera.get<Components::Camera>();
    const auto desiredPrimaryCamera = updateData.primaryCamera;
    updateData.primaryViewID = m_context.primaryViewID;
    updateData.hasPrimaryCamera = true;
    updateData.frameSlot = m_preparationFrameIndex;
    updateData.frameIndex = updateData.frameSlot;
    updateData.frameFenceValue = m_currentFrameFenceValue;
    updateData.frameNumber = m_totalFramesRendered;
    updateData.renderResolution = renderRes;
    updateData.outputResolution = outputRes;
    if (m_imageBasedLighting) updateData.globalPSOFlags |= PSOFlags::PSO_IMAGE_BASED_LIGHTING;
    if (m_clusteredLighting) updateData.globalPSOFlags |= PSOFlags::PSO_CLUSTERED_LIGHTING;
    if (m_screenSpaceReflections || (m_rayTracedReflections && DeviceManager::GetInstance().GetCLodRayTracingSupported()))
        updateData.globalPSOFlags |= PSOFlags::PSO_SCREENSPACE_REFLECTIONS;
    updateData.deltaTime = elapsedSeconds;

    // The executable-frame request owns the logical render snapshot used by
    // transitional packets. It must never reinterpret UpdateContext as the
    // differently-laid-out RenderContext during delayed recording.
    std::optional<basic_telemetry::Scope> snapshotCopyScope;
    static const basic_telemetry::Callsite snapshotCopyCallsite("Renderer::Update::CaptureFrameInputs::SnapshotCopy");
    snapshotCopyScope.emplace(snapshotCopyCallsite);
    auto renderSnapshot = m_context;
    snapshotCopyScope.reset();
    renderSnapshot.publishedRendererState = updateData.publishedRendererState;
    renderSnapshot.publishedManifestLease = updateData.publishedManifestLease;
    renderSnapshot.primaryCamera = updateData.primaryCamera;
    renderSnapshot.primaryViewID = updateData.primaryViewID;
    renderSnapshot.hasPrimaryCamera = updateData.hasPrimaryCamera;
    renderSnapshot.frameSlot = updateData.frameSlot;
    renderSnapshot.frameIndex = renderSnapshot.frameSlot;
    renderSnapshot.swapchainImageIndex = UINT_MAX;
    renderSnapshot.frameFenceValue = updateData.frameFenceValue;
    renderSnapshot.frameNumber = updateData.frameNumber;
    renderSnapshot.renderResolution = updateData.renderResolution;
    renderSnapshot.outputResolution = updateData.outputResolution;
    renderSnapshot.globalPSOFlags = updateData.globalPSOFlags;
    renderSnapshot.terrainRegionMaterialEvaluationEnabled =
        updateData.terrainRegionMaterialEvaluationEnabled;
    renderSnapshot.outputType = updateData.outputType;
    renderSnapshot.tonemapType = updateData.tonemapType;
    renderSnapshot.lightClusterSize = updateData.lightClusterSize;
    renderSnapshot.lighting = updateData.lighting;
    renderSnapshot.proceduralWind = updateData.proceduralWind;
    renderSnapshot.deltaTime = updateData.deltaTime;
    auto currentViews = std::make_shared<br::render::PreparedViewFamilyState>();
    br::render::PrimaryCameraFrameUpload primaryCameraUpload{};
    if (m_pViewManager) {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::ViewFamily");
        currentViews->revision = m_pViewManager->GetPublicationRevision();
        currentViews->cameraBufferSize = m_pViewManager->GetCameraBufferSize();
        primaryCameraUpload = m_pViewManager->CapturePrimaryCameraUpload(updateData.frameNumber);
        if (auto cameraBuffer = m_pViewManager->ProvideResource(Builtin::CameraBuffer))
            currentViews->retainedResources.push_back(std::move(cameraBuffer));
        if (auto cullingBuffer = m_pViewManager->ProvideResource(Builtin::CullingCameraBuffer))
            currentViews->retainedResources.push_back(std::move(cullingBuffer));
        const auto appendView = [&](uint64_t viewID) {
            auto* view = m_pViewManager->Get(viewID);
            if (!view) return;
            if (view->gpu.visibilityBuffer && !view->gpu.clodDeepVisibilityHeadPointers) {
                (void)m_pViewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
                view = m_pViewManager->Get(viewID);
                if (!view) return;
            }
            currentViews->views.push_back({
                .id = view->id,
                .cameraBufferIndex = view->gpu.cameraBufferIndex,
                .primary = view->flags.primaryCamera,
                .shadow = view->flags.shadow,
                .cascade = view->flags.cascaded,
                .lightType = view->lightType,
                .cameraInfo = view->cameraInfo,
                .jitterPixelSpace = view->flags.primaryCamera
                    ? desiredPrimaryCamera.jitterPixelSpace : DirectX::XMFLOAT2{},
                .jitterNDC = view->flags.primaryCamera
                    ? desiredPrimaryCamera.jitterNDC : DirectX::XMFLOAT2{},
                .visibilityBuffer = view->gpu.visibilityBuffer,
                .deepVisibilityHeadPointers = view->gpu.clodDeepVisibilityHeadPointers,
                .linearDepthMap = view->gpu.linearDepthMap,
                .depthHistory = m_depthHistory.Select(view->id, view->gpu.linearDepthMap),
                .depthBufferArrayIndex = view->cameraInfo.depthBufferArrayIndex,
            });
            if (view->gpu.visibilityBuffer)
                currentViews->retainedResources.push_back(view->gpu.visibilityBuffer);
            if (view->gpu.clodDeepVisibilityHeadPointers)
                currentViews->retainedResources.push_back(view->gpu.clodDeepVisibilityHeadPointers);
            if (view->gpu.linearDepthMap)
                currentViews->retainedResources.push_back(view->gpu.linearDepthMap);
        };
        // Primary is a structural invariant: every frame snapshot starts with it.
        appendView(updateData.primaryViewID);
        m_pViewManager->ForEachView([&](uint64_t viewID) {
            if (viewID != updateData.primaryViewID) appendView(viewID);
        });
        currentViews->resourceLayoutRevision = m_pViewManager->GetResourceLayoutRevision();
        if (currentViews->views.empty() || !currentViews->views.front().primary ||
            currentViews->views.front().cameraBufferIndex != 0) {
            throw std::logic_error("Accepted frame is missing primary view slot zero");
        }
    }
    updateData.viewFamily = currentViews;
    renderSnapshot.viewFamily = currentViews;
    const auto& desiredCameraPosition = desiredPrimaryCamera.info.positionWorldSpace;
    BT_PLOT("SARP.Camera.Desired.X", static_cast<int64_t>(desiredCameraPosition.x));
    BT_PLOT("SARP.Camera.Desired.Y", static_cast<int64_t>(desiredCameraPosition.y));
    BT_PLOT("SARP.Camera.Desired.Z", static_cast<int64_t>(desiredCameraPosition.z));
    BT_PLOT("SARP.Camera.CapturedRevision", static_cast<int64_t>(primaryCameraUpload.revision));
    static const std::filesystem::path cameraTelemetryPath = [] {
        wchar_t* value = nullptr;
        size_t length = 0;
        _wdupenv_s(&value, &length, L"SARP_CAMERA_TELEMETRY_PATH");
        std::filesystem::path result = value && value[0] ? value : L"";
        std::free(value);
        return result;
    }();
    if (!cameraTelemetryPath.empty()) {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::CameraTelemetryFile");
        std::error_code fileError;
        if (cameraTelemetryPath.has_parent_path())
            std::filesystem::create_directories(cameraTelemetryPath.parent_path(), fileError);
        const bool writeHeader = !std::filesystem::exists(cameraTelemetryPath, fileError) ||
            std::filesystem::file_size(cameraTelemetryPath, fileError) == 0u;
        std::ofstream output(cameraTelemetryPath, std::ios::app);
        if (writeHeader) {
            output << "frame,dt,forward,backward,left,right,up,down,pitch,yaw,camera_revision,x,y,z\n";
        }
        output << m_totalFramesRendered << ',' << elapsedSeconds << ','
            << cameraTelemetryMovement.forwardMagnitude << ',' << cameraTelemetryMovement.backwardMagnitude << ','
            << cameraTelemetryMovement.leftMagnitude << ',' << cameraTelemetryMovement.rightMagnitude << ','
            << cameraTelemetryMovement.upMagnitude << ',' << cameraTelemetryMovement.downMagnitude << ','
            << cameraTelemetryPitch << ',' << cameraTelemetryYaw << ',' << primaryCameraUpload.revision << ','
            << desiredCameraPosition.x << ',' << desiredCameraPosition.y << ',' << desiredCameraPosition.z << '\n';
    }
    basic_telemetry::SetGauge("SARP.FrameInputs.ViewFamily.ViewCount",
        static_cast<std::int64_t>(renderSnapshot.Views().size()));
	const auto publishedObjects = updateData.publishedRendererState
		? updateData.publishedRendererState->drawRecords.payload
			.Get<br::render::PublishedObjectBufferState>()
		: nullptr;
	if (publishedObjects) {
		basic_telemetry::AddCounter("SARP.FrameInputs.PublishedObjectSelection");
		basic_telemetry::SetGauge("SARP.FrameInputs.ObjectSnapshot.PlacementCount",
			publishedObjects->placementRecords
				? static_cast<std::int64_t>(publishedObjects->placementRecords->size()) : 0);
		basic_telemetry::SetGauge("SARP.FrameInputs.ObjectSnapshot.ActivePlacementCount",
			publishedObjects->activePlacementEntries
				? static_cast<std::int64_t>(publishedObjects->activePlacementEntries->size()) : 0);
	}
	const auto publishedStaticScene = updateData.publishedRendererState
		? updateData.publishedRendererState->geometry.payload
			.Get<br::render::PublishedStaticSceneState>()
		: nullptr;
	if (publishedStaticScene) {
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.DesiredPlacements",
			static_cast<std::int64_t>(publishedStaticScene->desiredPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.PublishedPlacements",
			static_cast<std::int64_t>(publishedStaticScene->publishedPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.MaterializedPlacements",
			static_cast<std::int64_t>(publishedStaticScene->materializedPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.GeometryRevision",
			static_cast<std::int64_t>(updateData.publishedRendererState->geometry.revision));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.ManifestEpoch",
			static_cast<std::int64_t>(updateData.publishedRendererState->epoch));
	}
    renderSnapshot.preparedRasterBucketCount = updateData.preparedRasterBucketCount;
    renderSnapshot.preparedRasterBucketFlags = updateData.preparedRasterBucketFlags;

    // Frame-input artifacts are posted, never submitted: the owner thread
    // does not enter the state-graph mutex. Buffer snapshots precede the
    // fragment intents that reference them by revision.
    std::vector<br::render::ArtifactIntent> frameIntents;
    std::shared_ptr<br::render::LightTableBuildInput> desiredLights;
    if (m_pLightManager && m_rendererStateRequests) {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::LightTables");
        const auto lightSourceRevision = (std::max<std::uint64_t>)(
            m_pLightManager->GetPublicationRevision(), 1u);
        const auto lightViewRevision = std::uint64_t{0};
        if (lightSourceRevision != m_lastLightSourceRevision ||
            lightViewRevision != m_lastLightViewFamilyRevision) {
            ++m_lightArtifactRevision;
            m_lastLightSourceRevision = lightSourceRevision;
            m_lastLightViewFamilyRevision = lightViewRevision;
            desiredLights = std::make_shared<br::render::LightTableBuildInput>();
        desiredLights->revision = m_lightArtifactRevision;
        desiredLights->lightCount = m_pLightManager->GetNumLights();
        desiredLights->lightPagePoolSize = m_pLightManager->GetLightPagePoolSize();
        desiredLights->tableImages = m_pLightManager->CaptureTableImages();
        auto& lightWorld = RendererECSManager::GetInstance().GetWorld();
        auto directionalLights = lightWorld.query_builder<const Components::Light,
            const Components::LightViewInfo>().build();
        directionalLights.each([&](flecs::entity, const Components::Light& light,
            const Components::LightViewInfo& viewInfo) {
            if (!light.lightInfo.shadowCaster || light.type != Components::LightType::Directional) return;
            br::render::PublishedDirectionalShadowLight shadow{};
            DirectX::XMStoreFloat3(&shadow.direction,
                DirectX::XMVector3Normalize(light.lightInfo.dirWorldSpace));
            shadow.viewIDs = viewInfo.viewIDs;
            shadow.unwrappedPageOffsetX.assign(viewInfo.virtualShadowUnwrappedPageOffsetX.begin(),
                viewInfo.virtualShadowUnwrappedPageOffsetX.end());
            shadow.unwrappedPageOffsetY.assign(viewInfo.virtualShadowUnwrappedPageOffsetY.begin(),
                viewInfo.virtualShadowUnwrappedPageOffsetY.end());
            desiredLights->directionalShadows.push_back(std::move(shadow));
        });
        for (const auto& key : m_pLightManager->GetSupportedKeys()) {
            if (auto resource = m_pLightManager->ProvideResource(key))
                desiredLights->retainedResources.push_back(std::move(resource));
        }
        std::vector<br::render::ArtifactRequirement> lightRequirements;
        if (auto uploads = currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr) {
            const std::array<std::uint32_t, 5> strides{
                sizeof(LightInfo), sizeof(std::uint32_t), sizeof(std::uint32_t), sizeof(std::uint32_t),
                sizeof(std::uint32_t) };
            for (std::size_t i = 0; i < desiredLights->tableImages.size() && i < strides.size(); ++i) {
                const auto& image = desiredLights->tableImages[i];
                const auto count = image ? image->size() / strides[i] : 0;
                const auto version = m_lightTableFamilies[i]->PostContentSnapshot(
                    *m_rendererStateRequests, uploads,
                    image ? std::span<const std::byte>(*image) : std::span<const std::byte>{},
                    count, (std::max<std::uint64_t>)(count, 1));
                if (version.revision != 0) lightRequirements.push_back(br::render::Exact(version));
            }
        }
        frameIntents.push_back({
            { br::render::ArtifactKind::LightTable, 0, 0 }, desiredLights->revision,
            std::move(lightRequirements),
            br::render::ArtifactPayload::Make<br::render::LightTableBuildInput>(desiredLights),
            desiredLights->revision });
        }
    }
    auto selectedLights = updateData.publishedRendererState
        ? updateData.publishedRendererState->lights.payload.Get<br::render::PublishedLightTableState>()
        : nullptr;
    if (selectedLights) {
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedLightTableSelection");
    }
    updateData.lightTables = selectedLights;
    renderSnapshot.lightTables = selectedLights;

    // Snapshot active skeleton membership and immutable base-skeleton data into
    // the state graph. Accepted frames can now outlive later manager mutations.
    std::shared_ptr<br::render::PoseStateBuildInput> desiredPoses;
    if (m_pSkeletonManager && m_rendererStateRequests) {
        BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::PoseState");
        const auto poseSourceRevision = (std::max<std::uint64_t>)(
            m_pSkeletonManager->GetActiveInstanceRevision(), 1u);
        if (poseSourceRevision != m_lastPoseSourceRevision) {
        m_lastPoseSourceRevision = poseSourceRevision;
        desiredPoses = std::make_shared<br::render::PoseStateBuildInput>();
        desiredPoses->activeInstanceRevision = poseSourceRevision;
        // The inverse-bind table is the only pose table with a versioned
        // consumer; it is captured from the buffer's journal (no CPU copy, no
        // hash) and posted. The frame-written palettes are graph resources.
        std::vector<br::render::ArtifactRequirement> poseRequirements;
        if (auto uploads = currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr) {
            auto capture = m_pSkeletonManager->CaptureInverseBindGraphState();
            const auto revision = capture.writeSequence;
            const auto version = m_poseTableFamilies[0]->PostCapture(
                *m_rendererStateRequests, uploads, revision, std::move(capture));
            if (version.revision != 0) poseRequirements.push_back(br::render::Exact(version));
        }
        for (const auto& instance : m_pSkeletonManager->GetActiveInstanceViews()) {
            if (!instance.skeleton) continue;
            desiredPoses->activeInstances.push_back({
                .baseSkeleton = instance.skeleton->GetBaseSkeletonShared(),
                .instanceSlot = instance.instanceSlot,
                .transformOffsetMatrices = instance.transformOffsetMatrices,
                .inverseSkinOffsetMatrices = instance.inverseSkinOffsetMatrices,
                .boneCount = instance.boneCount,
            });
        }
        for (const auto& key : m_pSkeletonManager->GetSupportedKeys()) {
            if (auto resource = m_pSkeletonManager->ProvideResource(key))
                desiredPoses->retainedResources.push_back(std::move(resource));
        }
        frameIntents.push_back({
            { br::render::ArtifactKind::PoseState, 0, 0 }, desiredPoses->activeInstanceRevision,
            std::move(poseRequirements),
            br::render::ArtifactPayload::Make<br::render::PoseStateBuildInput>(desiredPoses),
            desiredPoses->activeInstanceRevision });
        }
    }
    if (!frameIntents.empty() && m_rendererStateRequests) {
        m_rendererStateRequests->PostLatestBatch(std::move(frameIntents));
    }
    auto selectedPoses = updateData.publishedRendererState
        ? updateData.publishedRendererState->poses.payload.Get<br::render::PublishedPoseState>()
        : nullptr;
    if (selectedPoses) {
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedPoseSelection");
    }
    updateData.poses = selectedPoses;
    renderSnapshot.poses = std::move(selectedPoses);
    captureFrameInputsScope.reset();
    runCapturedStage("PublishDeferredBackingResizesLate", []() {
        BT_ZONE_SCOPE("Renderer::Update::PublishDeferredBackingResizesLate");
        (void)org::PublishReadyDeferredBackingResizes(false);
    });

    runCapturedStage("FlushUploadPolicies", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::FlushUploadPolicies");
        org::runtime::FlushUploadPolicies();
    });

    runCapturedStage("CommitGpuVisibleSnapshots", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::CommitGpuVisibleSnapshots");
        if (m_pMaterialManager) {
			m_pMaterialManager->ScheduleGpuVisibleSnapshotCommit();
        }
		if (m_pObjectManager) {
			m_pObjectManager->ScheduleDesiredBufferStatePublish();
		}
        if (m_pIndirectCommandBufferManager && m_pObjectManager) {
			m_pIndirectCommandBufferManager->PublishDesiredState(
				m_pObjectManager->DesiredBufferStateRequirement(),
				m_pObjectManager->GetResidentInstanceDrawRecordCount(),
				m_context.publishedRendererState);
        }
    });

    const bool publicationsReady = publishedMaterialState
        && updateData.lightTables && updateData.poses;
    if (!publicationsReady) {
        m_frameInputs.reset();
        basic_telemetry::AddCounter("SARP.FrameInputs.PublicationAdmissionBackpressure");
        basic_telemetry::SetGauge("SARP.FrameInputs.PublicationAdmissionReady", 0);
        return;
    }
    basic_telemetry::SetGauge("SARP.FrameInputs.PublicationAdmissionReady", 1);
    renderSnapshot.uiDrawData = Menu::GetInstance().PrepareDrawData(renderSnapshot);
    auto immutableUpdate = std::make_shared<const UpdateContext>(updateData);
    m_frameInputs = std::make_shared<const br::render::RendererFrameInputs>(
        std::move(immutableUpdate),
        std::make_shared<const RenderContext>(std::move(renderSnapshot)),
        primaryCameraUpload);

    org::UpdateExecutionContext context{};
    context.resolverCaptureContext = std::make_shared<const org::ResolverCaptureContext>(m_context.publishedManifestLease,
        m_context.publishedRendererState ? m_context.publishedRendererState->bindingBundle : nullptr);
    context.frameIndex = m_preparationFrameIndex;
    context.preparationSlot = m_preparationFrameIndex;
    context.frameFenceValue = m_currentFrameFenceValue;
    context.deltaTime = elapsedSeconds;
    context.hostData = m_frameInputs.get();
    context.ownedHostData = m_frameInputs;
    context.beforeCompileFrame = [this]() {
        BT_ZONE_SCOPE("Renderer::Update::TerrainRvtTelemetry");
        MaybeRequestTerrainRvtTelemetry();
        MaybeRequestObjectReyesAtlasTelemetry();
        // Opt-in, bounded camera-buffer stream. At most one capture may be in
        // flight, which preserves request/completion ordering and avoids
        // retaining an unbounded readback tail during shutdown.
        struct CameraReadbackStreamState {
            std::atomic_bool inFlight{ false };
            std::atomic_uint64_t completionSequence{ 0 };
            std::mutex outputMutex;
        };
        static auto cameraReadbackState = std::make_shared<CameraReadbackStreamState>();
        static const std::filesystem::path cameraReadbackPath = [] {
            wchar_t* value = nullptr;
            size_t length = 0;
            _wdupenv_s(&value, &length, L"SARP_CAMERA_READBACK_PATH");
            std::filesystem::path result = value && value[0] ? value : L"";
            std::free(value);
            return result;
        }();
        if (!cameraReadbackPath.empty() && currentRenderGraph && m_frameInputs &&
            !cameraReadbackState->inFlight.exchange(true, std::memory_order_acq_rel)) {
            const auto renderInputs = m_frameInputs->Render();
            const auto views = renderInputs ? renderInputs->viewFamily : nullptr;
            const auto primary = views ? std::find_if(views->views.begin(), views->views.end(),
                [](const auto& view) { return view.primary; }) : decltype(views->views.begin()){};
            const auto cameraResource = m_pViewManager ? m_pViewManager->GetCameraBuffer() : nullptr;
            if (cameraResource && views && primary != views->views.end()) {
                if (auto* service = currentRenderGraph->GetReadbackService()) {
                    const auto requestFrame = m_totalFramesRendered;
                    const auto cameraRevision = m_frameInputs->PrimaryCameraUpload().revision;
                    const auto expected = primary->cameraInfo;
                    try {
                        service->RequestReadbackCaptureAfterGraph(
                            cameraResource.get(), org::RangeSpec{},
                            [state = cameraReadbackState, path = cameraReadbackPath, requestFrame,
                                cameraRevision, expected](org::ReadbackCaptureResult&& result) {
                            CameraInfo camera{};
                            if (result.data.size() >= sizeof(camera))
                                std::memcpy(&camera, result.data.data(), sizeof(camera));
                            const auto expectedPreviousInverse = DirectX::XMMatrixInverse(nullptr, expected.prevView);
                            const auto gpuPreviousInverse = DirectX::XMMatrixInverse(nullptr, camera.prevView);
                            const auto completion = state->completionSequence.fetch_add(1,
                                std::memory_order_relaxed) + 1u;
                            {
                                std::scoped_lock lock(state->outputMutex);
                                std::error_code fileError;
                                if (path.has_parent_path())
                                    std::filesystem::create_directories(path.parent_path(), fileError);
                                const bool writeHeader = !std::filesystem::exists(path, fileError) ||
                                    std::filesystem::file_size(path, fileError) == 0u;
                                std::ofstream output(path, std::ios::app);
                                if (writeHeader) {
                                    output << "completion,request_frame,camera_revision,bytes,cpu_x,cpu_y,cpu_z,cpu_prev_x,cpu_prev_y,cpu_prev_z,gpu_x,gpu_y,gpu_z,gpu_prev_x,gpu_prev_y,gpu_prev_z\n";
                                }
                                output << completion << ',' << requestFrame << ',' << cameraRevision << ','
                                    << result.data.size() << ','
                                    << expected.positionWorldSpace.x << ',' << expected.positionWorldSpace.y << ','
                                    << expected.positionWorldSpace.z << ','
                                    << DirectX::XMVectorGetX(expectedPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetY(expectedPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetZ(expectedPreviousInverse.r[3]) << ','
                                    << camera.positionWorldSpace.x << ',' << camera.positionWorldSpace.y << ','
                                    << camera.positionWorldSpace.z << ','
                                    << DirectX::XMVectorGetX(gpuPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetY(gpuPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetZ(gpuPreviousInverse.r[3]) << '\n';
                            }
                                if (result.data.size() < sizeof(camera) ||
                                    std::memcmp(&camera, &expected, sizeof(camera)) != 0)
                                    basic_telemetry::AddCounter("SARP.Camera.Upload.ReadbackMismatch");
                                state->inFlight.store(false, std::memory_order_release);
                            });
                    } catch (...) {
                        cameraReadbackState->inFlight.store(false, std::memory_order_release);
                        throw;
                    }
                } else {
                    cameraReadbackState->inFlight.store(false, std::memory_order_release);
                }
            } else {
                cameraReadbackState->inFlight.store(false, std::memory_order_release);
            }
        }
        // Opt-in, one-frame GPU work snapshot. Capture producers as well as their
        // indirect consumers so visual failures can be localized without GPU printf.
        static const uint64_t diagnosticCaptureFrame = [] {
            char* value = nullptr;
            size_t length = 0;
            _dupenv_s(&value, &length, "SARP_GPU_WORK_READBACK_FRAME");
            const uint64_t frame = value ? std::strtoull(value, nullptr, 10) : 120u;
            std::free(value);
            return frame;
        }();
        static bool gpuWorkReadbackRequested = false;
        if (!gpuWorkReadbackRequested && m_totalFramesRendered >= diagnosticCaptureFrame && currentRenderGraph) {
            wchar_t* value = nullptr;
            size_t length = 0;
            _wdupenv_s(&value, &length, L"SARP_GPU_WORK_READBACK_DIR");
            if (value && value[0]) {
                const std::filesystem::path directory(value);
                std::filesystem::create_directories(directory);
                if (auto* service = currentRenderGraph->GetReadbackService()) {
                    gpuWorkReadbackRequested = true;
                    const auto captureResource = [&](const char* label, const char* anchor,
                        std::shared_ptr<org::Resource> resource,
                        std::shared_ptr<const br::render::PublishedGpuBufferVersion> version = {}) {
                        if (!resource) return;
                        // DynamicResource is a mutable indirection used by the
                        // frame graph for slot rotation. Capture the backing
                        // resource selected at acceptance time so an async
                        // frame cannot observe a later slot's contents.
                        if (auto* dynamic = dynamic_cast<org::DynamicResource*>(resource.get())) {
                            if (auto backing = dynamic->GetResource()) resource = std::move(backing);
                        }
                        const auto frame = m_totalFramesRendered;
                        const auto published = m_context.publishedRendererState;
                        const auto drawRecordsRevision = published ? published->drawRecords.revision : 0u;
                        const auto indirectRevision = published ? published->indirectWorkloads.revision : 0u;
                        const auto desiredDrawRecordsRevision = m_pObjectManager
                            ? m_pObjectManager->DesiredBufferStateRequirement()
                                .transform([](const auto& requirement) { return requirement.minimumRevision; })
                                .value_or(0u)
                            : 0u;
                        const auto indirectState = published
                            ? published->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>()
                            : nullptr;
                        const auto indirectDrawRecordsRevision = indirectState
                            ? indirectState->drawRecordsRoot.revision : 0u;
                        const auto path = directory / (std::string(label) + ".bin");
                        service->RequestReadbackCapture(anchor, resource.get(), org::RangeSpec{},
                            [path, frame, drawRecordsRevision, indirectRevision,
                                desiredDrawRecordsRevision, indirectDrawRecordsRevision,
                                version = std::move(version)](org::ReadbackCaptureResult&& result) {
                                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                                output.write(reinterpret_cast<const char*>(result.data.data()),
                                    static_cast<std::streamsize>(result.data.size()));
                                auto metadataPath = path;
                                metadataPath += L".meta.txt";
                                std::ofstream metadata(metadataPath);
                                metadata << "frame=" << frame << '\n'
                                    << "draw_records_revision=" << drawRecordsRevision << '\n'
                                    << "desired_draw_records_revision=" << desiredDrawRecordsRevision << '\n'
                                    << "indirect_revision=" << indirectRevision << '\n'
                                    << "indirect_draw_records_revision=" << indirectDrawRecordsRevision << '\n'
                                    << "resource=" << result.desc.resourceId << '\n'
                                    << "format=" << static_cast<uint32_t>(result.format) << '\n'
                                    << "width=" << result.width << '\n'
                                    << "height=" << result.height << '\n'
                                    << "bytes=" << result.data.size() << '\n';
                                if (version) metadata
                                    << "buffer_revision=" << version->revision << '\n'
                                    << "write_sequence=" << version->writeSequence << '\n'
                                    << "element_count=" << version->elementCount << '\n'
                                    << "capacity=" << version->capacity << '\n'
                                    << "element_stride=" << version->elementStride << '\n'
                                    << "content_version=" << version->contentVersion << '\n'
                                    << "content_epoch=" << version->contentEpoch << '\n'
                                    << "backing_epoch=" << version->backingEpoch << '\n'
                                    << "backing_generation=" << (version->backing
                                        ? version->backing->backingGeneration : 0u) << '\n';
                                if (!result.layouts.empty()) metadata
                                    << "offset=" << result.layouts.front().offset << '\n'
                                    << "row_pitch=" << result.layouts.front().rowPitch << '\n';
                            });
                    };
                    const auto capture = [&](const char* label, const char* anchor, org::ResourceIdentifier id) {
                        captureResource(label, anchor, currentRenderGraph->RequestResourcePtr(id, true));
                    };
                    // Preserve the visibility target at the two phase-1 producer
                    // boundaries as well as at its first material consumer.  The
                    // fixed benchmark camera expects phase 1 to cover the scene;
                    // shader invocation counts alone did not catch descriptor-table
                    // mismatches that discarded almost every raster write.
                    capture("visibility-after-phase1-hw", "CLodOpaque::RasterizeClustersPass1",
                        Builtin::PrimaryCamera::VisibilityTexture);
                    capture("visibility-after-phase1-sw", "CLodOpaque::SoftwareRasterizeClustersPass1",
                        Builtin::PrimaryCamera::VisibilityTexture);
                    capture("visibility", "MaterialHistogramPass", Builtin::PrimaryCamera::VisibilityTexture);
                    capture("material-counts", "BuildMaterialIndirectCommandBufferPass", "Builtin::VisUtil::MaterialPixelCountBuffer");
                    capture("material-offsets", "BuildMaterialIndirectCommandBufferPass", "Builtin::VisUtil::MaterialOffsetBuffer");
                    capture("material-args", "BuildMaterialIndirectCommandBufferPass", "Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
                    capture("pixel-list", "BuildPixelListPass", "Builtin::VisUtil::PixelListBuffer");
                    capture("surface-identity", "EvaluateMaterialGroupsPass", Builtin::Surface::Identity);
                    // Publication audit: retain the exact object buffer backing
                    // selected for this graph generation alongside visibility.
                    // Its byte count and resource id are written by the
                    // readback service, allowing CPU publication cuts to be
                    // compared with the GPU-consumed object data.
                    if (const auto published = m_context.publishedRendererState) {
                        if (const auto objects = published->drawRecords.payload
                                .Get<br::render::PublishedObjectBufferState>()) {
                            const auto capturePublishedObjectVersion = [&](const char* label,
                                std::uint64_t variant) {
                                const auto version = objects->FindVersion(variant);
                                captureResource(label, "CLodOpaque::RasterizeClustersPass1",
                                    version ? version->resource : nullptr, version);
                            };
                            // Capture the immutable resources selected by the
                            // accepted manifest.  The manager-facing logical
                            // resources can point at a newer backing by the
                            // time this async frame reaches the readback pass.
                            capturePublishedObjectVersion("published-per-object",
                                br::render::kObjectPerObjectVariant);
                            capturePublishedObjectVersion("published-instance-transforms",
                                br::render::kObjectInstanceTransformVariant);
                            capturePublishedObjectVersion("published-draw-records",
                                br::render::kObjectDrawRecordVariant);
                        }
                        if (const auto indirect = published->indirectWorkloads.payload
                                .Get<br::render::PublishedIndirectState>()) {
                            captureResource("published-visibility-generations",
                                "CLodOpaque::RasterizeClustersPass1", indirect->visibilityGenerations);
                            std::unordered_set<std::uint64_t> capturedActiveResources;
                            std::unordered_set<std::uint64_t> capturedArgumentResources;
                            for (std::size_t workloadIndex = 0;
                                workloadIndex < indirect->workloads.size(); ++workloadIndex) {
                                const auto& workload = indirect->workloads[workloadIndex];
                                // Multiple views commonly retain the same
                                // immutable buffer version.  Read each backing
                                // once; its label still records the retained
                                // revision and advertised element count.
                                const auto label = std::format("published-active-draw-{}-rev-{}-count-{}",
                                    workloadIndex, workload.activeListRevision, workload.count);
                                if (workload.activeDrawList && capturedActiveResources.insert(
                                        workload.activeDrawList->GetGlobalResourceID()).second) {
                                    captureResource(label.c_str(), "CLodOpaque::RasterizeClustersPass1",
                                        workload.activeDrawList);
                                }
                                const auto argsLabel = std::format(
                                    "published-indirect-args-{}-active-rev-{}-count-{}",
                                    workloadIndex, workload.activeListRevision, workload.count);
                                if (workload.indirectArguments && capturedArgumentResources.insert(
                                        workload.indirectArguments->GetGlobalResourceID()).second) {
                                    captureResource(argsLabel.c_str(), "CLodOpaque::RasterizeClustersPass1",
                                        workload.indirectArguments);
                                }
                            }
                        }
                    }
                }
            }
            std::free(value);
        }
        static bool colorOutputReadbackRequested = false;
        if (!colorOutputReadbackRequested && m_totalFramesRendered >= diagnosticCaptureFrame &&
            currentRenderGraph && m_dynamicPresentationColor) {
            wchar_t* outputPath = nullptr;
            size_t outputPathLength = 0;
            if (_wdupenv_s(
                    &outputPath,
                    &outputPathLength,
                    L"SARP_COLOR_OUTPUT_READBACK_PATH") == 0 &&
                outputPath != nullptr && outputPath[0] != L'\0') {
                const std::filesystem::path path(outputPath);
                std::free(outputPath);
                outputPath = nullptr;
                if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                    colorOutputReadbackRequested = true;
                    readbackService->RequestReadbackCaptureAfterGraph(
                        m_dynamicPresentationColor.get(),
                        org::RangeSpec{},
                        [path](org::ReadbackCaptureResult&& result) {
                            BT_ZONE_SCOPE("Renderer::ColorOutputReadback::Analyze");
                            std::ofstream output(path, std::ios::binary | std::ios::trunc);
                            if (output && !result.data.empty()) {
                                output.write(
                                    reinterpret_cast<const char*>(result.data.data()),
                                    static_cast<std::streamsize>(result.data.size()));
                            }

                            std::uint64_t whitePixels = 0;
                            std::uint64_t opaquePixels = 0;
                            std::uint64_t allWhiteTiles = 0;
                            constexpr std::uint32_t tileSize = 16;
                            if (!result.layouts.empty() && result.width && result.height) {
                                const auto& layout = result.layouts.front();
                                const auto rowPitch = static_cast<std::size_t>(layout.rowPitch);
                                const auto baseOffset = static_cast<std::size_t>(layout.offset);
                                if (rowPitch >= static_cast<std::size_t>(result.width) * 4u &&
                                    baseOffset + rowPitch * result.height <= result.data.size()) {
                                    for (std::uint32_t y = 0; y < result.height; ++y) {
                                        const auto* row = result.data.data() + baseOffset + rowPitch * y;
                                        for (std::uint32_t x = 0; x < result.width; ++x) {
                                            const auto* pixel = row + static_cast<std::size_t>(x) * 4u;
                                            const bool white = std::to_integer<std::uint8_t>(pixel[0]) >= 250u &&
                                                std::to_integer<std::uint8_t>(pixel[1]) >= 250u &&
                                                std::to_integer<std::uint8_t>(pixel[2]) >= 250u;
                                            whitePixels += white ? 1u : 0u;
                                            opaquePixels += std::to_integer<std::uint8_t>(pixel[3]) >= 250u ? 1u : 0u;
                                        }
                                    }
                                    for (std::uint32_t y = 0; y + tileSize <= result.height; y += tileSize) {
                                        for (std::uint32_t x = 0; x + tileSize <= result.width; x += tileSize) {
                                            bool allWhite = true;
                                            for (std::uint32_t tileY = 0; tileY < tileSize && allWhite; ++tileY) {
                                                const auto* row = result.data.data() + baseOffset +
                                                    rowPitch * (y + tileY) + static_cast<std::size_t>(x) * 4u;
                                                for (std::uint32_t tileX = 0; tileX < tileSize; ++tileX) {
                                                    const auto* pixel = row + static_cast<std::size_t>(tileX) * 4u;
                                                    if (std::to_integer<std::uint8_t>(pixel[0]) < 250u ||
                                                        std::to_integer<std::uint8_t>(pixel[1]) < 250u ||
                                                        std::to_integer<std::uint8_t>(pixel[2]) < 250u) {
                                                        allWhite = false;
                                                        break;
                                                    }
                                                }
                                            }
                                            allWhiteTiles += allWhite ? 1u : 0u;
                                        }
                                    }
                                }
                            }
                            BT_PLOT("Renderer.ColorOutputReadback.WhitePixels", static_cast<int64_t>(whitePixels));
                            BT_PLOT("Renderer.ColorOutputReadback.AllWhite16x16Tiles", static_cast<int64_t>(allWhiteTiles));
                            BT_PLOT("Renderer.ColorOutputReadback.Bytes", static_cast<int64_t>(result.data.size()));

                            auto metadataPath = path;
                            metadataPath += L".meta.txt";
                            std::ofstream metadata(metadataPath, std::ios::trunc);
                            if (metadata) {
                                metadata << "format=" << static_cast<std::uint32_t>(result.format) << '\n';
                                metadata << "width=" << result.width << '\n';
                                metadata << "height=" << result.height << '\n';
                                metadata << "bytes=" << result.data.size() << '\n';
                                metadata << "white_pixels=" << whitePixels << '\n';
                                metadata << "opaque_pixels=" << opaquePixels << '\n';
                                metadata << "all_white_16x16_tiles=" << allWhiteTiles << '\n';
                                if (!result.layouts.empty()) {
                                    metadata << "offset=" << result.layouts.front().offset << '\n';
                                    metadata << "row_pitch=" << result.layouts.front().rowPitch << '\n';
                                }
                            }
                            spdlog::info(
                                "Color output readback: bytes={} dimensions={}x{} white_pixels={} all_white_16x16_tiles={} output='{}'.",
                                result.data.size(), result.width, result.height, whitePixels, allWhiteTiles,
                                path.string());
                        });
                }
            }
            if (outputPath) {
                std::free(outputPath);
            }
        }
        static bool materialBufferReadbackRequested = false;
        if (!materialBufferReadbackRequested && m_totalFramesRendered >= 120u &&
            currentRenderGraph && m_pMaterialManager) {
            wchar_t* outputPath = nullptr;
            size_t outputPathLength = 0;
            if (_wdupenv_s(
                    &outputPath,
                    &outputPathLength,
                    L"SARP_MATERIAL_BUFFER_READBACK_PATH") == 0 &&
                outputPath != nullptr && outputPath[0] != L'\0') {
                const std::filesystem::path path(outputPath);
                std::free(outputPath);
                outputPath = nullptr;
                if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                    const auto lease = m_context.publishedManifestLease;
                    const auto published = lease ? lease->state : nullptr;
                    const auto materialState = published
                        ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
                    if (materialState && materialState->baseTable && materialState->evalTable &&
                        materialState->openPbrTable) {
                        auto metadataPath = path;
                        metadataPath += L".meta.txt";
                        std::ofstream metadata(metadataPath, std::ios::trunc);
                        if (metadata) {
                            metadata << "epoch=" << published->epoch << '\n';
                            metadata << "lease_sequence=" << lease->sequence << '\n';
                            metadata << "lease_frame_slot=" << lease->frameSlot << '\n';
                            metadata << "revision=" << published->materials.revision << '\n';
                            metadata << "compile_flag_slots=" << materialState->compileFlagSlotsUsed << '\n';
                            metadata << "active_compile_flags=" << materialState->activeCompileFlags.size() << '\n';
                            for (std::size_t index = 0;
                                index < materialState->activeCompileFlags.size() &&
                                index < materialState->activeCompileFlagSlots.size(); ++index) {
                                metadata << "active[" << index << "].flags="
                                    << static_cast<std::uint64_t>(materialState->activeCompileFlags[index])
                                    << " slot=" << materialState->activeCompileFlagSlots[index] << '\n';
                            }
                        }
                        const auto requestTable = [readbackService, &path, published](
                            const char* label,
                            const std::shared_ptr<const br::render::PublishedGpuBufferVersion>& table,
                            org::Resource* captureResource = nullptr) {
                            if (!table || !table->resource || !table->image) return;
                            auto tablePath = path;
                            tablePath += std::filesystem::path(fmt::format(".{}.bin", label));
                            const auto expected = table->MaterializeCpuImage();
							auto expectedPath = tablePath;
							expectedPath += L".expected";
							std::ofstream expectedOutput(expectedPath, std::ios::binary | std::ios::trunc);
							if (expectedOutput && !expected->empty()) {
								expectedOutput.write(reinterpret_cast<const char*>(expected->data()),
									static_cast<std::streamsize>(expected->size()));
							}
                            const auto resourceID = table->resource->GetGlobalResourceID();
                            const br::render::ArtifactSnapshot* tableArtifact = nullptr;
                            for (const auto& dependency : published->materials.dependencyClosure) {
                                const auto root = dependency.payload
                                    .Get<br::render::RendererStateFragmentArtifact>();
                                const auto dependencyTable = root
                                    ? root->fragment.payload
                                        .Get<br::render::PublishedGpuBufferVersion>() : nullptr;
                                if (dependencyTable == table) {
                                    tableArtifact = &dependency;
                                    break;
                                }
                            }
                            auto tableMetadataPath = tablePath;
                            tableMetadataPath += L".meta.txt";
                            std::ofstream tableMetadata(tableMetadataPath, std::ios::trunc);
                            if (tableMetadata) {
                                tableMetadata << "manifest_epoch=" << published->epoch << '\n';
                                tableMetadata << "manifest_material_revision="
                                    << published->materials.revision << '\n';
                                tableMetadata << "manifest_material_root_kind="
                                    << static_cast<std::uint32_t>(
                                        published->materials.publicationRoot.address.kind) << '\n';
                                tableMetadata << "manifest_material_root_primary="
                                    << published->materials.publicationRoot.address.primaryID << '\n';
                                tableMetadata << "manifest_material_root_variant="
                                    << published->materials.publicationRoot.address.variantID << '\n';
                                tableMetadata << "buffer_revision=" << table->revision << '\n';
                                tableMetadata << "buffer_write_sequence="
                                    << table->writeSequence << '\n';
                                tableMetadata << "buffer_content_version="
                                    << table->contentVersion << '\n';
                                tableMetadata << "buffer_backing_generation="
                                    << (table->backing ? table->backing->backingGeneration : 0u) << '\n';
                                tableMetadata << "resource_id=" << resourceID << '\n';
                                tableMetadata << "capture_resource_id="
                                    << (captureResource
                                        ? captureResource->GetGlobalResourceID() : resourceID) << '\n';
                                tableMetadata << "capture_matches_published="
                                    << (!captureResource || captureResource == table->resource.get()) << '\n';
                                if (tableArtifact) {
                                    tableMetadata << "artifact_kind="
                                        << static_cast<std::uint32_t>(tableArtifact->key.kind) << '\n';
                                    tableMetadata << "artifact_primary="
                                        << tableArtifact->key.primaryID << '\n';
                                    tableMetadata << "artifact_variant="
                                        << tableArtifact->key.variantID << '\n';
                                    tableMetadata << "artifact_revision="
                                        << tableArtifact->revision << '\n';
                                    tableMetadata << "artifact_generation="
                                        << tableArtifact->generation << '\n';
                                    tableMetadata << "artifact_readiness="
                                        << static_cast<std::uint32_t>(
                                            tableArtifact->readiness) << '\n';
                                }
                            }
                            readbackService->RequestReadbackCapture(
                                "EvaluateMaterialGroupsPass",
                                captureResource ? captureResource : table->resource.get(), org::RangeSpec{},
                                [tablePath, expected, resourceID, label](org::ReadbackCaptureResult&& result) {
                                    std::ofstream output(tablePath, std::ios::binary | std::ios::trunc);
                                    if (output && !result.data.empty()) {
                                        output.write(reinterpret_cast<const char*>(result.data.data()),
                                            static_cast<std::streamsize>(result.data.size()));
                                    }
                                    const auto comparedBytes = (std::min)(result.data.size(), expected->size());
                                    std::size_t firstMismatch = comparedBytes;
                                    for (std::size_t offset = 0; offset < comparedBytes; ++offset) {
                                        if (result.data[offset] != (*expected)[offset]) {
                                            firstMismatch = offset;
                                            break;
                                        }
                                    }
                                    const bool exactPrefix = result.data.size() >= expected->size() &&
                                        firstMismatch == comparedBytes;
                                    spdlog::info(
                                        "Published material GPU readback: table={} resource={} gpuBytes={} expectedBytes={} exactPrefix={} firstMismatch={} output='{}'.",
                                        label, resourceID, result.data.size(), expected->size(), exactPrefix,
                                        firstMismatch == comparedBytes ? UINT64_MAX : firstMismatch,
                                        tablePath.string());
                                });
                        };
                        materialBufferReadbackRequested = true;
                        requestTable("base", materialState->baseTable);
                        requestTable("eval", materialState->evalTable);
                        requestTable("openpbr", materialState->openPbrTable);
                        const auto textureImages = published->textureImages.payload
                            .Get<br::render::PublishedTextureImageTable>();
                        if (textureImages && textureImages->table) {
                            const auto* textureStreaming = m_pMaterialManager
                                ? m_pMaterialManager->GetTextureStreamingManager() : nullptr;
                            const auto boundTextureTable = textureStreaming
                                ? textureStreaming->ResolvePublishedImageTableResourceForDiagnostics()
                                : std::shared_ptr<org::Resource>{};
							const auto readbackAnchor = textureStreaming
								? textureStreaming->PublishedImageTableReadbackAnchorForDiagnostics()
								: std::shared_ptr<org::Resource>{};
                            // Request the resolver-selected published backing directly.  The
                            // logical bootstrap resource is only the resolver fallback; passing
                            // it to the readback service captures that stale backing rather than
                            // the resource whose descriptor is selected for this pass.
                            requestTable("texture-images", textureImages->table,
								boundTextureTable ? boundTextureTable.get() :
									(readbackAnchor ? readbackAnchor.get() : nullptr));
                        }

                        const auto requestTexture = [readbackService, &path](
                            const char* label, const std::shared_ptr<org::Resource>& texture) {
                            if (!texture) return;
                            auto texturePath = path;
                            texturePath += std::filesystem::path(fmt::format(".{}.bin", label));
                            const auto resourceID = texture->GetGlobalResourceID();
                            readbackService->RequestReadbackCapture(
                                "EvaluateMaterialGroupsPass", texture.get(), org::RangeSpec{},
                                [texturePath, resourceID, label](org::ReadbackCaptureResult&& result) {
                                    std::ofstream output(texturePath, std::ios::binary | std::ios::trunc);
                                    if (output && !result.data.empty()) {
                                        output.write(reinterpret_cast<const char*>(result.data.data()),
                                            static_cast<std::streamsize>(result.data.size()));
                                    }
                                    auto textureMetadataPath = texturePath;
                                    textureMetadataPath += L".meta.txt";
                                    std::ofstream textureMetadata(textureMetadataPath, std::ios::trunc);
                                    if (textureMetadata) {
                                        textureMetadata << "resource_id=" << resourceID << '\n';
                                        textureMetadata << "format="
                                            << static_cast<std::uint32_t>(result.format) << '\n';
                                        textureMetadata << "width=" << result.width << '\n';
                                        textureMetadata << "height=" << result.height << '\n';
                                        textureMetadata << "depth=" << result.depth << '\n';
                                        textureMetadata << "bytes=" << result.data.size() << '\n';
                                        textureMetadata << "layouts=" << result.layouts.size() << '\n';
                                        for (std::size_t index = 0; index < result.layouts.size(); ++index) {
                                            const auto& layout = result.layouts[index];
                                            textureMetadata << "layout[" << index << "].offset="
                                                << layout.offset << '\n';
                                            textureMetadata << "layout[" << index << "].row_pitch="
                                                << layout.rowPitch << '\n';
                                        }
                                    }
                                    spdlog::info(
                                        "Published material pixel readback: texture={} resource={} bytes={} dimensions={}x{} output='{}'.",
                                        label, resourceID, result.data.size(), result.width, result.height,
                                        texturePath.string());
                                });
                            auto requestMetadataPath = texturePath;
                            requestMetadataPath += L".request.txt";
                            std::ofstream requestMetadata(requestMetadataPath, std::ios::trunc);
                            if (requestMetadata) {
                                requestMetadata << "resource_id=" << resourceID << '\n';
                                requestMetadata << "requested=1\n";
                            }
                        };
                        requestTexture("hdr", m_coreResourceProvider.m_HDRColorTarget);
                        if (const auto* primaryView = m_pViewManager
                                ? m_pViewManager->Get(m_context.primaryViewID) : nullptr) {
                            requestTexture("visibility", primaryView->gpu.visibilityBuffer);
                        }
                        requestTexture("surface-base-color", currentRenderGraph->RequestResourcePtr(
                            Builtin::Surface::BaseColorOpacity, true));
                        requestTexture("surface-records", currentRenderGraph->RequestResourcePtr(
                            Builtin::Surface::Records, true));
                    }
                }
            }
            std::free(outputPath);
        }

    };

    auto& deviceManager = DeviceManager::GetInstance();

    runCapturedStage("RenderGraphUpdate", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::RenderGraphUpdate");
        // Dynamic wrapper selection is part of serialized preparation state.
        // Do not mutate it while the preceding preparation owner may still be
        // freezing that frame's concrete resource table.
        currentRenderGraph->WaitForPreparation();
        if (m_dynamicPresentationColor &&
            m_preparationFrameIndex < m_presentationColorResources.size()) {
            m_dynamicPresentationColor->SetResource(
                m_presentationColorResources[m_preparationFrameIndex]);
        }
        currentRenderGraph->Update(context, deviceManager.GetDevice());
    });
    if (asyncFrameQueue) {
        m_preparationFrameIndex = static_cast<uint8_t>(
            (m_preparationFrameIndex + 1) % m_numFramesInFlight);
        BT_PLOT("ORG.AsyncExecution.NextPreparationSlot",
            static_cast<int64_t>(m_preparationFrameIndex));
    }
    ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "after RenderGraphUpdate");

    // Clear transform-update tags only after render-graph update so passes such as
    // virtual shadow invalidation can still consume same-frame movement signals.
    // Renderables remain dirty for one additional frame. That second upload copies
    // the current model into prevModel, preventing a one-frame transform change
    // from producing motion vectors indefinitely while the object is static.
    world.defer_begin();
    m_renderTransformUpdatedCleanupQuery.each([](flecs::entity e) {
        if (!e.has<Components::RenderableObject>()) {
            e.remove<Components::RenderTransformUpdated>();
        } else if (e.has<Components::RenderTransformNeedsConvergence>()) {
            e.remove<Components::RenderTransformNeedsConvergence>();
            e.remove<Components::RenderTransformUpdated>();
        } else {
            e.add<Components::RenderTransformNeedsConvergence>();
        }
    });
    world.defer_end();

    if (!m_externalSceneMode) {
        runCapturedStage("ScheduleSceneUpdate", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::ScheduleSceneUpdate");
            ScheduleSceneUpdateTask(elapsedSeconds);
        });
    }

    runCapturedStage("BeginUploadPolicyFrame", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::BeginUploadPolicyFrame");
        org::runtime::BeginUploadPolicyFrame();
    });

    auto graphicsQueue = deviceManager.GetGraphicsQueue();
    auto computeQueue = deviceManager.GetComputeQueue();
    runCapturedStage("FrameMaintenance", [&]() {
        if (currentRenderGraph) {
            BT_ZONE_SCOPE("Renderer::Update::FrameStatistics");
            if (auto* statisticsService = currentRenderGraph->GetStatisticsService()) {
                statisticsService->OnFrameComplete(m_frameIndex, computeQueue); // Gather statistics for the last iteration of the frame
                statisticsService->OnFrameComplete(m_frameIndex, graphicsQueue); // Gather statistics for the last iteration of the frame
            }
        }
        });
    ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "after FrameMaintenance");
}
