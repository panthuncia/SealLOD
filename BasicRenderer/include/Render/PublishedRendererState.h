#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <optional>

#include "Render/AsyncStateGraph.h"

namespace org { class Resource; }

namespace br::render {

enum class PublishedResourceUsage : std::uint8_t {
    ShaderResource, UnorderedAccess, IndirectArguments, CopySource, CopyDestination, ActiveDrawList
};

enum class PublishedFragmentKind : std::uint8_t {
    Materials, Geometry, DrawRecords, ActiveDrawLists, IndirectWorkloads
};

struct PublishedResourceKey {
    PublishedFragmentKind owner = PublishedFragmentKind::Geometry;
    PublishedResourceUsage usage = PublishedResourceUsage::ShaderResource;
    std::uint64_t renderPhaseHash = 0;
    std::uint64_t viewOrWorkloadID = 0;
    std::uint64_t variant = 0;
    auto operator<=>(const PublishedResourceKey&) const = default;
    struct Hasher { std::size_t operator()(const PublishedResourceKey&) const noexcept; };
};

struct PublishedResourceQuery {
    std::optional<PublishedFragmentKind> owner;
    std::optional<PublishedResourceUsage> usage;
    std::optional<std::uint64_t> renderPhaseHash;
    std::optional<std::uint64_t> viewOrWorkloadID;
    std::uint64_t requiredVariantMask = 0;
    std::uint64_t forbiddenVariantMask = 0;
    [[nodiscard]] bool Matches(const PublishedResourceKey& key) const noexcept;
};

struct PublishedResourceCatalog {
    using ResourceList = std::vector<std::shared_ptr<org::Resource>>;
    std::unordered_map<PublishedResourceKey, std::shared_ptr<const ResourceList>,
        PublishedResourceKey::Hasher> entries;
    [[nodiscard]] std::shared_ptr<const ResourceList> Find(const PublishedResourceKey& key) const;
    [[nodiscard]] ResourceList FindAll(const PublishedResourceQuery& query) const;
};

struct PublishedStateFragment {
    std::uint64_t revision = 0;
    ArtifactPayload payload;
    std::vector<ArtifactSnapshot> dependencyClosure;
    std::vector<std::shared_ptr<const void>> resourceHolds;
};

struct RendererStateFragmentArtifact {
    PublishedFragmentKind kind = PublishedFragmentKind::Geometry;
    bool publishRoot = true;
    PublishedStateFragment fragment;
    std::vector<std::pair<PublishedResourceKey,
        std::shared_ptr<const PublishedResourceCatalog::ResourceList>>> catalogEntries;
};

struct PublishedRendererState {
    std::uint64_t epoch = 0;
    PublishedStateFragment materials;
    PublishedStateFragment geometry;
    PublishedStateFragment drawRecords;
    PublishedStateFragment activeDrawLists;
    PublishedStateFragment indirectWorkloads;
    std::shared_ptr<const PublishedResourceCatalog> resourceCatalog;
};

class PublishedStateSource {
public:
    static void SetProcessSource(std::shared_ptr<PublishedStateSource> source) noexcept;
    [[nodiscard]] static std::shared_ptr<PublishedStateSource> ProcessSource() noexcept;
    void Store(std::shared_ptr<const PublishedRendererState> state) noexcept;
    [[nodiscard]] std::shared_ptr<const PublishedRendererState> Load() const noexcept;
    [[nodiscard]] std::uint64_t Epoch() const noexcept;
private:
    std::atomic<std::shared_ptr<const PublishedRendererState>> m_state;
};

struct RendererStateCandidate {
    std::uint64_t baseEpoch = 0;
    std::shared_ptr<const PublishedRendererState> state;
};

struct FrameManifestPayload {
    std::uint64_t baseEpoch = 0;
    std::shared_ptr<const PublishedRendererState> state;
};

struct RendererStatePublisherStats {
    std::uint64_t candidates = 0;
    std::uint64_t committed = 0;
    std::uint64_t replacedCandidates = 0;
    std::uint64_t rejectedBaseEpoch = 0;
    std::uint64_t commitMicros = 0;
    std::size_t retainedFrameStates = 0;
};

class RendererStatePublisher {
public:
    explicit RendererStatePublisher(std::size_t framesInFlight = 0);

    void Bootstrap(std::shared_ptr<const PublishedRendererState> fallback, std::size_t framesInFlight);
    bool PublishCandidate(RendererStateCandidate candidate);
    bool PublishArtifact(const ArtifactSnapshot& artifact);

    // Must be called after the frame slot fence has completed. This releases
    // that slot's old state and captures the selected state for the new frame.
    std::shared_ptr<const PublishedRendererState> Commit(std::size_t frameSlot);
    void ReleaseFrameSlot(std::size_t frameSlot);
    void DiscardCandidate();

    [[nodiscard]] std::shared_ptr<const PublishedRendererState> Active() const;
    [[nodiscard]] std::uint64_t ActiveEpoch() const;
    [[nodiscard]] RendererStatePublisherStats Stats() const;
    [[nodiscard]] std::shared_ptr<PublishedStateSource> ResourceSource() const { return m_source; }
    void SetCandidateRejectedCallback(std::function<void(std::uint64_t)> callback);

private:
    mutable std::mutex m_mutex;
    RendererStateCandidate m_candidate;
    std::shared_ptr<const PublishedRendererState> m_active;
    std::vector<std::shared_ptr<const PublishedRendererState>> m_frameStates;
    RendererStatePublisherStats m_stats;
    std::shared_ptr<PublishedStateSource> m_source = std::make_shared<PublishedStateSource>();
    std::function<void(std::uint64_t)> m_candidateRejected;
};

} // namespace br::render
