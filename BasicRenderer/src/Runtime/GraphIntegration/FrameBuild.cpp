#include "Runtime/GraphIntegration/FrameBuild.h"

#include "BasicRenderer/Extensions/RenderContext.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "Interfaces/IResourceProvider.h"
#include "Render/RenderGraph/ScheduleTrace.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resource.h"
#include "Scene/ECS/RendererECSManager.h"

namespace br::render {

namespace {
thread_local FrameBuild* t_currentFrameBuild = nullptr;
thread_local uint32_t t_declarationEpoch = kNoFrameEpoch;

// What a provider gave at one moment (SnapshotResourceProvider).
class ResourceProviderSnapshot final : public org::IResourceProvider {
public:
    explicit ResourceProviderSnapshot(org::IResourceProvider& provider) {
        m_keys = provider.GetSupportedKeys();
        for (const auto& key : m_keys) m_resources.emplace(key, provider.ProvideResource(key));
        m_resolverKeys = provider.GetSupportedResolverKeys();
        for (const auto& key : m_resolverKeys) m_resolvers.emplace(key, provider.ProvideResolver(key));
    }

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override {
        const auto found = m_resources.find(key);
        return found == m_resources.end() ? nullptr : found->second;
    }
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override { return m_keys; }
    std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override {
        const auto found = m_resolvers.find(key);
        return found == m_resolvers.end() ? nullptr : found->second;
    }
    std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override { return m_resolverKeys; }

private:
    std::vector<org::ResourceIdentifier> m_keys;
    std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
    std::vector<org::ResourceIdentifier> m_resolverKeys;
    std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::IResourceResolver>, org::ResourceIdentifier::Hasher> m_resolvers;
};
}

std::shared_ptr<org::IResourceProvider> SnapshotResourceProvider(org::IResourceProvider& provider) {
    return std::make_shared<ResourceProviderSnapshot>(provider);
}

FrameBuild* CurrentFrameBuild() noexcept {
    return t_currentFrameBuild;
}

const UpdateContext* BuildUpdateContext() noexcept {
    return t_currentFrameBuild ? t_currentFrameBuild->inputs.lastUpdate.get() : nullptr;
}

ScopedFrameBuild::ScopedFrameBuild(FrameBuild& build) noexcept
    : m_previous(t_currentFrameBuild) {
    t_currentFrameBuild = &build;
}

ScopedFrameBuild::~ScopedFrameBuild() {
    t_currentFrameBuild = m_previous;
}

uint32_t CurrentDeclarationEpoch() noexcept {
    return t_declarationEpoch;
}

ScopedDeclarationEpoch::ScopedDeclarationEpoch(uint32_t epoch) noexcept
    : m_previous(t_declarationEpoch) {
    t_declarationEpoch = epoch;
}

ScopedDeclarationEpoch::~ScopedDeclarationEpoch() {
    t_declarationEpoch = m_previous;
}

namespace {
std::atomic<int> g_frameSplit{ 2 };
}

void SetFrameSplit(int split) noexcept {
    g_frameSplit.store(split < 0 ? 0 : split > 2 ? 2 : split, std::memory_order_relaxed);
}

bool FrameSplit() noexcept {
    return g_frameSplit.load(std::memory_order_relaxed) != 0;
}

int FrameSplitLevel() noexcept {
    return g_frameSplit.load(std::memory_order_relaxed);
}

uint32_t HostEpochOf(uint32_t frameEpoch) noexcept {
    const int level = FrameSplitLevel();
    if (level == 0) return kFrameHostEpoch;
    switch (frameEpoch) {
    case kShadingEpoch: return level >= 2 ? kShadingEpoch : kFrameHostEpoch;
    case kPostEpoch: return kPostEpoch;
    case kOverlayEpoch: return kOverlayEpoch;
    default: return kFrameHostEpoch;  // Visibility
    }
}

const std::vector<uint32_t>& FrameHostEpochs() noexcept {
    static const std::vector<uint32_t> four{ kFrameHostEpoch, kShadingEpoch, kPostEpoch, kOverlayEpoch };
    static const std::vector<uint32_t> three{ kFrameHostEpoch, kPostEpoch, kOverlayEpoch };
    static const std::vector<uint32_t> one{ kFrameHostEpoch };
    const int level = FrameSplitLevel();
    return level >= 2 ? four : level == 1 ? three : one;
}

std::vector<uint32_t> HostEpochsOfMask(uint32_t mask) {
    std::vector<uint32_t> hosts;
    for (const auto host : FrameHostEpochs())
        if (mask & HostEpochBit(host)) hosts.push_back(host);
    return hosts;
}

uint32_t HostEpochMaskOf(std::initializer_list<uint32_t> frameEpochs) noexcept {
    uint32_t mask = 0;
    for (const auto epoch : frameEpochs) mask |= HostEpochBit(HostEpochOf(epoch));
    return mask;
}

bool FrameBuild::Declares(uint32_t epoch) const noexcept {
    return std::find(epochs.begin(), epochs.end(), epoch) != epochs.end();
}

void FrameBuild::SetHostEpochs(std::vector<uint32_t> hosts) {
    hostEpochs = std::move(hosts);
    epochs.clear();
    for (const auto epoch : kFrameEpochs)
        if (std::find(hostEpochs.begin(), hostEpochs.end(), HostEpochOf(epoch)) != hostEpochs.end()) epochs.push_back(epoch);
}

std::vector<const org::Resource*> FrameInterface::Objects() const {
    std::vector<const org::Resource*> objects;
    if (visibilityBuffer) objects.push_back(visibilityBuffer.get());
    if (visibilityHeadPointers) objects.push_back(visibilityHeadPointers.get());
    for (const auto& registration : registrations)
        if (registration.resource) objects.push_back(registration.resource.get());
    for (const auto* texture : { sunShadowMask.get(), stochasticDiffuse.get(), stochasticSpecular.get(), vsmReceiverHistory.get() })
        if (texture) objects.push_back(texture);
    return objects;
}

void FramePassEpochs::Tag(const std::string& pass, uint32_t epoch) {
    if (!IsFrameEpoch(epoch)) throw std::logic_error("Frame pass '" + pass + "' tagged with epoch " + std::to_string(epoch) + ", not a frame epoch");
    std::scoped_lock lock(m_mutex);
    m_epochs[pass] = epoch;
}

void FramePassEpochs::Follows(const std::string& pass, const std::string& anchor) {
    std::scoped_lock lock(m_mutex);
    m_anchors[pass] = anchor;
}

std::unordered_map<std::string, uint32_t> FramePassEpochs::Tagged() const {
    std::scoped_lock lock(m_mutex);
    return m_epochs;
}

void FramePassEpochs::SetKnown(std::shared_ptr<const std::unordered_map<std::string, uint32_t>> known) {
    std::scoped_lock lock(m_mutex);
    m_known = std::move(known);
}

uint32_t FramePassEpochs::EpochOf(const std::string& pass, bool* dangling) const {
    std::scoped_lock lock(m_mutex);
    std::string name = pass;
    bool follower = false;
    // A chain of followers ends at a tagged pass; the bound keeps a cycle from looping.
    for (int hop = 0; hop < 8; ++hop) {
        if (const auto tagged = m_epochs.find(name); tagged != m_epochs.end()) return tagged->second;
        // A pass of an attached graph (an epoch this build does not declare, or declares later).
        if (follower && m_known)
            if (const auto known = m_known->find(name); known != m_known->end()) return known->second;
        const auto anchor = m_anchors.find(name);
        if (anchor == m_anchors.end()) break;
        follower = true;
        name = anchor->second;
    }
    if (!follower) return kNoFrameEpoch;
    if (dangling) *dangling = true;
    return kVisibilityEpoch;
}

uint32_t FramePassEpochs::KnownEpochOf(const std::string& pass) const {
    std::scoped_lock lock(m_mutex);
    if (const auto tagged = m_epochs.find(pass); tagged != m_epochs.end()) return tagged->second;
    if (m_known)
        if (const auto known = m_known->find(pass); known != m_known->end()) return known->second;
    return kNoFrameEpoch;
}

FrameEpochCheck CheckFrameEpochs(const org::ScheduleTrace& trace, const FrameBuild& build) {
    FrameEpochCheck check;
    // The passes in execution order: batches as submitted, passes within a batch in order.
    std::vector<uint32_t> order;
    order.reserve(trace.passes.size());
    for (const auto& batch : trace.batches) order.insert(order.end(), batch.passes.begin(), batch.passes.end());
    if (order.size() != trace.passes.size()) {
        order.clear();
        for (uint32_t pass = 0; pass < trace.passes.size(); ++pass) order.push_back(pass);
    }
    std::vector<uint32_t> passEpoch(trace.passes.size(), kNoFrameEpoch);
    for (uint32_t pass = 0; pass < trace.passes.size(); ++pass) {
        bool dangling = false;
        const auto epoch = build.passEpochs ? build.passEpochs->EpochOf(trace.passes[pass].name, &dangling) : kNoFrameEpoch;
        passEpoch[pass] = epoch;
        if (dangling) check.danglingFollowers.push_back(trace.passes[pass].name);
        // The host's own passes (its upload pass) are in every epoch's graph.
        if (IsFrameEpoch(epoch)) ++check.passes[FrameEpochIndex(epoch)];
        else if (!trace.passes[pass].name.starts_with("org.host.")) check.untagged.push_back(trace.passes[pass].name);
    }

    std::unordered_set<uint64_t> interfaceIds;
    for (const auto* object : build.inputs.frameInterface.Objects()) interfaceIds.insert(object->GetGlobalResourceID());

    struct Use {
        uint32_t epochMask = 0;
        uint32_t firstWriter = kNoFrameEpoch;
        bool touched = false, readFirst = false;
    };
    std::vector<Use> uses(trace.resources.size());
    for (const auto pass : order) {
        const auto epoch = passEpoch[pass];
        for (const auto& access : trace.passes[pass].accesses) {
            if (access.resource >= uses.size()) continue;
            auto& use = uses[access.resource];
            if (!use.touched) use.readFirst = !access.write;
            use.touched = true;
            if (!IsFrameEpoch(epoch)) continue;
            use.epochMask |= 1u << FrameEpochIndex(epoch);
            if (access.write && use.firstWriter == kNoFrameEpoch) use.firstWriter = epoch;
        }
    }
    check.resources = trace.resources.size();
    for (uint32_t resource = 0; resource < trace.resources.size(); ++resource) {
        const auto& use = uses[resource];
        if (std::popcount(use.epochMask) < 2) continue;
        const auto& traced = trace.resources[resource];
        if (interfaceIds.contains(traced.objectId)) {
            ++check.interfaceResources;
            check.interfaceBytes += traced.bytes;
            continue;
        }
        if (!traced.aliasCandidate) {
            ++check.persistentResources;
            continue;
        }
        FrameEpochCheck::Crossing crossing;
        crossing.name = traced.name;
        crossing.objectId = traced.objectId;
        crossing.bytes = traced.bytes;
        for (const auto epoch : kFrameEpochs)
            if (use.epochMask & (1u << FrameEpochIndex(epoch))) crossing.epochs.push_back(epoch);
        crossing.firstWriter = use.firstWriter;
        crossing.readBeforeWritten = use.readFirst;
        check.crossingBytes += crossing.bytes;
        check.crossing.push_back(std::move(crossing));
    }
    std::sort(check.crossing.begin(), check.crossing.end(), [](const auto& a, const auto& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name;
    });
    return check;
}

std::string FrameEpochCheck::Describe(size_t most) const {
    std::string text = fmt::format("passes V/S/P/O {}/{}/{}/{}, {} untagged, {} following a pass not in the graph (as Visibility's); "
        "{} resources; crossing epochs: {} transient(s) {:.1f} MB not in the interface, {} through the interface ({:.1f} MB), "
        "{} persistent", passes[0], passes[1], passes[2], passes[3], untagged.size(), danglingFollowers.size(), resources, crossing.size(), crossingBytes / 1048576.0, interfaceResources, interfaceBytes / 1048576.0,
        persistentResources);
    for (size_t index = 0; index < crossing.size() && index < most; ++index) {
        const auto& entry = crossing[index];
        std::string epochs;
        for (const auto epoch : entry.epochs) epochs += (epochs.empty() ? "" : "+") + std::string(FrameEpochName(epoch));
        text += fmt::format("\n  '{}' {:.2f} MB {} (first written in {}{})", entry.name, entry.bytes / 1048576.0, epochs,
            FrameEpochName(entry.firstWriter), entry.readBeforeWritten ? ", read before written" : "");
    }
    if (crossing.size() > most) text += fmt::format("\n  ... {} more", crossing.size() - most);
    for (size_t index = 0; index < untagged.size() && index < most; ++index) text += "\n  untagged pass '" + untagged[index] + "'";
    return text;
}

void WriteResourceEntity(std::shared_ptr<org::Resource> resource, std::function<void(flecs::world&, flecs::entity)> write,
    std::string directoryKey) {
    if (!resource || !write) return;
    if (auto* build = CurrentFrameBuild()) {
        // Recorded into the epoch whose declarations made it (Visibility when none is current).
        const auto epoch = IsFrameEpoch(CurrentDeclarationEpoch()) ? CurrentDeclarationEpoch() : kVisibilityEpoch;
        build->Outputs(epoch).resourceEntityWrites.push_back({ std::move(resource), std::move(write), std::move(directoryKey) });
        return;
    }
    auto& world = RendererECSManager::GetInstance().GetWorld();
    if (auto entity = resource->GetECSEntity()) write(world, entity);
}

} // namespace br::render
