#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/PublishedTypes.h>
#include <ORGModuleServices/Async/VersionedBufferFamily.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/BufferJournal.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

namespace org::compiler { struct PreparedBackingState; }

namespace br::render {

class AsyncStateGraph;

// The versioned buffers are ORGModuleServices' (BufferJournal, BackingPool, BuildVersionedBuffer). This header is BasicRenderer's
// adapter: its names for those types, the catalog identity carried as a version's tag, the renderer's retirement clock and pool
// hooks, and the producer registration that wraps versions into renderer-state fragments.
using VersionedGpuBufferImage = org::services::BufferImage;
using VersionedGpuBufferWrite = org::services::BufferWrite;
using VersionedGpuBufferJournal = org::services::BufferJournal;
using VersionedGpuBufferBuildInput = org::services::VersionedBufferBuildInput;
using VersionedGpuBufferBackingPool = org::services::BackingPool;
using PublishedGpuBufferVersion = org::services::PublishedBufferVersion;
using BufferRevisionMode = org::services::BufferRevisionMode;

// A version's catalog identity (VersionedBufferBuildInput::tag): the producer publishes its resource under
// PublishedResourceKey{ owner, usage, 0, 0, variant } in an `owner` fragment.
struct VersionedBufferCatalogTag {
    PublishedFragmentKind owner = PublishedFragmentKind::Geometry;
    PublishedResourceUsage usage = PublishedResourceUsage::ShaderResource;
    std::uint64_t variant = 0;
};

[[nodiscard]] std::shared_ptr<const void> MakeVersionedBufferTag(
    PublishedFragmentKind owner, PublishedResourceUsage usage, std::uint64_t variant);
// The native state the producer seeded under SARP_PERSISTENT_PUBLICATIONS (PublishedBufferVersion::hostState); null otherwise.
[[nodiscard]] inline std::shared_ptr<const org::compiler::PreparedBackingState> PreparedBackingStateOf(
    const PublishedGpuBufferVersion& version) noexcept {
    return std::static_pointer_cast<const org::compiler::PreparedBackingState>(version.hostState);
}

// One renderer's versioned-buffer lifetime: its retirement clock (advanced once per retired frame slot), its frames in flight,
// and the pools its managers lease backings from. Owned by the renderer; managers reach it through
// RendererStateRequestService::VersionedBuffers().
class VersionedBufferRuntime {
public:
    explicit VersionedBufferRuntime(std::uint32_t framesInFlight);
    VersionedBufferRuntime(const VersionedBufferRuntime&) = delete;
    VersionedBufferRuntime& operator=(const VersionedBufferRuntime&) = delete;

    // Any thread: a frame slot's fence completed and its retained published states were released.
    void NotifyFrameRetirement() noexcept;
    [[nodiscard]] std::uint64_t RetirementEpoch() const noexcept { return m_clock->Completed(); }
    [[nodiscard]] const std::shared_ptr<org::services::RetirementClock>& Clock() const noexcept { return m_clock; }
    [[nodiscard]] std::uint32_t FramesInFlight() const noexcept { return m_framesInFlight; }
    // The clock point a backing superseded now as the published one is not reused before: async admission can overlap one
    // generation of planned work with the submitted frame ring, so two complete rotations plus one.
    [[nodiscard]] std::uint64_t SupersededSafeAt() const noexcept {
        return RetirementEpoch() + 2ull * m_framesInFlight + 1u;
    }
    // A pool with the renderer's hooks (indexed structured buffers, memory tags, SARP telemetry), 2·FIF + 2 backings per class.
    // Acquisition is serialized by the pool's own mutex: several graph workers build versions.
    [[nodiscard]] std::shared_ptr<VersionedGpuBufferBackingPool> CreatePool(std::string name) const;

private:
    std::shared_ptr<org::services::RetirementClock> m_clock;
    std::atomic<std::uint64_t> m_retirements{ 0 };
    std::uint32_t m_framesInFlight = 1;
};

// VersionedBufferFamily::Acknowledge for raw pool users: `version` became the published one, and `published` (the version that
// was) is bound to the runtime's SupersededSafeAt() before it is replaced. Single-threaded per slot.
void AcknowledgePublishedVersion(std::shared_ptr<const PublishedGpuBufferVersion>& published,
    std::shared_ptr<const PublishedGpuBufferVersion> version, const VersionedBufferRuntime& runtime);

// The single manager-facing authority for one persistent buffer address (org::services::VersionedBufferFamily over the renderer
// graph), configured with BasicRenderer's catalog identity and a pool of its runtime.
class VersionedBufferFamily final : public org::services::VersionedBufferFamily<RendererGraphBinding> {
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
        bool gpuWritten = false;
    };

    // Throws std::invalid_argument for a generic address or a zero stride.
    VersionedBufferFamily(const VersionedBufferRuntime& runtime, Config config);
};

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph);

} // namespace br::render
