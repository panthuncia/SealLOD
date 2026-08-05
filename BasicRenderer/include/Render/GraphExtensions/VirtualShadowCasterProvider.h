#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Render/RenderGraph/RenderGraph.h"

class Buffer;
class PixelBuffer;

enum class VirtualShadowCasterMobility : uint8_t {
    Rigid,
    SkinnedOrDeformable,
};

inline constexpr bool VirtualShadowCasterUsesDynamicLayer(
    VirtualShadowCasterMobility mobility,
    uint32_t clipmapFlags,
    uint32_t dynamicSkinnedFlag = 0x4u) noexcept
{
    return mobility == VirtualShadowCasterMobility::SkinnedOrDeformable &&
        (clipmapFlags & dynamicSkinnedFlag) != 0u;
}

struct VirtualShadowInvalidationBounds {
    std::array<float, 3> center{};
    float radius = 0.0f;
    VirtualShadowCasterMobility mobility = VirtualShadowCasterMobility::Rigid;
    uint32_t clipmapMask = 0xFFFFFFFFu;
    uint32_t providerLabel = 0u;
};

struct VirtualShadowInvalidationBatch {
    std::vector<VirtualShadowInvalidationBounds> bounds;
    bool invalidateAllActiveClipmaps = false;
};

class VirtualShadowInvalidationQueue {
public:
    static constexpr uint32_t DefaultCapacity = 65536u;

    explicit VirtualShadowInvalidationQueue(uint32_t capacity = DefaultCapacity);
    bool Enqueue(const VirtualShadowInvalidationBounds& bounds);
    VirtualShadowInvalidationBatch Drain(uint32_t dynamicSkinnedClipmapCount);
    uint64_t GetOverflowCount() const;

private:
    const uint32_t m_capacity;
    mutable std::mutex m_mutex;
    std::vector<VirtualShadowInvalidationBounds> m_pending;
    bool m_overflowed = false;
    uint64_t m_overflowCount = 0u;
};

struct VirtualShadowCasterBuildContext {
    std::shared_ptr<PixelBuffer> pageTable;
    std::shared_ptr<PixelBuffer> staticPhysicalPages;
    std::shared_ptr<PixelBuffer> dynamicPhysicalPages;
    std::shared_ptr<Buffer> clipmapInfo;
    std::shared_ptr<Buffer> compactShadowCameras;
    std::shared_ptr<Buffer> staticActiveBlockMetadata;
    std::shared_ptr<Buffer> dynamicActiveBlockMetadata;
    std::shared_ptr<Buffer> directionalPageViews;
    std::shared_ptr<Buffer> statistics;
    std::shared_ptr<VirtualShadowInvalidationQueue> invalidationQueue;
};

// Optional graph-resource metadata for provider-owned uint counter buffers.
// The renderer reads these after completionPassName while VSM telemetry is enabled.
struct VirtualShadowCasterTelemetryTag {
    std::string providerId;
    std::string completionPassName;
};

class VirtualShadowPassBuilder {
public:
    VirtualShadowPassBuilder(
        std::vector<RenderGraph::ExternalPassDesc>& passes,
        std::string afterPass,
        std::string beforePass = {});

    void Add(RenderGraph::ExternalPassDesc pass);
    const std::string& LastPassName() const;

private:
    std::vector<RenderGraph::ExternalPassDesc>& m_passes;
    std::string m_lastPass;
    std::string m_beforePass;
};

class IVirtualShadowCasterProvider {
public:
    virtual ~IVirtualShadowCasterProvider() = default;
    virtual std::string_view GetVirtualShadowCasterProviderId() const noexcept = 0;
    // Providers with continuously deforming geometry can extend the dynamic
    // VSM region beyond the renderer's skeletal-animation radius. Returning
    // zero leaves the global policy unchanged.
    virtual float GetRequestedDynamicShadowRadius() const { return 0.0f; }
    virtual void OnVirtualShadowCasterRegistered(
        const std::shared_ptr<VirtualShadowInvalidationQueue>& queue) { (void)queue; }
    virtual void GatherVirtualShadowPreparationPasses(
        const VirtualShadowCasterBuildContext& context,
        VirtualShadowPassBuilder& builder) = 0;
    virtual void GatherVirtualShadowRasterPasses(
        const VirtualShadowCasterBuildContext& context,
        VirtualShadowPassBuilder& builder) = 0;
};

class VirtualShadowCasterRegistry {
public:
    VirtualShadowCasterRegistry();
    void Register(IVirtualShadowCasterProvider& provider);
    bool Empty() const noexcept;
    size_t Size() const noexcept;
    float GetRequestedDynamicShadowRadius() const;
    std::shared_ptr<VirtualShadowInvalidationQueue> GetInvalidationQueue() const;
    void GatherPreparationPasses(
        const VirtualShadowCasterBuildContext& context,
        VirtualShadowPassBuilder& builder) const;
    void GatherRasterPasses(
        const VirtualShadowCasterBuildContext& context,
        VirtualShadowPassBuilder& builder) const;

private:
    std::vector<IVirtualShadowCasterProvider*> m_providers;
    std::unordered_map<std::string, IVirtualShadowCasterProvider*> m_providerIds;
    std::shared_ptr<VirtualShadowInvalidationQueue> m_invalidationQueue;
};
