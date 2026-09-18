#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <flecs.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Render/ShadowViewService.h"
#include "Render/ViewStateArtifacts.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Scene/Components.h"
#include "ShaderBuffers.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"

namespace org { class ResourceGroup; }
using org::ResourceGroup;
namespace org { class PixelBuffer; }
using org::PixelBuffer;
namespace org { class DynamicGloballyIndexedResource; }
using org::DynamicGloballyIndexedResource;

// Flags describing purpose/type of a view
struct ViewFlags {
    bool primaryCamera : 1;
    bool shadow : 1;
    bool reflection : 1;
    bool probe : 1;
    bool cascaded : 1;

    static ViewFlags PrimaryCamera() { return { true, false, false, false, false }; }
    static ViewFlags ShadowCascade(bool cascadedFlag = true) { return { false, true, false, false, cascadedFlag }; }
    static ViewFlags ShadowFace() { return { false, true, false, false, false }; }
    static ViewFlags Generic() { return { false, false, false, false, false }; }
};

// Optional creation customization
struct ViewCreationParams {
    // Provide existing depth resources if already created externally
    std::shared_ptr<PixelBuffer> depthMap;
    std::shared_ptr<PixelBuffer> linearDepthMap;

    // Link to ECS entity that owns this view (camera or light)
    uint64_t parentEntityID = 0;

    // Light meta
    Components::LightType lightType = Components::LightType::Directional;
    int cascadeIndex = -1;
};

struct ViewResources {
    std::shared_ptr<BufferView> cameraBufferView;
	std::shared_ptr<BufferView> cullingCameraBufferView;
    uint32_t cameraBufferIndex = 0;

	std::shared_ptr<PixelBuffer> depthMap = nullptr;
    std::shared_ptr<PixelBuffer> linearDepthMap = nullptr;
    std::shared_ptr<PixelBuffer> visibilityBuffer = nullptr;
    std::shared_ptr<PixelBuffer> clodDeepVisibilityHeadPointers = nullptr;
    // Descriptor indices are deliberately not cached here: the render graph
    // gives a resource new slots whenever it realizes it on a new backing.
};

struct View {
    uint64_t id = 0;
    CameraInfo cameraInfo{};
    ViewFlags flags = ViewFlags::Generic();

    Components::LightType lightType = Components::LightType::Directional;
    int cascadeIndex = -1;
    uint64_t parentEntityID = 0;

    ViewResources gpu;
};

// Filtering helper for iteration
struct ViewFilter {
    bool requirePrimary = false;
    bool requireShadow = false;
    bool requireCascade = false;
    bool requireLightType = false;
    Components::LightType lightType;

    static ViewFilter PrimaryCameras() {
        ViewFilter filter;
        filter.requirePrimary = true;
        return filter;
	}

    static ViewFilter Shadows() {
        ViewFilter filter;
        filter.requireShadow = true;
        return filter;
	}

    bool Match(const View& v) const {
        if (requirePrimary && !v.flags.primaryCamera) return false;
        if (requireShadow && !v.flags.shadow) return false;
        if (requireCascade && !v.flags.cascaded) return false;
        if (requireLightType && v.lightType != lightType) return false;
        return true;
    }
};

// Optional callbacks
struct ViewEvents {
    std::function<void(const View&)> onCreated;
    std::function<void(uint64_t)>    onDestroyed;
    std::function<void(const View&)> onCameraUpdated;
    std::function<void(const View&)> onDepthAttached;
	std::function<void(const View&)> onVisibilityBufferAttached;
};

