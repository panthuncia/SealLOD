#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapSetupPass.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Streaming/LightStateArtifacts.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "Scene/ECS/RendererECSManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowSetupRootConstants.h"

namespace {

uint32_t GetVirtualShadowVirtualResolution()
{
    return CLodVirtualShadowBuildRuntimeResolutionConfig().virtualResolution;
}

CLodVirtualShadowResolutionConfig GetVirtualShadowResolutionConfig()
{
    return CLodVirtualShadowBuildRuntimeResolutionConfig();
}

uint32_t WrapPageOffset(int64_t pageCoord, uint32_t pageTableResolution)
{
    if (pageTableResolution == 0u) {
        return 0u;
    }

    const int64_t resolution = static_cast<int64_t>(pageTableResolution);
    int64_t wrapped = pageCoord % resolution;
    if (wrapped < 0) {
        wrapped += resolution;
    }

    return static_cast<uint32_t>(wrapped);
}

float ExtractOrthographicWidth(const DirectX::XMMATRIX& projection)
{
    const float m11 = DirectX::XMVectorGetX(projection.r[0]);
    return std::abs(m11) > 1.0e-6f ? (2.0f / std::abs(m11)) : 0.0f;
}

float ExtractOrthographicHeight(const DirectX::XMMATRIX& projection)
{
    const float m22 = DirectX::XMVectorGetY(projection.r[1]);
    return std::abs(m22) > 1.0e-6f ? (2.0f / std::abs(m22)) : 0.0f;
}

bool NearlyEqualFloat(float lhs, float rhs, float epsilon = 1.0e-5f)
{
    return std::abs(lhs - rhs) <= epsilon * std::max(std::max(std::abs(lhs), std::abs(rhs)), 1.0f);
}

bool NearlyEqualDirection(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs, float cosineThreshold = 0.9999f)
{
    const float dot = lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    return dot >= cosineThreshold;
}

bool ClipmapStructureEquals(const CLodVirtualShadowClipmapInfo& lhs, const CLodVirtualShadowClipmapInfo& rhs)
{
    return NearlyEqualFloat(lhs.texelWorldSize, rhs.texelWorldSize) &&
        NearlyEqualFloat(lhs.directionalLodBias, rhs.directionalLodBias) &&
        NearlyEqualFloat(lhs.depthNear, rhs.depthNear) &&
        NearlyEqualFloat(lhs.depthRange, rhs.depthRange) &&
        lhs.pageTableLayer == rhs.pageTableLayer &&
        lhs.clipLevel == rhs.clipLevel &&
        lhs.virtualResolution == rhs.virtualResolution &&
        lhs.pageTableResolution == rhs.pageTableResolution &&
        lhs.physicalAtlasPagesWide == rhs.physicalAtlasPagesWide &&
        lhs.physicalAtlasPagesHigh == rhs.physicalAtlasPagesHigh &&
    ((lhs.flags & CLodVirtualShadowClipmapValidFlag) == (rhs.flags & CLodVirtualShadowClipmapValidFlag));
}

bool IsClipmapValid(const CLodVirtualShadowClipmapInfo& clipmapInfo)
{
    return (clipmapInfo.flags & CLodVirtualShadowClipmapValidFlag) != 0u &&
        clipmapInfo.shadowCameraBufferIndex != 0xFFFFFFFFu;
}

int32_t ClampClearOffset(int64_t delta, uint32_t pageTableResolution)
{
    if (pageTableResolution == 0u) {
        return 0;
    }

    const int64_t limit = static_cast<int64_t>(pageTableResolution);
    if (delta >= limit) {
        return static_cast<int32_t>(pageTableResolution);
    }
    if (delta <= -limit) {
        return -static_cast<int32_t>(pageTableResolution);
    }

    return static_cast<int32_t>(delta);
}

} // namespace

