#include "Render/GraphExtensions/VirtualShadowCasterProvider.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"

VirtualShadowInvalidationQueue::VirtualShadowInvalidationQueue(uint32_t capacity)
    : m_capacity(std::max(1u, capacity))
{
    m_pending.reserve(std::min(m_capacity, 4096u));
}

bool VirtualShadowInvalidationQueue::Enqueue(const VirtualShadowInvalidationBounds& bounds)
{
    if (bounds.radius < 0.0f) {
        return false;
    }
    std::scoped_lock lock(m_mutex);
    if (m_pending.size() >= m_capacity) {
        m_overflowed = true;
        ++m_overflowCount;
        return false;
    }
    m_pending.push_back(bounds);
    return true;
}

VirtualShadowInvalidationBatch VirtualShadowInvalidationQueue::Drain(uint32_t dynamicSkinnedClipmapCount)
{
    std::scoped_lock lock(m_mutex);
    VirtualShadowInvalidationBatch result;
    result.invalidateAllActiveClipmaps = m_overflowed;
    result.bounds.swap(m_pending);
    m_overflowed = false;

    const uint32_t skinnedMask = dynamicSkinnedClipmapCount >= 32u
        ? 0u
        : (0xFFFFFFFFu << dynamicSkinnedClipmapCount);
    for (auto& bounds : result.bounds) {
        if (bounds.mobility == VirtualShadowCasterMobility::SkinnedOrDeformable) {
            bounds.clipmapMask &= skinnedMask;
        }
    }
    std::erase_if(result.bounds, [](const auto& bounds) { return bounds.clipmapMask == 0u; });
    return result;
}

uint64_t VirtualShadowInvalidationQueue::GetOverflowCount() const
{
    std::scoped_lock lock(m_mutex);
    return m_overflowCount;
}

VirtualShadowPassBuilder::VirtualShadowPassBuilder(
    std::vector<RenderGraph::ExternalPassDesc>& passes,
    std::string afterPass,
    std::string beforePass)
    : m_passes(passes)
    , m_lastPass(std::move(afterPass))
    , m_beforePass(std::move(beforePass))
{}

void VirtualShadowPassBuilder::Add(RenderGraph::ExternalPassDesc pass)
{
    if (pass.name.empty()) {
        throw std::invalid_argument("Virtual shadow caster pass must have a stable name");
    }
    auto insertion = RenderGraph::ExternalInsertPoint::After(m_lastPass);
    if (!m_beforePass.empty()) {
        insertion.AlsoBefore(m_beforePass);
    }
    pass.At(std::move(insertion));
    m_lastPass = pass.name;
    m_passes.push_back(std::move(pass));
}

const std::string& VirtualShadowPassBuilder::LastPassName() const
{
    return m_lastPass;
}

VirtualShadowCasterRegistry::VirtualShadowCasterRegistry()
    : m_invalidationQueue(std::make_shared<VirtualShadowInvalidationQueue>())
{}

void VirtualShadowCasterRegistry::Register(IVirtualShadowCasterProvider& provider)
{
    const std::string id(provider.GetVirtualShadowCasterProviderId());
    if (id.empty()) {
        throw std::invalid_argument("Virtual shadow caster provider ID cannot be empty");
    }
    if (!m_providerIds.emplace(id, &provider).second) {
        throw std::runtime_error("Duplicate virtual shadow caster provider ID: " + id);
    }
    m_providers.push_back(&provider);
    provider.OnVirtualShadowCasterRegistered(m_invalidationQueue);
}

bool VirtualShadowCasterRegistry::Empty() const noexcept { return m_providers.empty(); }
size_t VirtualShadowCasterRegistry::Size() const noexcept { return m_providers.size(); }

float VirtualShadowCasterRegistry::GetRequestedDynamicShadowRadius() const
{
    float radius = 0.0f;
    for (const auto* provider : m_providers) {
        const float requested = provider->GetRequestedDynamicShadowRadius();
        if (std::isfinite(requested) && requested > radius) {
            radius = requested;
        }
    }
    return radius;
}

std::shared_ptr<VirtualShadowInvalidationQueue> VirtualShadowCasterRegistry::GetInvalidationQueue() const
{
    return m_invalidationQueue;
}

void VirtualShadowCasterRegistry::GatherPreparationPasses(
    const VirtualShadowCasterBuildContext& context,
    VirtualShadowPassBuilder& builder) const
{
    for (auto* provider : m_providers) {
        provider->GatherVirtualShadowPreparationPasses(context, builder);
    }
}

void VirtualShadowCasterRegistry::GatherRasterPasses(
    const VirtualShadowCasterBuildContext& context,
    VirtualShadowPassBuilder& builder) const
{
    for (auto* provider : m_providers) {
        provider->GatherVirtualShadowRasterPasses(context, builder);
    }
}
