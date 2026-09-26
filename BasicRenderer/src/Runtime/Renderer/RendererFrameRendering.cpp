#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include <BasicRenderer/Extensions/RenderContext.h>
#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <stacktrace>
#include <sstream>
#include <thread>
#include <iomanip>
#include <typeinfo>
#include <chrono>
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Streaming/TaskScheduler.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Resources/ExternalTextureResource.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Runtime/Frame/RendererFrameInputs.h"
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include "Scene/ECS/RendererECSManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Materials/MaterialManager.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "Utilities/Utilities.h"
#include "OpenRenderGraph/OpenRenderGraph.h"

namespace {
std::string RendererExceptionStacktraceString()
{
#if defined(__cpp_lib_stacktrace) && (__cpp_lib_stacktrace >= 202011L)
    try {
        std::ostringstream output;
        output << std::stacktrace::current();
        return output.str();
    } catch (...) {
        return "(stacktrace capture failed)";
    }
#else
    return "(no <stacktrace> support in this build)";
#endif
}

std::filesystem::path MakeRendererExceptionPath(uint64_t frameNumber, const char* stageName)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    (void)localtime_s(std::addressof(localTime), std::addressof(time));

    std::ostringstream name;
    name << "RendererException-"
         << std::put_time(std::addressof(localTime), "%Y%m%d-%H%M%S")
         << "-frame" << frameNumber
         << "-" << stageName
         << "-tid" << GetCurrentThreadId()
         << ".txt";

    std::error_code ec;
    std::filesystem::create_directories("crashes", ec);
    return std::filesystem::current_path() / "crashes" / name.str();
}

void WriteRendererExceptionNote(
    const char* stageName,
    uint64_t frameNumber,
    uint8_t frameIndex,
    uint64_t frameFenceValue,
    const std::exception& ex)
{
    const auto path = MakeRendererExceptionPath(frameNumber, stageName);
    std::ofstream report(path, std::ios::trunc);
    if (!report) {
        return;
    }

    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    report << "Renderer exception note\n";
    report << "stage='" << stageName << "'\n";
    report << "frame=" << frameNumber << "\n";
    report << "frame_index=" << static_cast<unsigned>(frameIndex) << "\n";
    report << "frame_fence_value=" << frameFenceValue << "\n";
    report << "thread_id=" << threadId.str() << "\n";
    report << "win32_thread_id=" << GetCurrentThreadId() << "\n";
    report << "exception_type='" << typeid(ex).name() << "'\n";
    report << "exception_what='" << ex.what() << "'\n\n";
    report << "Catch-site stacktrace:\n" << RendererExceptionStacktraceString() << "\n";
}
}

namespace {
}

