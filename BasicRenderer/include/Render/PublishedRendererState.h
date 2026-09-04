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

class ArtifactLeaseSet {
public:
    void Add(const ArtifactLease& lease);
    void Merge(const ArtifactLeaseSet& other);
    [[nodiscard]] std::size_t Size() const noexcept { return m_leases.size(); }
private:
    std::vector<ArtifactLease> m_leases;
};

struct PublicationBundle {
    ArtifactVersionID root;
    // Persistent ownership DAG. Each node owns only its artifact and shares
    // unchanged dependency nodes with successor manifests.
    std::vector<std::shared_ptr<const PublicationBundle>> parents;
    std::vector<ArtifactVersionID> versions;
    std::vector<std::shared_ptr<const GpuSubmissionSet>> gpuSubmissions;
    std::vector<std::shared_ptr<const void>> resourceHolds;
    ArtifactLeaseSet leases;
};

enum class PublishedResourceUsage : std::uint8_t {
    ShaderResource, UnorderedAccess, IndirectArguments, CopySource, CopyDestination, ActiveDrawList
};

enum class PublishedFragmentKind : std::uint8_t {
    Materials, TextureImages, Terrain, Geometry, DrawRecords, ActiveDrawLists, IndirectWorkloads,
    Grass, Count
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

struct PublishedResourceSelection {
    using ResourceList = std::vector<std::shared_ptr<org::Resource>>;
    std::shared_ptr<const ResourceList> resources;
    std::uint64_t contentVersion = 0;
    ArtifactVersionID sourceArtifact;
    std::vector<std::shared_ptr<const GpuSubmissionSet>> gpuSubmissions;
    std::vector<std::shared_ptr<const void>> lifetimeHolds;
    std::uint64_t manifestEpoch = 0;
    std::shared_ptr<const PublicationBundle> publicationBundle;
};

struct PublishedResourceCatalog {
    using ResourceList = std::vector<std::shared_ptr<org::Resource>>;
	struct OwnerShard {
		std::unordered_map<PublishedResourceKey, std::shared_ptr<const ResourceList>,
			PublishedResourceKey::Hasher> entries;
		std::unordered_map<PublishedResourceKey, std::uint64_t,
			PublishedResourceKey::Hasher> contentVersions;
		std::unordered_map<PublishedResourceKey, PublishedResourceSelection,
			PublishedResourceKey::Hasher> selections;
	};
	std::array<std::shared_ptr<const OwnerShard>, kPublishedFragmentCount> ownerShards{};

	// Legacy construction surface retained for bootstrap callers. Published
	// updates are normalized into ownerShards before becoming visible.
    std::unordered_map<PublishedResourceKey, std::shared_ptr<const ResourceList>,
        PublishedResourceKey::Hasher> entries;
    std::unordered_map<PublishedResourceKey, std::uint64_t,
        PublishedResourceKey::Hasher> contentVersions;
    std::unordered_map<PublishedResourceKey, PublishedResourceSelection,
        PublishedResourceKey::Hasher> selections;
    [[nodiscard]] std::shared_ptr<const ResourceList> Find(const PublishedResourceKey& key) const;
    [[nodiscard]] const PublishedResourceSelection* FindSelection(
        const PublishedResourceKey& key) const noexcept;
    [[nodiscard]] std::vector<const PublishedResourceSelection*> FindSelections(
        const PublishedResourceQuery& query) const;
    [[nodiscard]] ResourceList FindAll(const PublishedResourceQuery& query) const;
    [[nodiscard]] std::uint64_t ContentVersion(const PublishedResourceKey& key) const noexcept;
    [[nodiscard]] std::uint64_t ContentVersion(const PublishedResourceQuery& query) const noexcept;
};

struct PublishedStateFragment {
    std::uint64_t revision = 0;
    // The graph artifact whose payload became this manifest fragment. The
    // persistent publication bundle retains its dependency DAG and leases;
    // frame commit therefore acknowledges only this root.
    ArtifactVersionID publicationRoot{};
    std::shared_ptr<const PublicationBundle> publicationBundle;
    std::vector<std::pair<PublishedFragmentKind, ArtifactVersionID>> publicationDependencies;
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
    PublishedStateFragment textureImages;
    PublishedStateFragment terrain;
    PublishedStateFragment geometry;
    PublishedStateFragment drawRecords;
    PublishedStateFragment activeDrawLists;
    PublishedStateFragment indirectWorkloads;
    PublishedStateFragment grass;
    std::shared_ptr<const PublishedResourceCatalog> resourceCatalog;
    std::shared_ptr<const PublicationBundle> publicationBundle;