VirtualShadowMapSetupPass::VirtualShadowMapSetupPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> markClipmapDataBuffer,
    std::shared_ptr<org::Buffer> compactMainCameraBuffer,
    std::shared_ptr<org::Buffer> compactShadowCameraBuffer,
    std::shared_ptr<org::Buffer> statsBuffer,
    std::shared_ptr<org::Buffer> runtimeStateBuffer,
    std::shared_ptr<org::Buffer> fallbackCandidateCountBuffer,
    std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters,
    bool forceResetResources)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_markClipmapDataBuffer(std::move(markClipmapDataBuffer))
    , m_compactMainCameraBuffer(std::move(compactMainCameraBuffer))
    , m_compactShadowCameraBuffer(std::move(compactShadowCameraBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_runtimeStateBuffer(std::move(runtimeStateBuffer))
    , m_fallbackCandidateCountBuffer(std::move(fallbackCandidateCountBuffer))
    , m_virtualShadowCasters(std::move(virtualShadowCasters))
    , m_forceResetResources(forceResetResources)
{
}

VirtualShadowMapSetupBindings VirtualShadowMapSetupPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.UnorderedAccess(
        m_compactMainCameraBuffer,
        m_compactShadowCameraBuffer);

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/clodUtil.hlsl",
                L"CLodVirtualShadowSetupCSMain", "CLod.VirtualShadow.Setup.PSO")),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_pageMetadataBuffer),
        builder.UnorderedAccess(m_allocationCountBuffer), builder.UnorderedAccess(m_dirtyPageFlagsBuffer),
        builder.UnorderedAccess(m_clipmapInfoBuffer), builder.UnorderedAccess(m_markClipmapDataBuffer),
        builder.UnorderedAccess(m_statsBuffer), builder.UnorderedAccess(m_runtimeStateBuffer),
        builder.UnorderedAccess(m_fallbackCandidateCountBuffer)};
}

void VirtualShadowMapSetupPass::Initialize() {}

