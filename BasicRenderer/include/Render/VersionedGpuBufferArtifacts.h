#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"

namespace org {
class GloballyIndexedResource;
namespace runtime { class IUploadService; }
}

namespace br::render {

struct PublishedGpuBufferVersion;

struct VersionedGpuBufferWrite {
    std::uint64_t sequence = 0;
    std::uint64_t elementOffset = 0;
    std::vector<std::byte> bytes;
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
    std::vector<VersionedGpuBufferWrite> writes;
    // Optional complete initial image. Successors normally use previous+writes.
    std::vector<std::byte> bytes;
};

struct PublishedGpuBufferVersion {
    std::uint64_t revision = 0;
    std::uint64_t writeSequence = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t capacity = 0;
    std::uint32_t elementStride = 0;
    std::shared_ptr<org::GloballyIndexedResource> resource;
    std::shared_ptr<const std::vector<std::byte>> cpuShadow;
};

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
    };

    explicit VersionedGpuBufferJournal(std::uint32_t elementStride = 0);

    void Initialize(std::span<const std::byte> bytes,
        std::uint64_t elementCount, std::uint64_t capacity);
    std::uint64_t AppendWrite(std::uint64_t elementOffset,
        std::span<const std::byte> bytes, std::uint64_t resultingElementCount);
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
};

// Pure replay step shared by the producer and deterministic journal tests.
std::shared_ptr<const std::vector<std::byte>> ReplayVersionedGpuBufferShadow(
    const VersionedGpuBufferBuildInput& input,
    std::string& error);

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph);

} // namespace br::render