void SyncOpenRenderGraphSettings(uint8_t numFramesInFlight) {
    auto& sm = SettingsManager::GetInstance();
    org::runtime::OpenRenderGraphSettings orgSettings{};
    orgSettings.numFramesInFlight = numFramesInFlight;
    orgSettings.collectPassStatistics = sm.getSettingGetter<bool>("collectPassStatistics")();
    orgSettings.collectPipelineStatistics = sm.getSettingGetter<bool>("collectPipelineStatistics")();
    orgSettings.useAsyncCompute = sm.getSettingGetter<bool>("useAsyncCompute")();
    const auto requestedCompileMode = sm.getSettingGetter<int>("experimentalAsyncCompileMode")();
    orgSettings.experimentalAsyncCompileMode = requestedCompileMode == 2
        ? org::runtime::AsyncCompileMode::Async : org::runtime::AsyncCompileMode::Off;
    orgSettings.experimentalCompileConcurrency = static_cast<uint8_t>(std::clamp(
        sm.getSettingGetter<int>("experimentalCompileConcurrency")(), 1, 4));
    orgSettings.renderGraphCompileDumpEnabled = sm.getSettingGetter<bool>("renderGraphCompileDumpEnabled")();
    orgSettings.renderGraphVramDumpEnabled = sm.getSettingGetter<bool>("renderGraphVramDumpEnabled")();
    orgSettings.renderGraphBatchTraceEnabled = sm.getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    orgSettings.renderGraphLightweightCompileSummaryEnabled = sm.getSettingGetter<bool>("renderGraphLightweightCompileSummaryEnabled")();
    orgSettings.readOnlyUniformTransitionElisionEnabled = true;
    orgSettings.autoAliasMode = static_cast<uint8_t>(sm.getSettingGetter<org::AutoAliasMode>("autoAliasMode")());
    orgSettings.autoAliasPackingStrategy = static_cast<uint8_t>(sm.getSettingGetter<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy")());
    orgSettings.autoAliasEnableLogging = sm.getSettingGetter<bool>("autoAliasEnableLogging")();
    orgSettings.autoAliasLogExclusionReasons = sm.getSettingGetter<bool>("autoAliasLogExclusionReasons")();
    orgSettings.autoAliasBuildDebugData = sm.getSettingGetter<bool>("autoAliasBuildDebugData")();
    orgSettings.queueSchedulingEnableLogging = sm.getSettingGetter<bool>("queueSchedulingEnableLogging")();
    orgSettings.queueSchedulingSelectionPolicy = static_cast<org::runtime::QueueSchedulingSelectionPolicy>(sm.getSettingGetter<uint8_t>("queueSchedulingSelectionPolicy")());
    orgSettings.queueSchedulingWidthScale = sm.getSettingGetter<float>("queueSchedulingWidthScale")();
    orgSettings.queueSchedulingPenaltyBias = sm.getSettingGetter<float>("queueSchedulingPenaltyBias")();
    orgSettings.queueSchedulingMinPenalty = sm.getSettingGetter<float>("queueSchedulingMinPenalty")();
    orgSettings.queueSchedulingResourcePressureWeight = sm.getSettingGetter<float>("queueSchedulingResourcePressureWeight")();
    orgSettings.queueSchedulingUavPressureWeight = sm.getSettingGetter<float>("queueSchedulingUavPressureWeight")();
    orgSettings.queueSchedulingAutoGraphicsBias = sm.getSettingGetter<float>("queueSchedulingAutoGraphicsBias")();
    orgSettings.queueSchedulingAsyncOverlapBonus = sm.getSettingGetter<float>("queueSchedulingAsyncOverlapBonus")();
    orgSettings.queueSchedulingCrossQueueHandoffPenalty = sm.getSettingGetter<float>("queueSchedulingCrossQueueHandoffPenalty")();
    orgSettings.autoAliasPoolRetireIdleFrames = sm.getSettingGetter<uint32_t>("autoAliasPoolRetireIdleFrames")();
    orgSettings.autoAliasPoolGrowthHeadroom = sm.getSettingGetter<float>("autoAliasPoolGrowthHeadroom")();
    orgSettings.transitionPlacementMode = static_cast<org::runtime::TransitionPlacementMode>(sm.getSettingGetter<uint8_t>("transitionPlacementMode")());
    orgSettings.heavyDebug = sm.getSettingGetter<bool>("heavyDebug")();
    org::runtime::SetOpenRenderGraphSettings(orgSettings);
}



void Renderer::Render() {
    BT_ZONE_SCOPE("Renderer::Render");

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        stageFn();
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };

    auto deltaTime = m_frameTimer.tick();
    if (m_deterministicSamplingMode) {
        deltaTime = 0.0f;
    }
    if (!IsSceneReadyForFrame()) {
        return;
    }

    if (!currentRenderGraph) {
        return;
    }

    // Update may intentionally apply publication backpressure while the first
    // complete immutable manifest is being produced.
    if (!m_frameInputs) {
        return;
    }

    // Async mode runs graph update/declaration/preparation on its serialized
    // owner. Transitional render-context assembly still reads a few renderer
    // services, so join before those reads until they are publication-only.
    currentRenderGraph->WaitForPreparation();

    if (!m_swapChainReady) {
        if (!m_loggedSwapChainNotReady) {
            spdlog::critical(
                "Renderer: skipping render because swapchain/backbuffers are not ready frame={} frameIndex={}",
                m_totalFramesRendered,
                static_cast<unsigned>(m_frameIndex));
            m_loggedSwapChainNotReady = true;
        }
        return;
    }

    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();

    // Vulkan does not guarantee round-robin swapchain acquisition.  Re-read the
    // acquired image at the last responsible point and bind the graph's dynamic
    // backbuffer to that exact image.  Using the CPU frame slot here can record
    // transitions for a presentable image that was not acquired.
    const uint8_t renderedFrameIndex = m_swapChain
        ? static_cast<uint8_t>(m_swapChain->CurrentImageIndex())
        : m_frameIndex;
    if (renderedFrameIndex != m_frameIndex) {
        spdlog::warn(
            "Renderer: acquired swapchain image changed before render (frame slot={} acquired={}); resynchronizing",
            static_cast<unsigned>(m_frameIndex),
            static_cast<unsigned>(renderedFrameIndex));
        m_frameIndex = renderedFrameIndex;
    }
    if (m_dynamicBackbuffer && renderedFrameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[renderedFrameIndex]);
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
	const Components::DrawStats& drawStats = world.get<Components::DrawStats>();
    auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    auto outputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();

    auto& deviceManager = DeviceManager::GetInstance();

    {
        BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext");
        runCapturedStage("PrepareRenderContext", [&]() {
			if (!currentRenderGraph || !currentRenderGraph->GetDescriptorService()) {
				throw std::runtime_error("Renderer: descriptor service unavailable while preparing render context");
			}
			auto* descriptorService = currentRenderGraph->GetDescriptorService();
            m_context.hasPrimaryCamera = false;
            m_context.primaryViewID = 0;
			m_context.textureDescriptorHeap = descriptorService->GetSRVDescriptorHeap();
			m_context.samplerDescriptorHeap = descriptorService->GetSamplerDescriptorHeap();
            m_context.rtvHeap = rtvHeap.Get();
            m_context.rtvDescriptorSize = rtvDescriptorSize;
            m_context.dsvDescriptorSize = dsvDescriptorSize;
            m_context.swapchainImageIndex = renderedFrameIndex;
            m_context.frameIndex = renderedFrameIndex;
            m_context.frameNumber = m_totalFramesRendered;
            m_context.frameFenceValue = m_currentFrameFenceValue;
            m_context.renderResolution = { renderRes.x, renderRes.y };
            m_context.outputResolution = { outputRes.x, outputRes.y };
            m_context.clodRayTracingSupported = deviceManager.GetCLodRayTracingSupported();
            m_context.rayTracedReflectionsEnabled = m_rayTracedReflections && m_context.clodRayTracingSupported;
            m_context.materialTextureStreamingStats = m_pMaterialManager
                ? m_pMaterialManager->GetMaterialTextureStreamingStats()
                : MaterialTextureStreamingStats{};
            m_context.environmentWork = m_environmentWorkServices;
            m_context.clodRayTracingSystem = m_clodRayTracingSystem;

            if (m_context.rayTracedReflectionsEnabled && m_clodRayTracingSystem && m_pMeshManager) {
                std::scoped_lock rayTracingLock(m_clodRayTracingSystem->FrameOperationMutex());
                m_clodRayTracingSystem->Refresh(*m_pMeshManager);
                m_clodRayTracingSystem->UpdateGpuResources(deviceManager.GetDevice(), deviceManager.GetRayTracingFeatures());
            }
            else if (m_clodRayTracingSystem) {
                std::scoped_lock rayTracingLock(m_clodRayTracingSystem->FrameOperationMutex());
                m_clodRayTracingSystem->Reset();
            }
            m_context.drawStats = drawStats;
            m_context.deltaTime = deltaTime;
            m_context.sceneOverlapStatus = GetSceneOverlapStatus();

            auto primaryCamera = GetValidatedPrimaryRenderCamera(false);
            if (primaryCamera) {
                m_context.hasPrimaryCamera = true;
                m_context.primaryViewID = primaryCamera.get<Components::RenderViewRef>().viewID;
                m_context.primaryCamera = primaryCamera.get<Components::Camera>();
                if (auto depthMap = primaryCamera.try_get<Components::DepthMap>()) {
                    m_context.primaryDepthMap = *depthMap;
                }
            }

            unsigned int globalPSOFlags = 0;
            if (m_imageBasedLighting) {
                globalPSOFlags |= PSOFlags::PSO_IMAGE_BASED_LIGHTING;
            }
            if (m_clusteredLighting) {
                globalPSOFlags |= PSOFlags::PSO_CLUSTERED_LIGHTING;
            }
            if (m_screenSpaceReflections || m_context.rayTracedReflectionsEnabled) {
                globalPSOFlags |= PSOFlags::PSO_SCREENSPACE_REFLECTIONS;
            }
            m_context.globalPSOFlags = globalPSOFlags;
        });

    }

    org::PassExecutionContext passExecutionContext{};
    passExecutionContext.device = deviceManager.GetDevice();
    passExecutionContext.frameIndex = m_context.frameIndex;
    passExecutionContext.executionSlot = renderedFrameIndex;
    passExecutionContext.frameFenceValue = m_context.frameFenceValue;
    passExecutionContext.deltaTime = m_context.deltaTime;
    passExecutionContext.ownedHostData = m_frameInputs;
    passExecutionContext.hostData = passExecutionContext.ownedHostData.get();

    auto graphicsQueue = deviceManager.GetGraphicsQueue();
    auto computeQueue = deviceManager.GetComputeQueue();

    SyncOpenRenderGraphSettings(m_numFramesInFlight);

    {
        BT_ZONE_SCOPE("Renderer::Render::CLodVisibilityTelemetry");
        MaybeRequestCLodVisibilityTelemetry();
        MaybeRequestCLodVirtualShadowTelemetry();
    }
    runCapturedStage("RenderGraphExecute", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::RenderGraphExecute");
        if (renderGraphBatchTraceEnabled) {
            ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "before RenderGraph::Execute");
            spdlog::info("Renderer: frame {} entering RenderGraph::Execute", m_totalFramesRendered);
        }
        const rhi::Backend activeBackend = deviceManager.GetBackend();
        try {
            br::telemetry::nvperf::BeginFrameCapture(
                activeBackend,
                deviceManager.GetDevice(),
                graphicsQueue,
                computeQueue,
                m_totalFramesRendered);
            passExecutionContext.beginGpuPassRange = [activeBackend](rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName) {
                br::telemetry::nvperf::BeginPassRange(activeBackend, commandList, queue, queueName, passName);
            };
            passExecutionContext.endGpuPassRange = [activeBackend](rhi::CommandList commandList, rhi::Queue queue) {
                br::telemetry::nvperf::EndPassRange(activeBackend, commandList, queue);
            };
            currentRenderGraph->Execute(passExecutionContext); // Main render graph execution
            passExecutionContext.beginGpuPassRange = {};
            passExecutionContext.endGpuPassRange = {};
            static const std::filesystem::path submittedCameraTracePath = [] {
                wchar_t* value = nullptr;
                size_t length = 0;
                _wdupenv_s(&value, &length, L"SARP_SUBMITTED_CAMERA_TELEMETRY_PATH");
                std::filesystem::path result = value && value[0] ? value : L"";
                std::free(value);
                return result;
            }();
            if (!submittedCameraTracePath.empty()) {
                const auto submittedData = currentRenderGraph->GetLastSubmittedFrameData();
                const auto* submitted = submittedData ? submittedData->Get<RenderContext>() : nullptr;
                const auto views = submitted ? submitted->viewFamily : nullptr;
                const auto primary = views ? std::find_if(views->views.begin(), views->views.end(),
                    [](const auto& view) { return view.primary; }) : decltype(views->views.begin()){};
                if (submitted && views && primary != views->views.end()) {
                    const auto previousInverse = DirectX::XMMatrixInverse(nullptr, primary->cameraInfo.prevView);
                    std::error_code fileError;
                    if (submittedCameraTracePath.has_parent_path())
                        std::filesystem::create_directories(submittedCameraTracePath.parent_path(), fileError);
                    const bool writeHeader = !std::filesystem::exists(submittedCameraTracePath, fileError) ||
                        std::filesystem::file_size(submittedCameraTracePath, fileError) == 0u;
                    std::ofstream output(submittedCameraTracePath, std::ios::app);
                    if (writeHeader)
                        output << "present_call,owned_frame,preparation_slot,view_snapshot_revision,x,y,z,prev_x,prev_y,prev_z,context_x,context_y,context_z,jitter_x,jitter_y\n";
                    output << m_totalFramesRendered << ',' << submitted->frameNumber << ','
                        << submitted->frameSlot << ',' << views->revision << ','
                        << primary->cameraInfo.positionWorldSpace.x << ','
                        << primary->cameraInfo.positionWorldSpace.y << ','
                        << primary->cameraInfo.positionWorldSpace.z << ','
                        << DirectX::XMVectorGetX(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetY(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetZ(previousInverse.r[3]) << ','
                        << submitted->primaryCamera.info.positionWorldSpace.x << ','
                        << submitted->primaryCamera.info.positionWorldSpace.y << ','
                        << submitted->primaryCamera.info.positionWorldSpace.z << ','
                        << submitted->primaryCamera.jitterPixelSpace.x << ','
                        << submitted->primaryCamera.jitterPixelSpace.y << '\n';
                }
            }
            if (renderGraphBatchTraceEnabled) {
                spdlog::info("Renderer: frame {} completed RenderGraph::Execute", m_totalFramesRendered);
            }
        }
        catch (const std::exception& ex) {
            passExecutionContext.beginGpuPassRange = {};
            passExecutionContext.endGpuPassRange = {};
            spdlog::critical("Renderer: frame {} RenderGraph::Execute threw: {}", m_totalFramesRendered, ex.what());
            WriteRendererExceptionNote(
                "RenderGraphExecute",
                m_totalFramesRendered,
                renderedFrameIndex,
                m_currentFrameFenceValue,
                ex);
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
                logger->flush();
            });
            throw;
        }
    });

    if (!currentRenderGraph->GetLastPresentDependency()) {
        basic_telemetry::AddCounter("ORG.PresentationTail.DeferredForRecording");
        br::telemetry::nvperf::EndFrameCapture(
            deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
        PublishFrameTaskGraphCapture();
        FrameMark;
        return;
    }

    // Acquire/bind the swapchain image only after scene recording and FIFO
    // submission selected the logical frame. PresentationColor is slot-owned
    // and was left in CopySource by PresentationReadyPass.
    const auto presentationSlot = currentRenderGraph->GetLastExecutedPreparationSlot();
    const auto sourceSlot = static_cast<size_t>(presentationSlot.value_or(renderedFrameIndex));
    if (sourceSlot >= m_presentationColorResources.size()
        || renderedFrameIndex >= m_backbufferResources.size()
        || renderedFrameIndex >= m_commandAllocators.size()
        || renderedFrameIndex >= m_commandLists.size()) {
        throw std::runtime_error("Presentation tail has incomplete slot ownership");
    }
    auto presentationSource = m_presentationColorResources[sourceSlot];
    auto currentBackbufferResource = m_backbufferResources[renderedFrameIndex];
    if (!presentationSource || !currentBackbufferResource
        || !presentationSource->HasValidBackingResource()
        || !currentBackbufferResource->HasHandle()) {
        throw std::runtime_error("Presentation tail has invalid resource bindings");
    }
    auto& presentationAllocator = m_commandAllocators[renderedFrameIndex];
    auto& presentationList = m_commandLists[renderedFrameIndex];
    auto recordPresentationTail = std::make_shared<std::packaged_task<void()>>(
        [presentationSource, currentBackbufferResource,
         &presentationAllocator, &presentationList]() {
    presentationAllocator->Recycle();
    presentationList->Recycle(presentationAllocator.Get());
    rhi::TextureSubresourceRange presentationRange{};
    presentationRange.mipCount = 1;
    presentationRange.layerCount = 1;
    rhi::TextureBarrier backbufferToCopy{};
    backbufferToCopy.texture = currentBackbufferResource->GetHandle();
    backbufferToCopy.range = presentationRange;
    backbufferToCopy.beforeSync = rhi::ResourceSyncState::All;
    backbufferToCopy.afterSync = rhi::ResourceSyncState::Copy;
    backbufferToCopy.beforeAccess = rhi::ResourceAccessType::Present;
    backbufferToCopy.afterAccess = rhi::ResourceAccessType::CopyDest;
    backbufferToCopy.beforeLayout = rhi::ResourceLayout::Present;
    backbufferToCopy.afterLayout = rhi::ResourceLayout::CopyDest;
    rhi::BarrierBatch beforePresentationCopy{};
    beforePresentationCopy.textures = {&backbufferToCopy, 1};
    presentationList->Barriers(beforePresentationCopy);

    rhi::TextureCopyRegion sourceRegion{};
    sourceRegion.texture = presentationSource->GetAPIResource().GetHandle();
    rhi::TextureCopyRegion destinationRegion{};
    destinationRegion.texture = currentBackbufferResource->GetHandle();
    presentationList->CopyTextureRegion(destinationRegion, sourceRegion);

    auto backbufferToPresent = backbufferToCopy;
    backbufferToPresent.beforeSync = rhi::ResourceSyncState::Copy;
    backbufferToPresent.afterSync = rhi::ResourceSyncState::All;
    backbufferToPresent.beforeAccess = rhi::ResourceAccessType::CopyDest;
    backbufferToPresent.afterAccess = rhi::ResourceAccessType::Present;
    backbufferToPresent.beforeLayout = rhi::ResourceLayout::CopyDest;
    backbufferToPresent.afterLayout = rhi::ResourceLayout::Present;
    rhi::BarrierBatch afterPresentationCopy{};
    afterPresentationCopy.textures = {&backbufferToPresent, 1};
    presentationList->Barriers(afterPresentationCopy);
    presentationList->End();
        });
    auto presentationTailReady = recordPresentationTail->get_future();
    const bool presentationTailSubmitted = m_presentationTailScope.Valid()
        && TaskSchedulerManager::GetInstance().SubmitCpu(
            m_presentationTailScope, TaskLane::FrameCritical, TaskDomain::General,
            "Renderer::RecordPresentationTail",
            [recordPresentationTail](const br::TaskContext&) { (*recordPresentationTail)(); });
    if (!presentationTailSubmitted)
        throw std::runtime_error("Presentation tail recording task was rejected");
    recordPresentationTail.reset();
    presentationTailReady.get();
    basic_telemetry::AddCounter("ORG.PresentationTail.WorkerRecorded");

    if (const auto dependency = currentRenderGraph->GetLastPresentDependency();
        dependency && dependency->valid
        && graphicsQueue.Wait(dependency->wait) != rhi::Result::Ok) {
        throw std::runtime_error("Presentation tail failed to wait for scene output");
    }
    auto presentationCommandList = presentationList.Get();
    if (graphicsQueue.Submit({&presentationCommandList, 1}) != rhi::Result::Ok) {
        throw std::runtime_error("Presentation tail submission failed");
    }
    currentRenderGraph->ConfirmPresentationTailSubmission();
    basic_telemetry::AddCounter("ORG.PresentationTail.Submitted");

    // Present the frame
    rhi::Result presentResult = rhi::Result::Ok;
    runCapturedStage("Present", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::Present");
        if (renderGraphBatchTraceEnabled) {
            spdlog::info("Renderer: frame {} calling Present for slot {}", m_totalFramesRendered, renderedFrameIndex);
        }
        // The tail was submitted on the presentation queue immediately before
        // this call, so queue order covers the copy and Present transition.
        presentResult = m_swapChain->Present(!m_allowTearing);
    });
	if (presentResult == rhi::Result::ModeChanged) {
		RECT clientRect{};
		if (m_hwnd && GetClientRect(m_hwnd, &clientRect)) {
			const UINT clientWidth = static_cast<UINT>((std::max)(clientRect.right - clientRect.left, 0L));
			const UINT clientHeight = static_cast<UINT>((std::max)(clientRect.bottom - clientRect.top, 0L));
			if (clientWidth != 0 && clientHeight != 0) {
				spdlog::info(
					"Renderer: presentation reported mode change; rebuilding swapchain for client extent {}x{}",
					clientWidth,
					clientHeight);
				OnResize(clientWidth, clientHeight);
				return;
			}
		}
		spdlog::warn("Renderer: presentation reported mode change but no non-zero client extent is available");
		return;
	}
	if (presentResult != rhi::Result::Ok) {
		spdlog::error("Renderer: swapchain presentation failed with {}", rhi::ResultName(presentResult));
		return;
	}

    runCapturedStage("SignalFence", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::SignalFence");
        const auto preparationSlot = currentRenderGraph->GetLastExecutedPreparationSlot();
        SignalFence(graphicsQueue,
            static_cast<uint8_t>(preparationSlot.value_or(renderedFrameIndex)));
        br::telemetry::nvperf::EndFrameCapture(deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
    });

    AdvanceFrameIndex();

    runCapturedStage("ReadbackRequests", [&]() {
        if (currentRenderGraph) {
            BT_ZONE_SCOPE("Renderer::Render::ReadbackRequests");
            if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                readbackService->ProcessReadbackRequests(); // Process readback captures
            }
        }
        if (m_pReadbackManager) {
            m_pReadbackManager->ProcessReadbackRequests(); // Save images to disk if requested
        }
    });
    PublishFrameTaskGraphCapture();
    FrameMark;
}