    [[nodiscard]] PublishedStateFragment& Fragment(PublishedFragmentKind kind);
    [[nodiscard]] const PublishedStateFragment& Fragment(PublishedFragmentKind kind) const;
};

// Ordinary publication may advance independently within each fragment slot,
// but it may not move backwards within one logical artifact address. Revisions
// from different addresses are intentionally incomparable.
[[nodiscard]] bool IsMonotonicFragmentSuccessor(const PublishedStateFragment& active,
    const PublishedStateFragment& successor) noexcept;

enum class ManifestPublicationPolicy : std::uint8_t {
    MonotonicSuccessor,
    ExplicitRollback,
};

// Immutable ownership token for the renderer state selected after a frame
// slot's fence completes.  Render-graph resource resolution must retain this
// token rather than independently reloading the process publication source.
struct PublishedManifestLease {
    std::shared_ptr<const PublishedRendererState> state;
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::size_t frameSlot = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(state); }
};

class PublishedStateSource {
public:
    static void SetProcessSource(std::shared_ptr<PublishedStateSource> source) noexcept;
    [[nodiscard]] static std::shared_ptr<PublishedStateSource> ProcessSource() noexcept;
    void Store(std::shared_ptr<const PublishedRendererState> state) noexcept;
    [[nodiscard]] std::shared_ptr<const PublishedRendererState> Load() const noexcept;
    [[nodiscard]] std::shared_ptr<const PublishedManifestLease> AcquireLease(
        std::size_t frameSlot, std::shared_ptr<const PublishedRendererState> state = {}) noexcept;
    [[nodiscard]] std::shared_ptr<const PublishedManifestLease> LoadLease() const noexcept;
    [[nodiscard]] std::uint64_t Epoch() const noexcept;
	[[nodiscard]] std::shared_ptr<const void> ResolverDependencyIdentity(
		const PublishedResourceKey& key) const;
	[[nodiscard]] std::shared_ptr<const void> ResolverDependencyIdentity(
		const PublishedResourceQuery& query) const;
    void Clear() noexcept;
private:
	struct ResolverIdentityEntry {
		bool exact = false;
		PublishedResourceKey key{};
		PublishedResourceQuery query{};
		std::weak_ptr<const void> identity;
	};
    std::atomic<std::shared_ptr<const PublishedRendererState>> m_state;
    std::atomic<std::shared_ptr<const PublishedManifestLease>> m_lease;
    std::atomic<std::uint64_t> m_leaseSequence{ 0 };
	mutable std::mutex m_resolverIdentityMutex;
	mutable std::vector<ResolverIdentityEntry> m_resolverIdentities;
};

struct RendererStateCandidate {
    std::uint64_t baseEpoch = 0;
    std::shared_ptr<const PublishedRendererState> state;
    ManifestPublicationPolicy policy = ManifestPublicationPolicy::MonotonicSuccessor;
    std::string reason;
};

struct PublishedFragmentPrecondition {
    PublishedFragmentKind kind = PublishedFragmentKind::Geometry;
    ArtifactVersionID publicationRoot{};
};