void VirtualShadowMapSetupPass::Update(const org::UpdateExecutionContext& executionContext)
{
    const bool disableVirtualShadowPageCaching =
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableVirtualShadowPageCachingSettingName)();
    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    const bool renderResolutionChanged =
        updateContext != nullptr &&
        m_previousRenderResolutionValid &&
        (m_previousRenderResolution.x != updateContext->renderResolution.x ||
            m_previousRenderResolution.y != updateContext->renderResolution.y);
    if (renderResolutionChanged) {
        // Virtual shadow page marking samples the primary linear-depth texture
        // before the current frame repopulates it, so a resolution change needs
        // one reset for the resize frame and one more once the new-size depth is valid.
        m_pendingRenderResolutionResetFrames = 2u;
    }
    const bool renderResolutionResetPending = m_pendingRenderResolutionResetFrames > 0u;
    const bool forceResetResources = m_forceResetResources || disableVirtualShadowPageCaching || renderResolutionResetPending;
    m_forceResetResources = false;
    m_feedbackRecoveryRefresh =
        g_clodVirtualShadowFeedbackRecoveryRequested.exchange(
            false,
            std::memory_order_acq_rel);
    if (renderResolutionResetPending) {
        --m_pendingRenderResolutionResetFrames;
    }
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = GetVirtualShadowResolutionConfig();
    const uint32_t virtualShadowResolution = virtualShadowConfig.virtualResolution;
    const uint32_t virtualShadowPageTableResolution = virtualShadowConfig.pageTableResolution;
    const uint32_t virtualShadowPhysicalPageCount = virtualShadowConfig.maxPhysicalPages;
    const uint32_t virtualShadowPhysicalAtlasPagesWide = virtualShadowConfig.physicalAtlasPagesWide;
    const uint32_t virtualShadowPhysicalAtlasPagesHigh = virtualShadowConfig.physicalAtlasPagesHigh;

    m_resetReasonForced = forceResetResources;
    m_resetReasonNoPreviousState = !m_previousClipmapInfosValid;
    m_resetReasonStructureMismatch = false;
    m_resetReasonLightDirectionChanged = false;
    m_resetResources = m_resetReasonForced || m_resetReasonNoPreviousState;

    CLodVirtualShadowRuntimeState runtimeState{};
    // Slice rows, then one view row per camera buffer index (CLodVirtualShadowClipInfoRowCount).
    std::vector<CLodVirtualShadowClipmapInfo> clipmapInfos(CLodVirtualShadowClipInfoRowCount);
    std::array<CLodVirtualShadowMarkClipmapData, CLodVirtualShadowMaxSupportedClipmapCount> markClipmapData{};
    std::vector<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameras(CLodVirtualShadowMaxSliceCount);
    const auto setViewRow = [&](uint32_t cameraBufferIndex, uint32_t slice) {
        if (cameraBufferIndex >= CLodVirtualShadowMaxViewRowCount) {
            if (!m_warnedViewRowOverflow) {
                spdlog::warn("Virtual shadow view with camera buffer index {} has no view row (at most {}); it renders nothing",
                    cameraBufferIndex, CLodVirtualShadowMaxViewRowCount);
                m_warnedViewRowOverflow = true;
            }
            return;
        }
        auto& row = clipmapInfos[CLodVirtualShadowViewRowBase + cameraBufferIndex];
        row.flags = CLodVirtualShadowClipmapValidFlag;
        row.pageTableLayer = slice;
        row.shadowCameraBufferIndex = cameraBufferIndex;
    };
    CLodVirtualShadowMainCameraInfo compactMainCamera{};
    DirectX::XMFLOAT3 currentDirectionalLightDirection{};
    bool currentDirectionalLightDirectionValid = false;
    uint32_t activeClipmapCount = 0u;

    if (updateContext) {
        for (const auto& view : updateContext->Views()) {
            if (!view.primary) continue;
            compactMainCamera.positionWorldSpace = view.cameraInfo.positionWorldSpace;
            compactMainCamera.viewInverse = view.cameraInfo.viewInverse;
            compactMainCamera.projectionInverse = view.cameraInfo.projectionInverse;
            // Page marking reads the linear depth before this frame writes it, so
            // its receivers come from the previous frame's camera. Without a
            // previous frame (this pass's first) the current camera is all there is.
            const bool markWithCurrentCamera = !m_previousMarkCameraValid;
            compactMainCamera.markDepthViewInverse = markWithCurrentCamera
                ? view.cameraInfo.viewInverse
                : DirectX::XMMatrixInverse(nullptr, view.cameraInfo.prevView);
            compactMainCamera.markDepthProjectionInverse = markWithCurrentCamera
                ? view.cameraInfo.projectionInverse
                // Matches projectionInverse, which inverts the jittered projection.
                : DirectX::XMMatrixInverse(nullptr, view.cameraInfo.prevJitteredProjection);
            m_previousMarkCameraValid = true;
            break;
        }

        const auto& publishedLights = updateContext->lightTables;
        if (publishedLights && !publishedLights->directionalShadows.empty()) {
            const auto& lightViewInfo = publishedLights->directionalShadows.front();
            currentDirectionalLightDirection = lightViewInfo.direction;
            currentDirectionalLightDirectionValid = true;
            const uint32_t clipmapCount = std::min<uint32_t>(
                static_cast<uint32_t>(lightViewInfo.viewIDs.size()),
                CLodVirtualShadowMaxSupportedClipmapCount);
            activeClipmapCount = clipmapCount;
			// DynamicWind's world-space effect cutoff is also the distance through
			// which skinned VSM clipmaps are required. Keep the two workloads on one
			// quality control instead of allowing shadow skinning beyond wind reach.
			const float configuredSkinnedShadowRadius = (std::max)(0.0f,
				updateContext->proceduralWind.outerRadius);
			const float casterDynamicShadowRadius = m_virtualShadowCasters
				? m_virtualShadowCasters->GetRequestedDynamicShadowRadius()
				: 0.0f;
			const float skinnedShadowRadius = (std::max)(
				configuredSkinnedShadowRadius, casterDynamicShadowRadius);
			const int32_t dynamicClipmapOverride =
				SettingsManager::GetInstance().getSettingGetter<int32_t>(
					CLodSkinnedShadowDynamicClipmapCountOverrideSettingName)();
			const uint32_t clampedOverride = dynamicClipmapOverride < 0
				? 0u
				: (std::min)(static_cast<uint32_t>(dynamicClipmapOverride), clipmapCount);
			bool autoBoundaryFound = skinnedShadowRadius <= 0.0f;
			uint32_t effectiveDynamicClipmapCount = dynamicClipmapOverride >= 0 ? clampedOverride : 0u;
			uint32_t reclassifiedClipmapCount = 0u;

            for (uint32_t clipmapIndex = 0; clipmapIndex < clipmapCount; ++clipmapIndex) {
                const auto viewIt = std::ranges::find(updateContext->Views(),
                    lightViewInfo.viewIDs[clipmapIndex], &PreparedViewFrameData::id);
                if (viewIt == updateContext->Views().end()) {
                    continue;
                }
                const auto& view = *viewIt;

                const float orthoWidth = ExtractOrthographicWidth(view.cameraInfo.unjitteredProjection);
                const float orthoHeight = ExtractOrthographicHeight(view.cameraInfo.unjitteredProjection);
                const float virtualShadowResolutionFloat = static_cast<float>(virtualShadowResolution);
				const bool dynamicSkinnedClipmap = dynamicClipmapOverride >= 0
					? clipmapIndex < clampedOverride
					: !autoBoundaryFound;
				if (dynamicClipmapOverride < 0 && dynamicSkinnedClipmap) {
					effectiveDynamicClipmapCount = clipmapIndex + 1u;
					if (0.5f * (std::max)(orthoWidth, orthoHeight) >= skinnedShadowRadius) {
						autoBoundaryFound = true;
					}
				}

                auto& clipmapInfo = clipmapInfos[clipmapIndex];
                // Offsets and matrices come from the same view snapshot. The light
                // table publishes asynchronously; pairing its offsets with this
                // frame's matrices addressed neighbouring pages for a frame or two
                // whenever a clip scrolled, flashing the whole clip.
                const int64_t pageOffsetX = view.virtualShadowPageOffsetX;
                const int64_t pageOffsetY = view.virtualShadowPageOffsetY;
                clipmapInfo.worldOriginX = view.cameraInfo.positionWorldSpace.x;
                clipmapInfo.worldOriginY = view.cameraInfo.positionWorldSpace.y;
                clipmapInfo.worldOriginZ = view.cameraInfo.positionWorldSpace.z;
                clipmapInfo.texelWorldSize = std::max(orthoWidth, orthoHeight) / std::max(virtualShadowResolutionFloat, 1.0f);
                clipmapInfo.pageOffsetX = WrapPageOffset(pageOffsetX, virtualShadowPageTableResolution);
                clipmapInfo.pageOffsetY = WrapPageOffset(pageOffsetY, virtualShadowPageTableResolution);
                clipmapInfo.pageTableLayer = clipmapIndex;
                clipmapInfo.shadowCameraBufferIndex = view.cameraBufferIndex;
                clipmapInfo.clipLevel = clipmapIndex;
				const uint32_t previousDynamicFlag = m_previousClipmapInfosValid
					? m_previousClipmapInfos[clipmapIndex].flags & CLodVirtualShadowClipmapDynamicSkinnedFlag
					: 0u;
				const uint32_t currentDynamicFlag = dynamicSkinnedClipmap
					? CLodVirtualShadowClipmapDynamicSkinnedFlag
					: 0u;
				clipmapInfo.flags = CLodVirtualShadowClipmapValidFlag | currentDynamicFlag;
				if (m_previousClipmapInfosValid && previousDynamicFlag != currentDynamicFlag) {
					clipmapInfo.flags |= CLodVirtualShadowClipmapInvalidateFlag;
					++reclassifiedClipmapCount;
				}
                clipmapInfo.directionalLodBias = virtualShadowConfig.directionalLodBias;
                clipmapInfo.virtualResolution = virtualShadowResolution;
                clipmapInfo.pageTableResolution = virtualShadowPageTableResolution;
                clipmapInfo.physicalAtlasPagesWide = virtualShadowPhysicalAtlasPagesWide;
                clipmapInfo.physicalAtlasPagesHigh = virtualShadowPhysicalAtlasPagesHigh;
                clipmapInfo.unwrappedPageOffsetX =
                    static_cast<int32_t>(pageOffsetX);
                clipmapInfo.unwrappedPageOffsetY =
                    static_cast<int32_t>(pageOffsetY);
                clipmapInfo.depthNear = view.cameraInfo.zNear;
                clipmapInfo.depthRange = std::max(
                    view.cameraInfo.zFar - view.cameraInfo.zNear,
                    1.0e-6f);

                auto& markData = markClipmapData[clipmapIndex];
                markData.texelWorldSize = clipmapInfo.texelWorldSize;
                markData.pageOffsetX = clipmapInfo.pageOffsetX;
                markData.pageOffsetY = clipmapInfo.pageOffsetY;
                markData.pageTableLayer = clipmapInfo.pageTableLayer;
                markData.flags = clipmapInfo.flags;
                markData.directionalLodBias = clipmapInfo.directionalLodBias;
                markData.virtualResolution = clipmapInfo.virtualResolution;
                markData.pageTableResolution = clipmapInfo.pageTableResolution;
                markData.physicalAtlasPagesWide = clipmapInfo.physicalAtlasPagesWide;
                markData.physicalAtlasPagesHigh = clipmapInfo.physicalAtlasPagesHigh;
                markData.unwrappedPageOffsetX =
                    clipmapInfo.unwrappedPageOffsetX;
                markData.unwrappedPageOffsetY =
                    clipmapInfo.unwrappedPageOffsetY;
                markData.shadowViewProjection = view.cameraInfo.viewProjection;

                compactShadowCameras[clipmapIndex].view = view.cameraInfo.view;
                compactShadowCameras[clipmapIndex].projection = view.cameraInfo.jitteredProjection;
                compactShadowCameras[clipmapIndex].viewProjection = view.cameraInfo.viewProjection;
                compactShadowCameras[clipmapIndex].isOrtho = view.cameraInfo.isOrtho;
                setViewRow(view.cameraBufferIndex, clipmapIndex);
                if (m_previousClipmapInfosValid && IsClipmapValid(m_previousClipmapInfos[clipmapIndex])) {
                    clipmapInfo.clearOffsetX = ClampClearOffset(
                        pageOffsetX - m_previousClipmapPageOffsetX[clipmapIndex],
                        virtualShadowPageTableResolution);
                    clipmapInfo.clearOffsetY = ClampClearOffset(
                        pageOffsetY - m_previousClipmapPageOffsetY[clipmapIndex],
                        virtualShadowPageTableResolution);
                }
                m_previousClipmapPageOffsetX[clipmapIndex] = pageOffsetX;
                m_previousClipmapPageOffsetY[clipmapIndex] = pageOffsetY;
            }
			g_clodSkinnedShadowEffectiveDynamicClipmapCount.store(
				effectiveDynamicClipmapCount, std::memory_order_relaxed);
			g_clodSkinnedShadowActiveClipmapCount.store(
				clipmapCount, std::memory_order_relaxed);
			g_clodSkinnedShadowDynamicClipmapMask.store(
				effectiveDynamicClipmapCount >= 32u
					? 0xFFFFFFFFu
					: ((1u << effectiveDynamicClipmapCount) - 1u),
				std::memory_order_relaxed);
			if (reclassifiedClipmapCount != 0u) {
				g_clodSkinnedShadowClassificationGeneration.fetch_add(1u, std::memory_order_relaxed);
				g_clodSkinnedShadowOneShotInvalidationCount.fetch_add(
					reclassifiedClipmapCount, std::memory_order_relaxed);
			}
        }
    }

    if (currentDirectionalLightDirectionValid &&
        m_previousDirectionalLightDirectionValid &&
        !NearlyEqualDirection(currentDirectionalLightDirection, m_previousDirectionalLightDirection)) {
        m_resetReasonLightDirectionChanged = true;
        m_resetResources = true;
    }

    for (uint32_t clipmapIndex = 0; clipmapIndex < CLodVirtualShadowMaxSupportedClipmapCount; ++clipmapIndex) {
        auto& info = clipmapInfos[clipmapIndex];
        auto& markData = markClipmapData[clipmapIndex];
        info.pageTableLayer = clipmapIndex;
        info.directionalLodBias = virtualShadowConfig.directionalLodBias;
        info.virtualResolution = virtualShadowResolution;
        info.pageTableResolution = virtualShadowPageTableResolution;
        info.physicalAtlasPagesWide = virtualShadowPhysicalAtlasPagesWide;
        info.physicalAtlasPagesHigh = virtualShadowPhysicalAtlasPagesHigh;
        if (info.shadowCameraBufferIndex == 0xFFFFFFFFu) {
            info.texelWorldSize = static_cast<float>(CLodVirtualShadowPhysicalPageSize << clipmapIndex);
        }

        markData.texelWorldSize = info.texelWorldSize;
        markData.pageOffsetX = info.pageOffsetX;
        markData.pageOffsetY = info.pageOffsetY;
        markData.pageTableLayer = info.pageTableLayer;
        markData.flags = info.flags;
        markData.directionalLodBias = info.directionalLodBias;
        markData.virtualResolution = info.virtualResolution;
        markData.pageTableResolution = info.pageTableResolution;
        markData.physicalAtlasPagesWide = info.physicalAtlasPagesWide;
        markData.physicalAtlasPagesHigh = info.physicalAtlasPagesHigh;
        markData.unwrappedPageOffsetX = info.unwrappedPageOffsetX;
        markData.unwrappedPageOffsetY = info.unwrappedPageOffsetY;

        if (!m_resetResources && !ClipmapStructureEquals(info, m_previousClipmapInfos[clipmapIndex])) {
            m_resetReasonStructureMismatch = true;
            m_resetResources = true;
        }
    }

    std::copy_n(clipmapInfos.begin(), CLodVirtualShadowMaxSupportedClipmapCount, m_previousClipmapInfos.begin());
    m_previousClipmapInfosValid = true;
    m_previousDirectionalLightDirection = currentDirectionalLightDirection;
    m_previousDirectionalLightDirectionValid = currentDirectionalLightDirectionValid;
    if (updateContext) {
        m_previousRenderResolution = updateContext->renderResolution;
        m_previousRenderResolutionValid = true;
    }

    // Local light faces. Each owns two slices: mip 0, then mips 1-7 packed. Their pages never scroll, so the clipmap
    // offsets stay zero; a face whose light or camera changed starts over (Invalidate).
    for (uint32_t slice = CLodVirtualShadowFirstLocalSlice; slice < CLodVirtualShadowMaxSliceCount; ++slice) {
        auto& info = clipmapInfos[slice];
        info.pageTableLayer = slice;
        info.virtualResolution = virtualShadowResolution;
        info.pageTableResolution = virtualShadowPageTableResolution;
        info.physicalAtlasPagesWide = virtualShadowPhysicalAtlasPagesWide;
        info.physicalAtlasPagesHigh = virtualShadowPhysicalAtlasPagesHigh;
    }
    uint32_t activeLocalFaceCount = 0u;
    std::array<bool, CLodVirtualShadowMaxLocalFaceCount> localFaceSeen{};
    const float localLodBias = SettingsManager::GetInstance().getSettingGetter<float>(CLodLocalVirtualShadowLodBiasSettingName)();
    const auto& publishedLocalLights = updateContext ? updateContext->lightTables : nullptr;
    if (publishedLocalLights) {
        // As for the sun's clips: skinned and wind-deformed casters render into a face's dynamic layer every frame only
        // where they animate, within the wind's reach of the camera. Elsewhere they are cached like rigid casters.
        const float localSkinnedShadowRadius = (std::max)((std::max)(0.0f, updateContext->proceduralWind.outerRadius),
            m_virtualShadowCasters ? m_virtualShadowCasters->GetRequestedDynamicShadowRadius() : 0.0f);
        const DirectX::XMVECTOR cameraPosition = DirectX::XMLoadFloat4(&compactMainCamera.positionWorldSpace);
        for (const auto& local : publishedLocalLights->localShadows) {
            for (size_t i = 0; i < local.viewIDs.size(); ++i) {
                const uint32_t face = local.firstFace + static_cast<uint32_t>(i);
                if (face >= CLodVirtualShadowMaxLocalFaceCount) continue;
                const auto viewIt = std::ranges::find(updateContext->Views(), local.viewIDs[i], &PreparedViewFrameData::id);
                if (viewIt == updateContext->Views().end()) continue;
                const auto& view = *viewIt;
                const float lightDistance = DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(
                    DirectX::XMVectorSetW(DirectX::XMLoadFloat4(&view.cameraInfo.positionWorldSpace), 0.0f),
                    DirectX::XMVectorSetW(cameraPosition, 0.0f))));
                const bool dynamicSkinned = localSkinnedShadowRadius > 0.0f &&
                    lightDistance - view.cameraInfo.zFar < localSkinnedShadowRadius;
                DirectX::XMFLOAT4X4 viewProjection;
                DirectX::XMStoreFloat4x4(&viewProjection, view.cameraInfo.viewProjection);
                auto& previous = m_previousLocalFaces[face];
                const bool changed = !previous.valid || previous.lightIndex != local.lightIndex ||
                    previous.dynamicSkinned != dynamicSkinned ||
                    std::memcmp(&previous.viewProjection, &viewProjection, sizeof(viewProjection)) != 0;
                previous = {true, local.lightIndex, dynamicSkinned, viewProjection};
                localFaceSeen[face] = true;
                const uint32_t firstSlice = CLodVirtualShadowFirstLocalSlice + face * CLodVirtualShadowLocalSlicesPerFace;
                // A face's texels widen with depth: texelWorldSize is mip 0's at unit view depth.
                const float projectionScale = DirectX::XMVectorGetY(view.cameraInfo.unjitteredProjection.r[1]);
                const float texelPerDepth = projectionScale > 0.0f
                    ? 2.0f / (projectionScale * static_cast<float>(std::max(virtualShadowResolution, 1u)))
                    : 0.0f;
                for (uint32_t s = 0; s < CLodVirtualShadowLocalSlicesPerFace; ++s) {
                    const uint32_t slice = firstSlice + s;
                    auto& info = clipmapInfos[slice];
                    info.worldOriginX = view.cameraInfo.positionWorldSpace.x;
                    info.worldOriginY = view.cameraInfo.positionWorldSpace.y;
                    info.worldOriginZ = view.cameraInfo.positionWorldSpace.z;
                    info.texelWorldSize = texelPerDepth;
                    info.shadowCameraBufferIndex = view.cameraBufferIndex;
                    info.clipLevel = s; // 0: mip 0's slice; 1: the slice of mips 1-7
                    info.flags = CLodVirtualShadowClipmapValidFlag | CLodVirtualShadowClipmapLocalFlag |
                        (dynamicSkinned ? CLodVirtualShadowClipmapDynamicSkinnedFlag : 0u) |
                        (changed ? CLodVirtualShadowClipmapInvalidateFlag : 0u) |
                        (local.viewIDs.size() > 1u ? CLodVirtualShadowClipmapCubeFlag : 0u);
                    info.directionalLodBias = localLodBias;
                    info.depthNear = view.cameraInfo.zNear;
                    info.depthRange = std::max(view.cameraInfo.zFar - view.cameraInfo.zNear, 1.0e-6f);
                    compactShadowCameras[slice].view = view.cameraInfo.view;
                    compactShadowCameras[slice].projection = view.cameraInfo.unjitteredProjection;
                    compactShadowCameras[slice].viewProjection = view.cameraInfo.viewProjection;
                    compactShadowCameras[slice].isOrtho = 0u;
                }
                setViewRow(view.cameraBufferIndex, firstSlice);
                activeLocalFaceCount = std::max(activeLocalFaceCount, face + 1u);
            }
        }
    }
    for (uint32_t face = 0; face < CLodVirtualShadowMaxLocalFaceCount; ++face) {
        if (!localFaceSeen[face]) m_previousLocalFaces[face].valid = false;
    }
    g_clodVirtualShadowActiveLocalFaceCount.store(activeLocalFaceCount, std::memory_order_relaxed);

    runtimeState.clipmapCount = activeClipmapCount;
    runtimeState.supportedClipmapCount = CLodVirtualShadowMaxSupportedClipmapCount;
    runtimeState.virtualResolution = virtualShadowResolution;
    runtimeState.pageTableResolution = virtualShadowPageTableResolution;
    runtimeState.physicalAtlasPagesWide = virtualShadowPhysicalAtlasPagesWide;
    runtimeState.physicalAtlasPagesHigh = virtualShadowPhysicalAtlasPagesHigh;
    runtimeState.maxPhysicalPages = virtualShadowPhysicalPageCount;
    runtimeState.maxAllocationRequests = virtualShadowConfig.maxAllocationRequests;
    runtimeState.directionalLodBias = virtualShadowConfig.directionalLodBias;
    UploadBufferData(&runtimeState, sizeof(runtimeState), org::runtime::UploadTarget::FromShared(m_runtimeStateBuffer), 0);

    UploadBufferData(
        clipmapInfos.data(),
        static_cast<uint32_t>(clipmapInfos.size() * sizeof(CLodVirtualShadowClipmapInfo)),
        org::runtime::UploadTarget::FromShared(m_clipmapInfoBuffer),
        0);

    UploadBufferData(
        markClipmapData.data(),
        static_cast<uint32_t>(markClipmapData.size() * sizeof(CLodVirtualShadowMarkClipmapData)),
        org::runtime::UploadTarget::FromShared(m_markClipmapDataBuffer),
        0);

    UploadBufferData(
        &compactMainCamera,
        sizeof(compactMainCamera),
        org::runtime::UploadTarget::FromShared(m_compactMainCameraBuffer),
        0);

    UploadBufferData(
        compactShadowCameras.data(),
        static_cast<uint32_t>(compactShadowCameras.size() * sizeof(CLodVirtualShadowCompactShadowCameraInfo)),
        org::runtime::UploadTarget::FromShared(m_compactShadowCameraBuffer),
        0);
}

