#include "Managers/ViewManager.h"

#include <algorithm>
#include <limits>
#include <string>

#include "Managers/Singletons/ResourceManager.h"
#include "Managers/IndirectCommandBufferManager.h"
#include "Resources/ResourceGroup.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "../../generated/BuiltinResources.h"
#include "Resources/DynamicResource.h"
#include "Resources/MemoryStatisticsComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"

namespace
{
    constexpr float kClusterLodErrorPixels = 1.0f;

    float ComputeErrorOverDistanceThreshold(const CameraInfo& cameraInfo, float errorPixels)
    {
        const float projY = DirectX::XMVectorGetY(cameraInfo.jitteredProjection.r[1]);
        const float screenHeight = static_cast<float>(cameraInfo.lodResY != 0u ? cameraInfo.lodResY : cameraInfo.depthResY);
        const float denom = (projY * 0.5f) * screenHeight;
        if (denom <= 0.0f) {
            return std::numeric_limits<float>::max();
        }
        return errorPixels / denom;
    }

    TextureDescription CreateCLodDeepVisibilityHeadPointerDesc(const PixelBuffer& visibilityBuffer)
    {
        TextureDescription desc;
        ImageDimensions dims;
        dims.width = visibilityBuffer.GetWidth();
        dims.height = visibilityBuffer.GetHeight();
        desc.imageDimensions.push_back(dims);
        desc.format = rhi::Format::R32_UInt;
        desc.channels = 1;
        desc.hasSRV = true;
        desc.srvFormat = rhi::Format::R32_UInt;
        desc.hasUAV = true;
        desc.uavFormat = rhi::Format::R32_UInt;
        desc.hasNonShaderVisibleUAV = true;
        desc.generateMipMaps = false;
        return desc;
    }

    bool CLodHeadPointerMatchesVisibility(const PixelBuffer& headPointers, const PixelBuffer& visibilityBuffer)
    {
        return headPointers.GetWidth() == visibilityBuffer.GetWidth() &&
            headPointers.GetHeight() == visibilityBuffer.GetHeight();
    }

    CullingCameraInfo BuildCullingCameraInfo(const CameraInfo& cameraInfo)
    {
        CullingCameraInfo cullInfo{};
        cullInfo.positionWorldSpace = cameraInfo.positionWorldSpace;
        cullInfo.projX = DirectX::XMVectorGetX(cameraInfo.jitteredProjection.r[0]);
        cullInfo.projY = DirectX::XMVectorGetY(cameraInfo.jitteredProjection.r[1]);
        cullInfo.zNear = cameraInfo.zNear;
        cullInfo.errorOverDistanceThreshold = ComputeErrorOverDistanceThreshold(cameraInfo, kClusterLodErrorPixels);
        cullInfo.isOrtho = cameraInfo.isOrtho;
        cullInfo.viewportWidth = static_cast<float>(cameraInfo.depthResX);
        cullInfo.viewportHeight = static_cast<float>(cameraInfo.depthResY);
        cullInfo.reyesDiceRatePixels = CLodReyesDiceRatePixels();
        DirectX::XMStoreFloat4(&cullInfo.viewRightWorld, DirectX::XMVectorSetW(cameraInfo.viewInverse.r[0], 0.0f));
        DirectX::XMStoreFloat4(&cullInfo.viewUpWorld, DirectX::XMVectorSetW(cameraInfo.viewInverse.r[1], 0.0f));
        DirectX::XMStoreFloat4(&cullInfo.viewForwardWorld, DirectX::XMVectorSetW(DirectX::XMVectorNegate(cameraInfo.viewInverse.r[2]), 0.0f));
        cullInfo.viewProjection = cameraInfo.viewProjection;
        cullInfo.viewInverse = cameraInfo.viewInverse;
        cullInfo.projectionInverse = cameraInfo.projectionInverse;
        cullInfo.viewZ = {
            DirectX::XMVectorGetZ(cameraInfo.view.r[0]),
            DirectX::XMVectorGetZ(cameraInfo.view.r[1]),
            DirectX::XMVectorGetZ(cameraInfo.view.r[2]),
            DirectX::XMVectorGetZ(cameraInfo.view.r[3])
        };
        return cullInfo;
    }
}

ViewManager::ViewManager() {
    auto& resourceManager = ResourceManager::GetInstance();
    m_cameraBuffer = LazyDynamicStructuredBuffer<CameraInfo>::CreateShared(1, "cameraBuffer<ViewManager>");
	m_cullingCameraBuffer = LazyDynamicStructuredBuffer<CullingCameraInfo>::CreateShared(1, "cullingCameraBuffer<ViewManager>");
    rg::memory::SetResourceUsageHint(*m_cameraBuffer, "Camera and view buffers");
	rg::memory::SetResourceUsageHint(*m_cullingCameraBuffer, "Camera and view buffers");
    m_lastFrameLinearDepthGroup = std::make_shared<ResourceGroup>("LastFrameLinearDepthMaps");

    // Register provided resources
    m_resources[Builtin::CameraBuffer] = m_cameraBuffer;
	m_resources[Builtin::CullingCameraBuffer] = m_cullingCameraBuffer;
    m_resolvers[Builtin::LastFrameLinearDepthMaps] =
        std::make_shared<ResourceGroupResolver>(m_lastFrameLinearDepthGroup);
}