struct PublishedStatePatch {
    std::uint64_t sourceEpoch = 0;
    std::array<std::optional<PublishedStateFragment>, kPublishedFragmentCount> fragments;
    std::vector<PublishedFragmentPrecondition> preconditions;
    std::uint64_t catalogOwnerMask = 0;
    std::vector<std::pair<PublishedResourceKey,
        std::shared_ptr<const PublishedResourceCatalog::ResourceList>>> catalogEntries;
    std::vector<std::pair<PublishedResourceKey, PublishedResourceSelection>> catalogSelections;
    ManifestPublicationPolicy policy = ManifestPublicationPolicy::MonotonicSuccessor;
    std::string reason;
};

// Materializes and validates a complete immutable successor. This is intended
// for GraphPublication producers so render-thread Commit only selects a ready
// state and captures its frame lease.
[[nodiscard]] std::shared_ptr<const PublishedRendererState> MaterializePublishedState(
    const std::shared_ptr<const PublishedRendererState>& base,
    const PublishedStatePatch& patch, std::uint64_t targetEpoch);

struct FrameManifestPayload {
    std::uint64_t baseEpoch = 0;
    // Patch manifests are authoritative without materializing a duplicate full
    // state. Legacy whole-state candidates populate state and leave patch null.
    std::shared_ptr<const PublishedRendererState> state;
    std::shared_ptr<const PublishedStatePatch> patch;
};

struct RendererStatePublisherStats {
    std::uint64_t candidates = 0;
    std::uint64_t committed = 0;
    std::uint64_t replacedCandidates = 0;
    std::uint64_t rejectedBaseEpoch = 0;
    std::uint64_t rebasedPatches = 0;
    std::uint64_t rejectedPatchPreconditions = 0;
    std::uint64_t rejectedFragmentRegressions = 0;
    std::uint64_t explicitRollbacks = 0;
    std::uint64_t commitMicros = 0;
    std::uint64_t commitP99Micros = 0;
    std::uint64_t commitMaxMicros = 0;
    std::uint64_t commitSamples = 0;
    std::size_t retainedFrameStates = 0;
};

struct RendererStateCommitResult {
    std::shared_ptr<const PublishedRendererState> state;
    std::shared_ptr<const PublishedManifestLease> lease;
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
    bool PublishPatch(PublishedStatePatch patch);
    bool PublishArtifact(const ArtifactSnapshot& artifact);

    // Must be called after the frame slot fence has completed. This releases
    // that slot's old state and captures the selected state for the new frame.
    RendererStateCommitResult Commit(std::size_t frameSlot);
    void ReleaseFrameSlot(std::size_t frameSlot);
    void DiscardCandidate();
    // Releases every published ownership root. Call only after producers are
    // stopped and all frame fences have retired, before device allocator teardown.
    void Shutdown();

    [[nodiscard]] std::shared_ptr<const PublishedRendererState> Active() const;
    [[nodiscard]] std::uint64_t ActiveEpoch() const;
    [[nodiscard]] RendererStatePublisherStats Stats() const;
    [[nodiscard]] std::shared_ptr<PublishedStateSource> ResourceSource() const { return m_source; }
    void SetCandidateRejectedCallback(std::function<void(std::uint64_t)> callback);

private:
    mutable std::mutex m_mutex;
    RendererStateCandidate m_candidate;
    std::vector<PublishedStatePatch> m_patches;
    std::shared_ptr<const PublishedRendererState> m_active;
    std::vector<std::shared_ptr<const PublishedRendererState>> m_frameStates;
    RendererStatePublisherStats m_stats;
    static constexpr std::size_t kCommitLatencySampleCapacity = 4096;
    std::array<std::uint64_t, kCommitLatencySampleCapacity> m_commitLatencySamples{};
    std::size_t m_commitLatencySampleCursor = 0;
    std::size_t m_commitLatencySampleCount = 0;
    std::shared_ptr<PublishedStateSource> m_source = std::make_shared<PublishedStateSource>();
    std::function<void(std::uint64_t)> m_candidateRejected;
};

} // namespace br::render
