#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapSetupPass.h"
#include "Render/LightStateArtifacts.h"

#include <array>
#include <bit>
#include <cmath>

#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/VirtualShadowCasterProvider.h"
#include "Render/RenderContext.h"
#include "Render/RendererComponents.h"
#include "Render/RendererSettings.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowSetupRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

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
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowSetupCSMain",
        {},
        "CLod.VirtualShadow.Setup.PSO");
}

VirtualShadowMapSetupBindings VirtualShadowMapSetupPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithUnorderedAccess(
        m_compactMainCameraBuffer,
        m_compactShadowCameraBuffer);

    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    const uint32_t packedFlags =
        ((m_resetResources ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES_BIT) |
        ((m_resetReasonForced ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_FORCED_BIT) |
        ((m_resetReasonNoPreviousState ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_NO_PREVIOUS_STATE_BIT) |
        ((m_resetReasonStructureMismatch ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_STRUCTURE_MISMATCH_BIT) |
        ((m_resetReasonLightDirectionChanged ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_LIGHT_DIRECTION_CHANGED_BIT) |
        ((SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName)() ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_ENABLED_BIT) |
        ((m_feedbackRecoveryRefresh ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_FEEDBACK_RECOVERY_REFRESH_BIT);
    return {builder.BindUnorderedAccess(m_pageTableTexture), builder.BindUnorderedAccess(m_pageMetadataBuffer),
        builder.BindUnorderedAccess(m_allocationCountBuffer), builder.BindUnorderedAccess(m_dirtyPageFlagsBuffer),
        builder.BindUnorderedAccess(m_clipmapInfoBuffer), builder.BindUnorderedAccess(m_markClipmapDataBuffer),
        builder.BindUnorderedAccess(m_statsBuffer), builder.BindUnorderedAccess(m_runtimeStateBuffer),
        builder.BindUnorderedAccess(m_fallbackCandidateCountBuffer), packedFlags,
        SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName)()};
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
    std::array<CLodVirtualShadowClipmapInfo, CLodVirtualShadowMaxSupportedClipmapCount> clipmapInfos{};
    std::array<CLodVirtualShadowMarkClipmapData, CLodVirtualShadowMaxSupportedClipmapCount> markClipmapData{};
    std::array<CLodVirtualShadowCompactShadowCameraInfo, CLodVirtualShadowMaxSupportedClipmapCount> compactShadowCameras{};
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
                const int64_t pageOffsetX =
                    clipmapIndex < lightViewInfo.unwrappedPageOffsetX.size()
                    ? lightViewInfo.unwrappedPageOffsetX[clipmapIndex]
                    : 0;
                const int64_t pageOffsetY =
                    clipmapIndex < lightViewInfo.unwrappedPageOffsetY.size()
                    ? lightViewInfo.unwrappedPageOffsetY[clipmapIndex]
                    : 0;
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

    m_previousClipmapInfos = clipmapInfos;
    m_previousClipmapInfosValid = true;
    m_previousDirectionalLightDirection = currentDirectionalLightDirection;
    m_previousDirectionalLightDirectionValid = currentDirectionalLightDirectionValid;
    if (updateContext) {
        m_previousRenderResolution = updateContext->renderResolution;
        m_previousRenderResolutionValid = true;
    }

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



br::render::PreparedComputeDispatch VirtualShadowMapSetupPass::Prepare(
    const VirtualShadowMapSetupBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = GetVirtualShadowResolutionConfig();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const uint32_t packedConfig0 =
        ((config.pageTableResolution & CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_SHIFT) |
        ((CLodVirtualShadowMaxSupportedClipmapCount & CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_SHIFT) |
        ((config.maxPhysicalPages & CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_SHIFT);
    const uint32_t packedConfig1 = (CLodVirtualShadowDirtyWordCount(config.maxPhysicalPages) &
        CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_MASK) << CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_SHIFT;
    auto& c = data.constants;
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    c[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    c[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
    c[CLOD_VIRTUAL_SHADOW_SETUP_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = uav(bindings.allocationCount);
    c[CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyFlags);
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG0] = packedConfig0;
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG1] = packedConfig1;
    c[CLOD_VIRTUAL_SHADOW_SETUP_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    c[CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_INFO_DESCRIPTOR_INDEX] = uav(bindings.clipmapInfo);
    c[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_FLAGS] = bindings.packedFlags;
    c[CLOD_VIRTUAL_SHADOW_SETUP_MARK_CLIPMAP_DATA_DESCRIPTOR_INDEX] = uav(bindings.markClipmapData);
    c[CLOD_VIRTUAL_SHADOW_SETUP_RUNTIME_STATE_DESCRIPTOR_INDEX] = uav(bindings.runtimeState);
    c[CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_SCALE_AS_UINT] = std::bit_cast<uint32_t>(bindings.autoBiasScale);
    c[CLOD_VIRTUAL_SHADOW_SETUP_FALLBACK_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = uav(bindings.fallbackCandidateCount);
    data.groupsX = (config.pageTableResolution + 7u) / 8u;
    data.groupsY = data.groupsX;
    data.groupsZ = CLodVirtualShadowMaxSupportedClipmapCount;
    return data;
}

void VirtualShadowMapSetupPass::ShutdownPass() {}

void VirtualShadowMapSetupPass::Record(const VirtualShadowMapSetupBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