VirtualShadowMapSetupFrame VirtualShadowMapSetupPass::Prepare(
    const VirtualShadowMapSetupBindings&, const org::PassPrepareContext&) const
{
    const auto config = GetVirtualShadowResolutionConfig();
    VirtualShadowMapSetupFrame frame{};
    frame.pageTableResolution = config.pageTableResolution;
    frame.sliceCount = CLodVirtualShadowActiveSliceCount();
    frame.packedConfig0 =
        ((config.pageTableResolution & CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_SHIFT) |
        ((frame.sliceCount & CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_SHIFT) |
        ((config.maxPhysicalPages & CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_SHIFT);
    frame.packedConfig1 = (CLodVirtualShadowDirtyWordCount(config.maxPhysicalPages) &
        CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_SHIFT;
    frame.packedFlags =
        ((m_resetResources ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES_BIT) |
        ((m_resetReasonForced ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_FORCED_BIT) |
        ((m_resetReasonNoPreviousState ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_NO_PREVIOUS_STATE_BIT) |
        ((m_resetReasonStructureMismatch ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_STRUCTURE_MISMATCH_BIT) |
        ((m_resetReasonLightDirectionChanged ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_LIGHT_DIRECTION_CHANGED_BIT) |
        ((SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName)() ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_ENABLED_BIT) |
        ((m_feedbackRecoveryRefresh ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_FEEDBACK_RECOVERY_REFRESH_BIT);
    frame.autoBiasScale = SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName)();
    return frame;
}

void VirtualShadowMapSetupPass::ShutdownPass() {}

void VirtualShadowMapSetupPass::Record(const VirtualShadowMapSetupBindings& bindings,
    const VirtualShadowMapSetupFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants c{};
    c[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    c[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    c[CLOD_VIRTUAL_SHADOW_SETUP_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.allocationCount);
    c[CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG0] = frame.packedConfig0;
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG1] = frame.packedConfig1;
    c[CLOD_VIRTUAL_SHADOW_SETUP_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    c[CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_FLAGS] = frame.packedFlags;
    c[CLOD_VIRTUAL_SHADOW_SETUP_MARK_CLIPMAP_DATA_DESCRIPTOR_INDEX] = recording.Index(bindings.markClipmapData);
    c[CLOD_VIRTUAL_SHADOW_SETUP_RUNTIME_STATE_DESCRIPTOR_INDEX] = recording.Index(bindings.runtimeState);
    c[CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_SCALE_AS_UINT] = std::bit_cast<uint32_t>(frame.autoBiasScale);
    c[CLOD_VIRTUAL_SHADOW_SETUP_FALLBACK_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.fallbackCandidateCount);
    const uint32_t groups = (frame.pageTableResolution + 7u) / 8u;
    br::render::DispatchCompute(recording, bindings.pipeline, c, groups, groups, frame.sliceCount);
}