class ViewManager : public IResourceProvider,
                    public br::render::IShadowViewService {
public:
    static std::unique_ptr<ViewManager> CreateUnique() {
        return std::unique_ptr<ViewManager>(new ViewManager());
    }
    static std::shared_ptr<ViewManager> CreateShared() {
        return std::shared_ptr<ViewManager>(new ViewManager());
    }
    ~ViewManager();

    // Create a new view (camera or light), returns viewID
    uint64_t CreateView(const CameraInfo& cameraInfo,
        const ViewFlags& flags,
        const ViewCreationParams& params = {});

    // Destroy view and unregister indirect buffers
    void DestroyView(uint64_t viewID);

    // Attach (or replace) depth resources post creation
    void AttachDepth(uint64_t viewID,
        std::shared_ptr<PixelBuffer> depth,
        std::shared_ptr<PixelBuffer> linearDepth);

	void AttachVisibilityBuffer(uint64_t viewID, std::shared_ptr<PixelBuffer> visibilityBuffer);
    std::shared_ptr<PixelBuffer> EnsureCLodDeepVisibilityHeadPointers(uint64_t viewID);

    // Update camera matrices/params
    void UpdateCamera(uint64_t viewID, const CameraInfo& cameraInfo);
    br::render::PrimaryCameraFrameUpload CapturePrimaryCameraUpload(
        std::uint64_t frameNumber) const;

    uint64_t CreateShadowView(const CameraInfo& cameraInfo,
        const ViewFlags& flags, const ViewCreationParams& params) override {
        return CreateView(cameraInfo, flags, params);
    }
    void UpdateShadowView(uint64_t viewID, const CameraInfo& cameraInfo) override {
        UpdateCamera(viewID, cameraInfo);
    }
    void DestroyShadowView(uint64_t viewID) override { DestroyView(viewID); }
    uint32_t ShadowViewCameraBufferIndex(uint64_t viewID) const override;

	uint32_t GetCameraBufferSize() const { return static_cast<uint32_t>(m_cameraBuffer->Size()); }
    std::shared_ptr<Resource> GetCameraBuffer() const { return m_cameraBuffer; }
    std::shared_ptr<Resource> GetCullingCameraBuffer() const { return m_cullingCameraBuffer; }
    std::shared_ptr<const std::vector<std::byte>> CaptureCameraTableImage() const {
        return std::make_shared<const std::vector<std::byte>>(m_cameraBuffer->CaptureCpuShadowBytes());
    }
    std::shared_ptr<const std::vector<std::byte>> CaptureCullingCameraTableImage() const {
        return std::make_shared<const std::vector<std::byte>>(m_cullingCameraBuffer->CaptureCpuShadowBytes());
    }
    uint64_t GetResourceLayoutRevision() const { return m_resourceLayoutRevision; }
    uint64_t GetPublicationRevision() const noexcept {
        return m_publicationRevision.load(std::memory_order_acquire);
    }

    // Access
    View* Get(uint64_t viewID);
    const View* Get(uint64_t viewID) const;

    // Iteration
    template<class F>
    void ForEachView(F&& f) {
        for (auto& [_, v] : m_views)
            std::forward<F>(f)(v.id);
    }

    template<class F>
    void ForEachFiltered(const ViewFilter& filter, F&& f) {
        for (auto& [_, v] : m_views)
            if (filter.Match(v))
                std::forward<F>(f)(v.id);
    }


    // Events
    void SetEvents(ViewEvents events) { m_events = events; }

    // IResourceProvider
    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;
    std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
    std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;
private:
    ViewManager();

    std::unordered_map<uint64_t, View> m_views;
    std::atomic<uint64_t> m_nextViewID{ 1 };


    // Core buffers/groups
    std::shared_ptr<LazyDynamicStructuredBuffer<CameraInfo>> m_cameraBuffer;
	std::shared_ptr<LazyDynamicStructuredBuffer<CullingCameraInfo>> m_cullingCameraBuffer;

    std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
    std::unordered_map<ResourceIdentifier, std::shared_ptr<IResourceResolver>, ResourceIdentifier::Hasher> m_resolvers;

    std::shared_ptr<ResourceGroup> m_linearDepthGroup;

    uint64_t m_resourceLayoutRevision = 1u;
    std::atomic_uint64_t m_publicationRevision{1};
    std::atomic_uint64_t m_primaryCameraRevision{1};
    std::uint64_t m_primaryViewID = 0;
    std::shared_ptr<BufferView> m_primaryCameraBufferView;
    std::shared_ptr<BufferView> m_primaryCullingCameraBufferView;

    mutable std::mutex m_cameraUpdateMutex;
    ViewEvents m_events;
};
