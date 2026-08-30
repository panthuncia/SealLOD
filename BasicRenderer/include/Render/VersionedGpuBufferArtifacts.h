#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>

#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"

namespace org {
class GloballyIndexedResource;
namespace runtime { class IUploadService; }
}

namespace br::render {

struct PublishedGpuBufferVersion;
class RendererStateRequestService;

enum class BufferRevisionMode : std::uint8_t {
    Patch,
    Replace,
};

struct BufferBackingArtifact {
    std::shared_ptr<org::GloballyIndexedResource> resource;
    std::uint64_t backingGeneration = 0;
    std::uint64_t capacityClass = 0;
    std::uint64_t byteCapacity = 0;
    std::uint64_t contentEpoch = 0;
    std::shared_ptr<const std::vector<std::byte>> cpuShadow;
    std::uint64_t lastPublishedRetirementEpoch = 0;
    bool wasPublished = false;
};

class VersionedGpuBufferBackingPool : public std::enable_shared_from_this<VersionedGpuBufferBackingPool> {
public:
    ~VersionedGpuBufferBackingPool();
    [[nodiscard]] std::shared_ptr<BufferBackingArtifact> Acquire(
        std::uint64_t capacityClass, std::uint32_t elementStride,
        bool unorderedAccess, bool indirectArguments, std::string_view debugName,
        bool& expanded);
    void Retire(std::uint64_t backingGeneration) noexcept;
    void AcknowledgePublished(std::uint64_t backingGeneration,
        std::uint32_t framesInFlight) noexcept;
    // Registers a one-shot wake for the next frame-safe retirement boundary.
    // The returned identity is suitable for ArtifactSuspension::Capacity.
    [[nodiscard]] std::uint64_t SubscribeAvailability(
        std::function<void(std::uint64_t)> callback);
    void NotifyAvailability() noexcept;
private:
    std::mutex m_mutex;
    std::vector<std::shared_ptr<BufferBackingArtifact>> m_backings;
    std::vector<std::pair<std::uint64_t, std::function<void(std::uint64_t)>>> m_waiters;
    bool m_registeredForRetirementWake = false;
    std::uint64_t m_nextGeneration = 1;
    std::uint64_t m_activePublishedGeneration = 0;
    std::uint32_t m_framesInFlight = 3;
};

// Called only after a renderer frame slot's fence has completed and its
// retained published states have been destroyed.
void NotifyVersionedGpuBufferFrameRetirement() noexcept;
[[nodiscard]] std::uint64_t VersionedGpuBufferFrameRetirementEpoch() noexcept;

struct VersionedGpuBufferWrite {
    std::uint64_t sequence = 0;
    std::uint64_t elementOffset = 0;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

struct VersionedGpuBufferBuildInput {
    org::runtime::IUploadService* uploadService = nullptr;
    std::string debugName;
    std::uint64_t writeSequence = 0;
    std::uint32_t elementStride = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t capacity = 0;
    bool unorderedAccess = false;
    bool indirectArguments = false;
    PublishedFragmentKind catalogOwner = PublishedFragmentKind::Geometry;
    PublishedResourceUsage catalogUsage = PublishedResourceUsage::ShaderResource;
    std::uint64_t catalogVariant = 0;
    std::shared_ptr<const PublishedGpuBufferVersion> previous;
    std::shared_ptr<VersionedGpuBufferBackingPool> backingPool;
    std::vector<VersionedGpuBufferWrite> writes;
	// Immutable view of the producer's authoritative desired image. The journal
	// uses copy-on-write, so captures share this image until the next mutation.
	std::shared_ptr<const std::vector<std::byte>> desiredBytes;
	// All writes after this epoch are present in writes. A backing at or beyond
	// this epoch can catch up without scanning or replaying the full image.
	std::uint64_t journalBaseSequence = 0;
    // Optional complete initial image. Successors normally use previous+writes.
    std::vector<std::byte> bytes;
};

struct PublishedGpuBufferVersion {
    ~PublishedGpuBufferVersion();
    std::uint64_t revision = 0;
    std::uint64_t writeSequence = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t capacity = 0;
    std::uint32_t elementStride = 0;
    BufferRevisionMode revisionMode = BufferRevisionMode::Replace;
    std::uint64_t contentVersion = 0;
    std::uint64_t contentEpoch = 0;
    std::uint64_t backingEpoch = 0;
    std::shared_ptr<BufferBackingArtifact> backing;
    std::weak_ptr<VersionedGpuBufferBackingPool> backingPool;
    std::shared_ptr<org::GloballyIndexedResource> resource;
    std::shared_ptr<const std::vector<std::byte>> cpuShadow;
};

using BufferContentArtifact = PublishedGpuBufferVersion;
using BufferRevisionRequest = VersionedGpuBufferBuildInput;

// Thread-safe desired-state journal used by mutable manager-facing allocators.
// Captures are immutable and can be handed directly to graph producers without
// retaining a pointer to the source manager or its storage.
class VersionedGpuBufferJournal {
public:
    struct Capture {
        std::uint64_t writeSequence = 0;
        std::uint64_t elementCount = 0;
        std::uint64_t capacity = 0;
        std::shared_ptr<const PublishedGpuBufferVersion> previous;
        std::vector<VersionedGpuBufferWrite> writes;
        std::vector<std::byte> initialBytes;
		std::shared_ptr<const std::vector<std::byte>> desiredBytes;
		std::uint64_t journalBaseSequence = 0;
    };

