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
#include "Render/ViewStateArtifacts.h"

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
    auto& resourceManager = ::ResourceManager::GetInstance();
    m_cameraBuffer = LazyDynamicStructuredBuffer<CameraInfo>::CreateShared(1, "cameraBuffer<ViewManager>");
    m_cullingCameraBuffer = LazyDynamicStructuredBuffer<CullingCameraInfo>::CreateShared(1, "cullingCameraBuffer<ViewManager>");
    m_primaryCameraBufferView = m_cameraBuffer->Add();
    m_primaryCullingCameraBufferView = m_cullingCameraBuffer->Add();
    org::memory::SetResourceUsageHint(*m_cameraBuffer, "Camera and view buffers");
	org::memory::SetResourceUsageHint(*m_cullingCameraBuffer, "Camera and view buffers");
    m_linearDepthGroup = std::make_shared<ResourceGroup>("LinearDepthMaps");

    // Register provided resources
    m_resources[Builtin::CameraBuffer] = m_cameraBuffer;
	m_resources[Builtin::CullingCameraBuffer] = m_cullingCameraBuffer;
    m_resolvers[Builtin::LinearDepthMaps] =
        std::make_shared<ResourceGroupResolver>(m_linearDepthGroup);
    // History is the last submitted contents of the persistent linear-depth
    // resources. Keep the shader-facing legacy name as an alias while
    // DepthHistoryPublicationService owns which producer is valid.
    m_resolvers[Builtin::LastFrameLinearDepthMaps] =
        std::make_shared<ResourceGroupResolver>(m_linearDepthGroup);
}

ViewManager::~ViewManager() = default;

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

    // The primary camera owns the permanently reserved first table element.
    if (flags.primaryCamera) {
        if (m_primaryViewID != 0)
            throw std::logic_error("ViewManager supports exactly one primary camera view");
        m_primaryViewID = id;
        v.gpu.cameraBufferView = m_primaryCameraBufferView;
        v.gpu.cullingCameraBufferView = m_primaryCullingCameraBufferView;
    } else {
        v.gpu.cameraBufferView = m_cameraBuffer->Add();
        v.gpu.cullingCameraBufferView = m_cullingCameraBuffer->Add();
    }
    v.gpu.cameraBufferIndex = static_cast<uint32_t>(v.gpu.cameraBufferView->GetOffset() / sizeof(CameraInfo));
    m_cameraBuffer->UpdateView(v.gpu.cameraBufferView.get(), &cameraInfo);

    CullingCameraInfo cullCam = BuildCullingCameraInfo(cameraInfo);

	m_cullingCameraBuffer->UpdateView(v.gpu.cullingCameraBufferView.get(), &cullCam);

    // Depth (optional)
    v.gpu.depthMap = params.depthMap;
    v.gpu.linearDepthMap = params.linearDepthMap;

    m_views.emplace(id, std::move(v));
    ++m_resourceLayoutRevision;
    m_publicationRevision.fetch_add(1, std::memory_order_release);

    if (m_events.onCreated) m_events.onCreated(m_views[id]);
    return id;
}

void ViewManager::DestroyView(uint64_t viewID) {
    auto it = m_views.find(viewID);
    if (it == m_views.end()) return;

    auto& v = it->second;

    // Camera buffer view
    if (v.flags.primaryCamera) {
        m_primaryViewID = 0;
    } else {
        m_cameraBuffer->Remove(v.gpu.cameraBufferView.get());
	    m_cullingCameraBuffer->Remove(v.gpu.cullingCameraBufferView.get());
    }

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
            m_linearDepthGroup->RemoveResource(v.gpu.linearDepthMap.get());
        }
    }

    m_views.erase(it);
    ++m_resourceLayoutRevision;
    m_publicationRevision.fetch_add(1, std::memory_order_release);
    if (m_events.onDestroyed) m_events.onDestroyed(viewID);
}

void ViewManager::AttachDepth(uint64_t viewID,
    std::shared_ptr<PixelBuffer> depth,
    std::shared_ptr<PixelBuffer> linearDepth) {
    auto* v = Get(viewID);
    if (!v) return;
    const auto previousLinearDepth = v->gpu.linearDepthMap;
    if (previousLinearDepth && previousLinearDepth != linearDepth) {
        const uint64_t previousSourceID = previousLinearDepth->GetGlobalResourceID();
        const bool stillReferenced = std::any_of(
            m_views.begin(),
            m_views.end(),
            [viewID, previousSourceID](const auto& entry) {
                return entry.first != viewID && entry.second.gpu.linearDepthMap &&
                    entry.second.gpu.linearDepthMap->GetGlobalResourceID() == previousSourceID;
            });
        if (!stillReferenced) {
            m_linearDepthGroup->RemoveResource(previousLinearDepth.get());
        }
    }
    v->gpu.depthMap = depth;
    v->gpu.linearDepthMap = linearDepth;
    if (linearDepth) {
        m_linearDepthGroup->AddResource(linearDepth);
    }

    if (m_events.onDepthAttached) {
        m_events.onDepthAttached(*v);
    }
    ++m_resourceLayoutRevision;
    m_publicationRevision.fetch_add(1, std::memory_order_release);
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
    m_publicationRevision.fetch_add(1, std::memory_order_release);
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
    ++m_resourceLayoutRevision;
    m_publicationRevision.fetch_add(1, std::memory_order_release);
    return v->gpu.clodDeepVisibilityHeadPointers;
}

void ViewManager::UpdateCamera(uint64_t viewID, const CameraInfo& cameraInfo) {
    auto* v = Get(viewID);
    if (!v) return;
    std::lock_guard<std::mutex> lock(m_cameraUpdateMutex);
    const bool depthSliceChanged = v->cameraInfo.depthBufferArrayIndex != cameraInfo.depthBufferArrayIndex;
    v->cameraInfo = cameraInfo;
    if (!v->flags.primaryCamera) {
        m_cameraBuffer->UpdateView(v->gpu.cameraBufferView.get(), &cameraInfo);
	    CullingCameraInfo cullInfo = BuildCullingCameraInfo(cameraInfo);
	    m_cullingCameraBuffer->UpdateView(v->gpu.cullingCameraBufferView.get(), &cullInfo);
        m_publicationRevision.fetch_add(1, std::memory_order_release);
    } else {
        m_primaryCameraRevision.fetch_add(1, std::memory_order_release);
    }
    if (depthSliceChanged) {
        ++m_resourceLayoutRevision;
    }
    
    if (m_events.onCameraUpdated) {
        m_events.onCameraUpdated(*v);
    }
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

br::render::PrimaryCameraFrameUpload ViewManager::CapturePrimaryCameraUpload(
    std::uint64_t frameNumber) const {
    std::lock_guard<std::mutex> lock(m_cameraUpdateMutex);
    const auto* view = Get(m_primaryViewID);
    if (!view) return {};
    return {
        .camera = view->cameraInfo,
        .cullingCamera = BuildCullingCameraInfo(view->cameraInfo),
        .viewID = view->id,
        .revision = m_primaryCameraRevision.load(std::memory_order_acquire),
        .frameNumber = frameNumber,
        .cameraBufferIndex = 0,
    };
}

uint32_t ViewManager::ShadowViewCameraBufferIndex(uint64_t viewID) const {
    const auto* view = Get(viewID);
    return view ? view->gpu.cameraBufferIndex : 0xFFFFFFFFu;
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