void ViewManager::SetIndirectCommandBufferManager(IndirectCommandBufferManager* manager) {
    m_indirectManager = manager;
}

uint64_t ViewManager::CreateView(const CameraInfo& cameraInfo,
    const ViewFlags& flags,
    const ViewCreationParams& params) {

    uint64_t id = m_nextViewID.fetch_add(1);

    View v;
    v.id = id;
    v.cameraInfo = cameraInfo;
    v.flags = flags;
    v.lightType = params.lightType;
    v.cascadeIndex = params.cascadeIndex;
    v.parentEntityID = params.parentEntityID;

    // Indirect buffers
    if (m_indirectManager) {
        m_indirectManager->CreateBuffersForView(id);
    }

    // Camera buffer view
    v.gpu.cameraBufferView = m_cameraBuffer->Add();
    v.gpu.cameraBufferIndex = static_cast<uint32_t>(v.gpu.cameraBufferView->GetOffset() / sizeof(CameraInfo));
    m_cameraBuffer->UpdateView(v.gpu.cameraBufferView.get(), &cameraInfo);

    CullingCameraInfo cullCam = BuildCullingCameraInfo(cameraInfo);

	v.gpu.cullingCameraBufferView = m_cullingCameraBuffer->Add();
	m_cullingCameraBuffer->UpdateView(v.gpu.cullingCameraBufferView.get(), &cullCam);

    // Depth (optional)
    v.gpu.depthMap = params.depthMap;
    v.gpu.linearDepthMap = params.linearDepthMap;

    m_views.emplace(id, std::move(v));
    ++m_resourceLayoutRevision;

    if (m_events.onCreated) m_events.onCreated(m_views[id]);
    return id;
}

void ViewManager::DestroyView(uint64_t viewID) {
    auto it = m_views.find(viewID);
    if (it == m_views.end()) return;

    auto& v = it->second;

    // Remove indirect buffers
    if (m_indirectManager) {
        m_indirectManager->UnregisterBuffers(viewID);
    }

    // Camera buffer view
    m_cameraBuffer->Remove(v.gpu.cameraBufferView.get());
	m_cullingCameraBuffer->Remove(v.gpu.cullingCameraBufferView.get());

    if (v.gpu.linearDepthMap) {
        const uint64_t sourceID = v.gpu.linearDepthMap->GetGlobalResourceID();
        bool stillReferenced = false;
        for (const auto& [otherViewID, otherView] : m_views) {
            if (otherViewID == viewID || !otherView.gpu.linearDepthMap) {
                continue;
            }
            if (otherView.gpu.linearDepthMap->GetGlobalResourceID() == sourceID) {
                stillReferenced = true;
                break;
            }
        }

        if (!stillReferenced) {
            auto it = m_lastFrameLinearDepthBySource.find(sourceID);
            if (it != m_lastFrameLinearDepthBySource.end()) {
                m_lastFrameLinearDepthGroup->RemoveResource(it->second.get());
                m_lastFrameLinearDepthBySource.erase(it);
            }
        }
    }

    m_views.erase(it);
    ++m_resourceLayoutRevision;
    if (m_events.onDestroyed) m_events.onDestroyed(viewID);
}

void ViewManager::AttachDepth(uint64_t viewID,
    std::shared_ptr<PixelBuffer> depth,
    std::shared_ptr<PixelBuffer> linearDepth) {
    auto* v = Get(viewID);
    if (!v) return;
    v->gpu.depthMap = depth;
    v->gpu.linearDepthMap = linearDepth;
    v->gpu.lastFrameLinearDepthMap.reset();
    v->gpu.lastFrameLinearDepthValid = false;

    if (linearDepth) {
        const uint64_t sourceID = linearDepth->GetGlobalResourceID();
        auto it = m_lastFrameLinearDepthBySource.find(sourceID);
        if (it == m_lastFrameLinearDepthBySource.end()) {
            auto desc = linearDepth->GetDescription();
            auto history = PixelBuffer::CreateShared(desc);
            history->SetName("Last Frame Linear Depth");
            m_lastFrameLinearDepthBySource[sourceID] = history;
            m_lastFrameLinearDepthGroup->AddResource(history);
            v->gpu.lastFrameLinearDepthMap = history;
        }
        else {
            v->gpu.lastFrameLinearDepthMap = it->second;
        }
    }

    if (m_events.onDepthAttached) {
        m_events.onDepthAttached(*v);
    }
    ++m_resourceLayoutRevision;
}