    explicit VersionedGpuBufferJournal(std::uint32_t elementStride = 0);

    void Initialize(std::span<const std::byte> bytes,
        std::uint64_t elementCount, std::uint64_t capacity);
    std::uint64_t AppendWrite(std::uint64_t elementOffset,
        std::span<const std::byte> bytes, std::uint64_t resultingElementCount);
    std::uint64_t ReplaceImage(std::span<const std::byte> bytes,
        std::uint64_t elementCount, std::uint64_t capacity);
    void RequestCapacity(std::uint64_t capacity);
    [[nodiscard]] Capture CaptureDesired() const;
    void Acknowledge(const std::shared_ptr<const PublishedGpuBufferVersion>& version);
    [[nodiscard]] std::uint64_t DesiredSequence() const;
    [[nodiscard]] bool HasUnpublishedChanges() const;

private:
    std::uint32_t m_elementStride = 0;
    mutable std::mutex m_mutex;
    std::uint64_t m_writeSequence = 0;
    std::uint64_t m_elementCount = 0;
    std::uint64_t m_capacity = 0;
    std::shared_ptr<const PublishedGpuBufferVersion> m_previous;
    std::vector<VersionedGpuBufferWrite> m_writes;
    std::vector<std::byte> m_initialBytes;
	std::shared_ptr<std::vector<std::byte>> m_desiredBytes;
};

// The single manager-facing authority for one persistent buffer address. It
// owns backing reuse and constructs immutable graph requests; callers retain
// only the returned version handle.
class VersionedBufferFamily {
public:
    struct Config {
        ArtifactAddress address{};
        std::string debugName;
        std::uint32_t elementStride = 0;
        bool unorderedAccess = false;
        bool indirectArguments = false;
        PublishedFragmentKind catalogOwner = PublishedFragmentKind::Geometry;
        PublishedResourceUsage catalogUsage = PublishedResourceUsage::ShaderResource;
        std::uint64_t catalogVariant = 0;
    };

    explicit VersionedBufferFamily(Config config);
    [[nodiscard]] ArtifactRequestResult RequestSnapshot(RendererStateRequestService& requests,
        org::runtime::IUploadService& uploads, std::uint64_t revision,
        std::span<const std::byte> bytes, std::uint64_t elementCount,
        std::uint64_t capacity = 0);
    [[nodiscard]] ArtifactRequestResult RequestContentSnapshot(
        RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
        std::span<const std::byte> bytes, std::uint64_t elementCount,
        std::uint64_t capacity = 0);
    [[nodiscard]] ArtifactRequestResult RequestCapture(RendererStateRequestService& requests,
        org::runtime::IUploadService& uploads, std::uint64_t revision,
        VersionedGpuBufferJournal::Capture capture);
    void Acknowledge(std::shared_ptr<const PublishedGpuBufferVersion> version);
    [[nodiscard]] const Config& Configuration() const noexcept { return m_config; }

private:
    Config m_config;
    std::shared_ptr<VersionedGpuBufferBackingPool> m_backingPool;
    std::mutex m_mutex;
    std::shared_ptr<const PublishedGpuBufferVersion> m_previous;
    std::uint64_t m_lastJournalRevision = 0;
    ArtifactVersionHandle m_lastJournalHandle;
    std::unordered_map<std::uint64_t, std::uint64_t> m_contentRevisions;
    std::uint64_t m_nextContentRevision = 0;
};

// Pure replay step shared by the producer and deterministic journal tests.
std::shared_ptr<const std::vector<std::byte>> ReplayVersionedGpuBufferAuthoritativeState(
    const VersionedGpuBufferBuildInput& input,
    std::string& error);

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph);

} // namespace br::render
