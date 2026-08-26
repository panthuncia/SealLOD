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
#include <array>

#include "Render/AsyncStateGraph.h"

namespace org { class Resource; }

namespace br::render {

enum class PublishedResourceUsage : std::uint8_t {
    ShaderResource, UnorderedAccess, IndirectArguments, CopySource, CopyDestination, ActiveDrawList
};

enum class PublishedFragmentKind : std::uint8_t {
    Materials, Terrain, Geometry, DrawRecords, ActiveDrawLists, IndirectWorkloads, Count
};

inline constexpr std::size_t kPublishedFragmentCount =
    static_cast<std::size_t>(PublishedFragmentKind::Count);
inline constexpr std::uint64_t PublishedFragmentMask(PublishedFragmentKind kind) noexcept {
    return std::uint64_t{ 1 } << static_cast<std::uint8_t>(kind);
}

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
    std::unordered_map<PublishedResourceKey, std::uint64_t,
        PublishedResourceKey::Hasher> contentVersions;
    [[nodiscard]] std::shared_ptr<const ResourceList> Find(const PublishedResourceKey& key) const;
    [[nodiscard]] ResourceList FindAll(const PublishedResourceQuery& query) const;
    [[nodiscard]] std::uint64_t ContentVersion(const PublishedResourceKey& key) const noexcept;
    [[nodiscard]] std::uint64_t ContentVersion(const PublishedResourceQuery& query) const noexcept;
};

struct PublishedStateFragment {
    std::uint64_t revision = 0;
    // The graph artifact whose payload became this manifest fragment. Frame
    // commit acknowledges both this root and its dependency closure.
    ArtifactKey publicationRoot{};
    ArtifactPayload payload;
    std::vector<ArtifactSnapshot> dependencyClosure;
    std::vector<std::shared_ptr<const void>> resourceHolds;
};

struct RendererStateFragmentArtifact {
    PublishedFragmentKind kind = PublishedFragmentKind::Geometry;
    bool publishRoot = true;
    // Zero means that this fragment replaces its own catalog namespace.
    std::uint64_t catalogOwnerMask = 0;
    PublishedStateFragment fragment;
    std::vector<std::pair<PublishedResourceKey,
        std::shared_ptr<const PublishedResourceCatalog::ResourceList>>> catalogEntries;
};

struct PublishedRendererState {
    std::uint64_t epoch = 0;
    PublishedStateFragment materials;
    PublishedStateFragment terrain;
    PublishedStateFragment geometry;
    PublishedStateFragment drawRecords;
    PublishedStateFragment activeDrawLists;
    PublishedStateFragment indirectWorkloads;
    std::shared_ptr<const PublishedResourceCatalog> resourceCatalog;

    [[nodiscard]] PublishedStateFragment& Fragment(PublishedFragmentKind kind);
    [[nodiscard]] const PublishedStateFragment& Fragment(PublishedFragmentKind kind) const;
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

struct RendererStateCommitResult {
    std::shared_ptr<const PublishedRendererState> state;
    bool committed = false;
    std::array<std::shared_ptr<const PublishedRendererState>, 3> retiredStates{};
    std::uint8_t retiredStateCount = 0;
    std::function<void(std::uint64_t)> rejectedCallback;
    std::uint64_t rejectedEpoch = 0;

    [[nodiscard]] bool HasDeferredWork() const noexcept {
        return committed || retiredStateCount != 0 || static_cast<bool>(rejectedCallback);
    }
    void RunDeferred() noexcept;
};

class RendererStatePublisher {
public:
    explicit RendererStatePublisher(std::size_t framesInFlight = 0);

    void Bootstrap(std::shared_ptr<const PublishedRendererState> fallback, std::size_t framesInFlight);
    bool PublishCandidate(RendererStateCandidate candidate);
    bool PublishArtifact(const ArtifactSnapshot& artifact);

    // Must be called after the frame slot fence has completed. This releases
    // that slot's old state and captures the selected state for the new frame.
    RendererStateCommitResult Commit(std::size_t frameSlot);
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