void ViewManager::AttachVisibilityBuffer(uint64_t viewID, std::shared_ptr<PixelBuffer> visibilityBuffer) {
    auto* v = Get(viewID);
    if (!v) return;
    v->gpu.visibilityBuffer = visibilityBuffer;
    v->gpu.clodDeepVisibilityHeadPointers.reset();
    if (m_events.onVisibilityBufferAttached) {
        m_events.onVisibilityBufferAttached(*v);
    }
    ++m_resourceLayoutRevision;
}

std::shared_ptr<PixelBuffer> ViewManager::EnsureCLodDeepVisibilityHeadPointers(uint64_t viewID)
{
    auto* v = Get(viewID);
    if (!v || !v->gpu.visibilityBuffer) {
        return nullptr;
    }

    const bool needsCreate =
        !v->gpu.clodDeepVisibilityHeadPointers ||
        !CLodHeadPointerMatchesVisibility(*v->gpu.clodDeepVisibilityHeadPointers, *v->gpu.visibilityBuffer);

    if (!needsCreate) {
        v->gpu.clodDeepVisibilityHeadPointers->EnsureVirtualDescriptorSlotsAllocated();
        return v->gpu.clodDeepVisibilityHeadPointers;
    }

    auto headPointerTexture = PixelBuffer::CreateSharedUnmaterialized(
        CreateCLodDeepVisibilityHeadPointerDesc(*v->gpu.visibilityBuffer));
    headPointerTexture->SetName("CLod Deep Visibility Head Pointers " + std::to_string(viewID));
    // These textures are created outside of RenderGraph::AddResource(), but later update code
    // immediately queries bindless UAV indices from them. Reserve descriptor slots up front so
    // GetUAVShaderVisibleInfo()/GetUAVNonShaderVisibleInfo() are valid before graph materialization.
    headPointerTexture->EnsureVirtualDescriptorSlotsAllocated();
    v->gpu.clodDeepVisibilityHeadPointers = std::move(headPointerTexture);
    return v->gpu.clodDeepVisibilityHeadPointers;
}

void ViewManager::UpdateCamera(uint64_t viewID, const CameraInfo& cameraInfo) {
    auto* v = Get(viewID);
    if (!v) return;
    std::lock_guard<std::mutex> lock(m_cameraUpdateMutex);
    const bool depthSliceChanged = v->cameraInfo.depthBufferArrayIndex != cameraInfo.depthBufferArrayIndex;
    v->cameraInfo = cameraInfo;
    m_cameraBuffer->UpdateView(v->gpu.cameraBufferView.get(), &cameraInfo);
	CullingCameraInfo cullInfo = BuildCullingCameraInfo(cameraInfo);
	m_cullingCameraBuffer->UpdateView(v->gpu.cullingCameraBufferView.get(), &cullInfo);
    if (depthSliceChanged) {
        ++m_resourceLayoutRevision;
    }
    
    if (m_events.onCameraUpdated) {
        m_events.onCameraUpdated(*v);
    }
}

void ViewManager::MarkDepthHistoryValid(uint64_t viewID) {
    auto* v = Get(viewID);
    if (!v || v->gpu.lastFrameLinearDepthValid) {
        return;
    }

    v->gpu.lastFrameLinearDepthValid = true;
    ++m_resourceLayoutRevision;
}

View* ViewManager::Get(uint64_t viewID) {
    auto it = m_views.find(viewID);
    if (it == m_views.end()) return nullptr;
    return &it->second;
}

const View* ViewManager::Get(uint64_t viewID) const {
    auto it = m_views.find(viewID);
    if (it == m_views.end()) return nullptr;
    return &it->second;
}

std::shared_ptr<Resource> ViewManager::ProvideResource(ResourceIdentifier const& key) {
    auto it = m_resources.find(key);
    if (it == m_resources.end()) return nullptr;
    return it->second;
}

std::vector<ResourceIdentifier> ViewManager::GetSupportedKeys() {
    std::vector<ResourceIdentifier> keys;
    keys.reserve(m_resources.size());
    for (auto const& [k, _] : m_resources)
        keys.push_back(k);
    return keys;
}

std::vector<ResourceIdentifier> ViewManager::GetSupportedResolverKeys() {
    std::vector<ResourceIdentifier> keys;
    keys.reserve(m_resolvers.size());
    for (auto const& [k, _] : m_resolvers)
        keys.push_back(k);
	return keys;
}
std::shared_ptr<IResourceResolver> ViewManager::ProvideResolver(ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it == m_resolvers.end()) return nullptr;
	return it->second;
}
