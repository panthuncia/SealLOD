#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapSetupPass.h"

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
#include "Render/Runtime/UploadServiceAccess.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowSetupRootConstants.h"

namespace {

std::array<CLodVirtualShadowClipmapInfo, CLodVirtualShadowMaxSupportedClipmapCount> g_previousClipmapInfos{};
std::array<int64_t, CLodVirtualShadowMaxSupportedClipmapCount> g_previousClipmapPageOffsetX{};
std::array<int64_t, CLodVirtualShadowMaxSupportedClipmapCount> g_previousClipmapPageOffsetY{};
bool g_previousClipmapInfosValid = false;
DirectX::XMFLOAT3 g_previousDirectionalLightDirection{};
bool g_previousDirectionalLightDirectionValid = false;
DirectX::XMUINT2 g_previousRenderResolution{};
bool g_previousRenderResolutionValid = false;
uint32_t g_pendingRenderResolutionResetFrames = 0u;

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
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> allocationCountBuffer,
    std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<Buffer> markClipmapDataBuffer,
    std::shared_ptr<Buffer> compactMainCameraBuffer,
    std::shared_ptr<Buffer> compactShadowCameraBuffer,
    std::shared_ptr<Buffer> statsBuffer,
    std::shared_ptr<Buffer> runtimeStateBuffer,
    std::shared_ptr<Buffer> fallbackCandidateCountBuffer,
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

void VirtualShadowMapSetupPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithUnorderedAccess(
        m_clipmapInfoBuffer,
        m_pageTableTexture,
        m_pageMetadataBuffer,
        m_allocationCountBuffer,
        m_dirtyPageFlagsBuffer,
        m_markClipmapDataBuffer,
        m_compactMainCameraBuffer,
        m_compactShadowCameraBuffer,
        m_statsBuffer,
        m_runtimeStateBuffer,
        m_fallbackCandidateCountBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapSetupPass::Setup() {}

void VirtualShadowMapSetupPass::Update(const UpdateExecutionContext& executionContext)
{
    const bool disableVirtualShadowPageCaching =
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableVirtualShadowPageCachingSettingName)();
    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    const bool renderResolutionChanged =
        updateContext != nullptr &&
        g_previousRenderResolutionValid &&
        (g_previousRenderResolution.x != updateContext->renderResolution.x ||
            g_previousRenderResolution.y != updateContext->renderResolution.y);
    if (renderResolutionChanged) {
        // Virtual shadow page marking samples the primary linear-depth texture
        // before the current frame repopulates it, so a resolution change needs
        // one reset for the resize frame and one more once the new-size depth is valid.
        g_pendingRenderResolutionResetFrames = 2u;
    }
    const bool renderResolutionResetPending = g_pendingRenderResolutionResetFrames > 0u;
    const bool forceResetResources = m_forceResetResources || disableVirtualShadowPageCaching || renderResolutionResetPending;
    m_forceResetResources = false;
    m_feedbackRecoveryRefresh =
        g_clodVirtualShadowFeedbackRecoveryRequested.exchange(
            false,
            std::memory_order_acq_rel);
    if (renderResolutionResetPending) {
        --g_pendingRenderResolutionResetFrames;
    }
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = GetVirtualShadowResolutionConfig();
    const uint32_t virtualShadowResolution = virtualShadowConfig.virtualResolution;
    const uint32_t virtualShadowPageTableResolution = virtualShadowConfig.pageTableResolution;
    const uint32_t virtualShadowPhysicalPageCount = virtualShadowConfig.maxPhysicalPages;
    const uint32_t virtualShadowPhysicalAtlasPagesWide = virtualShadowConfig.physicalAtlasPagesWide;
    const uint32_t virtualShadowPhysicalAtlasPagesHigh = virtualShadowConfig.physicalAtlasPagesHigh;

    m_resetReasonForced = forceResetResources;
    m_resetReasonNoPreviousState = !g_previousClipmapInfosValid;
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

    if (updateContext && updateContext->viewManager) {
        bool foundPrimaryCamera = false;
        updateContext->viewManager->ForEachFiltered(ViewFilter::PrimaryCameras(), [&](uint64_t viewId) {
            if (foundPrimaryCamera) {
                return;
            }

            const View* view = updateContext->viewManager->Get(viewId);
            if (!view) {
                return;
            }

            foundPrimaryCamera = true;
            compactMainCamera.positionWorldSpace = view->cameraInfo.positionWorldSpace;
            compactMainCamera.viewInverse = view->cameraInfo.viewInverse;
            compactMainCamera.projectionInverse = view->cameraInfo.projectionInverse;
        });

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
        auto lightQuery = ecsWorld.query_builder<const Components::Light, const Components::LightViewInfo>().build();

        bool foundDirectionalShadow = false;
        lightQuery.each([&](flecs::entity, const Components::Light& light, const Components::LightViewInfo& lightViewInfo) {
            if (foundDirectionalShadow || !light.lightInfo.shadowCaster || light.type != Components::LightType::Directional) {
                return;
            }

            foundDirectionalShadow = true;
            DirectX::XMStoreFloat3(
                &currentDirectionalLightDirection,
                DirectX::XMVector3Normalize(light.lightInfo.dirWorldSpace));
            currentDirectionalLightDirectionValid = true;
            const uint32_t clipmapCount = std::min<uint32_t>(
                static_cast<uint32_t>(lightViewInfo.viewIDs.size()),
                CLodVirtualShadowMaxSupportedClipmapCount);
            activeClipmapCount = clipmapCount;
			const float configuredSkinnedShadowRadius = (std::max)(0.0f,
				SettingsManager::GetInstance().getSettingGetter<float>(CLodSkinnedShadowRadiusSettingName)());
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
                const View* view = updateContext->viewManager->Get(lightViewInfo.viewIDs[clipmapIndex]);
                if (!view) {
                    continue;
                }

                const float orthoWidth = ExtractOrthographicWidth(view->cameraInfo.unjitteredProjection);
                const float orthoHeight = ExtractOrthographicHeight(view->cameraInfo.unjitteredProjection);
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
                    clipmapIndex < lightViewInfo.virtualShadowUnwrappedPageOffsetX.size()
                    ? lightViewInfo.virtualShadowUnwrappedPageOffsetX[clipmapIndex]
                    : 0;
                const int64_t pageOffsetY =
                    clipmapIndex < lightViewInfo.virtualShadowUnwrappedPageOffsetY.size()
                    ? lightViewInfo.virtualShadowUnwrappedPageOffsetY[clipmapIndex]
                    : 0;
                clipmapInfo.worldOriginX = view->cameraInfo.positionWorldSpace.x;
                clipmapInfo.worldOriginY = view->cameraInfo.positionWorldSpace.y;
                clipmapInfo.worldOriginZ = view->cameraInfo.positionWorldSpace.z;
                clipmapInfo.texelWorldSize = std::max(orthoWidth, orthoHeight) / std::max(virtualShadowResolutionFloat, 1.0f);
                clipmapInfo.pageOffsetX = WrapPageOffset(pageOffsetX, virtualShadowPageTableResolution);
                clipmapInfo.pageOffsetY = WrapPageOffset(pageOffsetY, virtualShadowPageTableResolution);
                clipmapInfo.pageTableLayer = clipmapIndex;
                clipmapInfo.shadowCameraBufferIndex = view->gpu.cameraBufferIndex;
                clipmapInfo.clipLevel = clipmapIndex;
				const uint32_t previousDynamicFlag = g_previousClipmapInfosValid
					? g_previousClipmapInfos[clipmapIndex].flags & CLodVirtualShadowClipmapDynamicSkinnedFlag
					: 0u;
				const uint32_t currentDynamicFlag = dynamicSkinnedClipmap
					? CLodVirtualShadowClipmapDynamicSkinnedFlag
					: 0u;
				clipmapInfo.flags = CLodVirtualShadowClipmapValidFlag | currentDynamicFlag;
				if (g_previousClipmapInfosValid && previousDynamicFlag != currentDynamicFlag) {
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
                clipmapInfo.depthNear = view->cameraInfo.zNear;
                clipmapInfo.depthRange = std::max(
                    view->cameraInfo.zFar - view->cameraInfo.zNear,
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
                markData.shadowViewProjection = view->cameraInfo.viewProjection;

                compactShadowCameras[clipmapIndex].view = view->cameraInfo.view;
                compactShadowCameras[clipmapIndex].projection = view->cameraInfo.jitteredProjection;
                compactShadowCameras[clipmapIndex].viewProjection = view->cameraInfo.viewProjection;
                compactShadowCameras[clipmapIndex].isOrtho = view->cameraInfo.isOrtho;
                if (g_previousClipmapInfosValid && IsClipmapValid(g_previousClipmapInfos[clipmapIndex])) {
                    clipmapInfo.clearOffsetX = ClampClearOffset(
                        pageOffsetX - g_previousClipmapPageOffsetX[clipmapIndex],
                        virtualShadowPageTableResolution);
                    clipmapInfo.clearOffsetY = ClampClearOffset(
                        pageOffsetY - g_previousClipmapPageOffsetY[clipmapIndex],
                        virtualShadowPageTableResolution);
                }
                g_previousClipmapPageOffsetX[clipmapIndex] = pageOffsetX;
                g_previousClipmapPageOffsetY[clipmapIndex] = pageOffsetY;
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
        });
    }

    if (currentDirectionalLightDirectionValid &&
        g_previousDirectionalLightDirectionValid &&
        !NearlyEqualDirection(currentDirectionalLightDirection, g_previousDirectionalLightDirection)) {
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

        if (!m_resetResources && !ClipmapStructureEquals(info, g_previousClipmapInfos[clipmapIndex])) {
            m_resetReasonStructureMismatch = true;
            m_resetResources = true;
        }
    }

    g_previousClipmapInfos = clipmapInfos;
    g_previousClipmapInfosValid = true;
    g_previousDirectionalLightDirection = currentDirectionalLightDirection;
    g_previousDirectionalLightDirectionValid = currentDirectionalLightDirectionValid;
    if (updateContext) {
        g_previousRenderResolution = updateContext->renderResolution;
        g_previousRenderResolutionValid = true;
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
    BUFFER_UPLOAD(&runtimeState, sizeof(runtimeState), rg::runtime::UploadTarget::FromShared(m_runtimeStateBuffer), 0);

    BUFFER_UPLOAD(
        clipmapInfos.data(),
        static_cast<uint32_t>(clipmapInfos.size() * sizeof(CLodVirtualShadowClipmapInfo)),
        rg::runtime::UploadTarget::FromShared(m_clipmapInfoBuffer),
        0);

    BUFFER_UPLOAD(
        markClipmapData.data(),
        static_cast<uint32_t>(markClipmapData.size() * sizeof(CLodVirtualShadowMarkClipmapData)),
        rg::runtime::UploadTarget::FromShared(m_markClipmapDataBuffer),
        0);

    BUFFER_UPLOAD(
        &compactMainCamera,
        sizeof(compactMainCamera),
        rg::runtime::UploadTarget::FromShared(m_compactMainCameraBuffer),
        0);

    BUFFER_UPLOAD(
        compactShadowCameras.data(),
        static_cast<uint32_t>(compactShadowCameras.size() * sizeof(CLodVirtualShadowCompactShadowCameraInfo)),
        rg::runtime::UploadTarget::FromShared(m_compactShadowCameraBuffer),
        0);
}

PassReturn VirtualShadowMapSetupPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = GetVirtualShadowResolutionConfig();

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    const uint32_t packedConfig0 =
        ((virtualShadowConfig.pageTableResolution & CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_MASK)
            << CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION_SHIFT) |
        ((CLodVirtualShadowMaxSupportedClipmapCount & CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_MASK)
            << CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT_SHIFT) |
        ((virtualShadowConfig.maxPhysicalPages & CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_MASK)
            << CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT_SHIFT);
    const uint32_t packedConfig1 =
        (CLodVirtualShadowDirtyWordCount(virtualShadowConfig.maxPhysicalPages) & CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_MASK)
        << CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT_SHIFT;
    const uint32_t packedFlags =
        ((m_resetResources ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES_BIT) |
        ((m_resetReasonForced ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_FORCED_BIT) |
        ((m_resetReasonNoPreviousState ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_NO_PREVIOUS_STATE_BIT) |
        ((m_resetReasonStructureMismatch ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_STRUCTURE_MISMATCH_BIT) |
        ((m_resetReasonLightDirectionChanged ? 1u : 0u) << CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_LIGHT_DIRECTION_CHANGED_BIT) |
        ((SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName)() ? 1u : 0u)
            << CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_ENABLED_BIT) |
        ((m_feedbackRecoveryRefresh ? 1u : 0u)
            << CLOD_VIRTUAL_SHADOW_SETUP_FEEDBACK_RECOVERY_REFRESH_BIT);

    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_DESCRIPTOR_INDEX] = m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_METADATA_DESCRIPTOR_INDEX] = m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = m_allocationCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_FLAGS_DESCRIPTOR_INDEX] = m_dirtyPageFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG0] = packedConfig0;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_CONFIG1] = packedConfig1;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_clipmapInfoBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_PACKED_FLAGS] = packedFlags;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_MARK_CLIPMAP_DATA_DESCRIPTOR_INDEX] = m_markClipmapDataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_RUNTIME_STATE_DESCRIPTOR_INDEX] = m_runtimeStateBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_SCALE_AS_UINT] =
        std::bit_cast<uint32_t>(
            SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName)());
    rootConstants[CLOD_VIRTUAL_SHADOW_SETUP_FALLBACK_CANDIDATE_COUNT_DESCRIPTOR_INDEX] =
        m_fallbackCandidateCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    constexpr uint32_t kThreadsPerDimension = 8u;
    const uint32_t groupCountX = (virtualShadowConfig.pageTableResolution + kThreadsPerDimension - 1u) / kThreadsPerDimension;
    const uint32_t groupCountY = (virtualShadowConfig.pageTableResolution + kThreadsPerDimension - 1u) / kThreadsPerDimension;
    commandList.Dispatch(groupCountX, groupCountY, CLodVirtualShadowMaxSupportedClipmapCount);

    return {};
}

void VirtualShadowMapSetupPass::Cleanup() {}
