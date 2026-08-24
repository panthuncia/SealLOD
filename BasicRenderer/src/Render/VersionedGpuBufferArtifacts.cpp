#include "Render/VersionedGpuBufferArtifacts.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>

#include "Render/Runtime/IUploadService.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Utilities/Utilities.h"

namespace br::render {

VersionedGpuBufferJournal::VersionedGpuBufferJournal(std::uint32_t elementStride)
    : m_elementStride(elementStride) {}

void VersionedGpuBufferJournal::Initialize(std::span<const std::byte> bytes,
    std::uint64_t elementCount, std::uint64_t capacity) {
    if (m_elementStride == 0 || elementCount > capacity ||
        bytes.size() != elementCount * m_elementStride) {
        throw std::invalid_argument("invalid versioned GPU buffer journal initial image");
    }
    std::lock_guard lock(m_mutex);
    m_previous.reset();
    m_writes.clear();
    m_initialBytes.assign(bytes.begin(), bytes.end());
    m_elementCount = elementCount;
    m_capacity = capacity;
    m_writeSequence = 1;
}

std::uint64_t VersionedGpuBufferJournal::AppendWrite(std::uint64_t elementOffset,
    std::span<const std::byte> bytes, std::uint64_t resultingElementCount) {
    if (m_elementStride == 0 || bytes.empty() || bytes.size() % m_elementStride != 0) {
        throw std::invalid_argument("versioned GPU buffer journal write is not row aligned");
    }
    const auto writeElements = bytes.size() / m_elementStride;
    if (elementOffset > (std::numeric_limits<std::uint64_t>::max)() - writeElements ||
        elementOffset + writeElements > resultingElementCount) {
        throw std::out_of_range("versioned GPU buffer journal write exceeds logical extent");
    }
    std::lock_guard lock(m_mutex);
    if (resultingElementCount > m_capacity) {
        throw std::out_of_range("versioned GPU buffer journal write exceeds desired capacity");
    }
    VersionedGpuBufferWrite write;
    write.sequence = ++m_writeSequence;
    write.elementOffset = elementOffset;
    write.bytes.assign(bytes.begin(), bytes.end());
    m_writes.push_back(std::move(write));
    m_elementCount = (std::max)(m_elementCount, resultingElementCount);
    return m_writeSequence;
}

void VersionedGpuBufferJournal::RequestCapacity(std::uint64_t capacity) {
    std::lock_guard lock(m_mutex);
    if (capacity > m_capacity) {
        m_capacity = capacity;
        ++m_writeSequence;
        // Capacity-only revisions need an explicit zero-length checkpoint to
        // close the captured sequence without inventing a data write.
        m_writes.push_back(VersionedGpuBufferWrite{ m_writeSequence, m_elementCount, {} });
    }
}

VersionedGpuBufferJournal::Capture VersionedGpuBufferJournal::CaptureDesired() const {
    std::lock_guard lock(m_mutex);
    return Capture{ m_writeSequence, m_elementCount, m_capacity,
        m_previous, m_writes, m_previous ? std::vector<std::byte>{} : m_initialBytes };
}

void VersionedGpuBufferJournal::Acknowledge(
    const std::shared_ptr<const PublishedGpuBufferVersion>& version) {
    if (!version) return;
    std::lock_guard lock(m_mutex);
    if (version->writeSequence > m_writeSequence ||
        (m_previous && version->writeSequence <= m_previous->writeSequence)) return;
    m_previous = version;
    std::erase_if(m_writes, [&](const VersionedGpuBufferWrite& write) {
        return write.sequence <= version->writeSequence;
    });
    m_initialBytes.clear();
}

std::uint64_t VersionedGpuBufferJournal::DesiredSequence() const {
    std::lock_guard lock(m_mutex);
    return m_writeSequence;
}

bool VersionedGpuBufferJournal::HasUnpublishedChanges() const {
    std::lock_guard lock(m_mutex);
    return !m_previous || m_previous->writeSequence != m_writeSequence;
}

std::shared_ptr<const std::vector<std::byte>> ReplayVersionedGpuBufferShadow(
    const VersionedGpuBufferBuildInput& input,
    std::string& error) {
    error.clear();
    if (input.elementStride == 0u || input.elementCount > input.capacity ||
        input.capacity > (std::numeric_limits<std::uint32_t>::max)()) {
        error = "versioned buffer count/capacity/stride invalid";
        return {};
    }
    if (input.elementCount > (std::numeric_limits<std::size_t>::max)() / input.elementStride) {
        error = "versioned buffer byte count overflows address space";
        return {};
    }
    const auto requiredBytes = static_cast<std::size_t>(input.elementCount * input.elementStride);
    auto shadow = std::make_shared<std::vector<std::byte>>(requiredBytes);
    if (!input.bytes.empty()) {
        if (requiredBytes != input.bytes.size()) {
            error = "versioned buffer byte count does not match rows";
            return {};
        }
        *shadow = input.bytes;
    } else if (input.previous && input.previous->cpuShadow) {
        const auto copyBytes = (std::min)(shadow->size(), input.previous->cpuShadow->size());
        std::copy_n(input.previous->cpuShadow->begin(), copyBytes, shadow->begin());
    }
    std::uint64_t lastSequence = input.previous ? input.previous->writeSequence : 0u;
    for (const auto& write : input.writes) {
        if (write.sequence <= lastSequence || write.sequence > input.writeSequence) {
            error = "versioned buffer write journal sequence invalid";
            return {};
        }
        if (write.bytes.size() % input.elementStride != 0u) {
            error = "versioned buffer journal write is not row aligned";
            return {};
        }
        if (write.elementOffset > (std::numeric_limits<std::size_t>::max)() / input.elementStride) {
            error = "versioned buffer journal offset overflows address space";
            return {};
        }
        const auto byteOffset = static_cast<std::size_t>(write.elementOffset * input.elementStride);
        if (byteOffset > shadow->size() || write.bytes.size() > shadow->size() - byteOffset) {
            error = "versioned buffer journal write exceeds desired size";
            return {};
        }
        std::copy(write.bytes.begin(), write.bytes.end(), shadow->begin() + byteOffset);
        lastSequence = write.sequence;
    }
    if ((!input.writes.empty() && lastSequence != input.writeSequence) ||
        (input.writes.empty() && input.previous && input.writeSequence != input.previous->writeSequence)) {
        error = "versioned buffer is not closed through requested write sequence";
        return {};
    }
    return shadow;
}

namespace {

std::shared_ptr<const GpuDependencyToken> TokenForTicket(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuDependencyToken>();
    token->isComplete = [ticket] { return ticket->Complete(); };
    token->currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    token->currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
	token->describe = [ticket] {
		const auto state = ticket->state.load(std::memory_order_acquire);
		std::lock_guard lock(ticket->timelineMutex);
		const bool timelineComplete = ticket->isTimelineComplete &&
			ticket->isTimelineComplete(ticket->timelineValue);
		return std::format("ticket-state={} timeline-complete={}",
			static_cast<unsigned>(state), timelineComplete);
	};
	token->subscribe = [ticket](std::function<void()> callback) {
        ticket->SetChangeCallback(std::move(callback));
    };
	token->cancel = [ticket] { return ticket->Cancel(); };
    return token;
}

ArtifactBuildResult BuildVersionedGpuBuffer(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<VersionedGpuBufferBuildInput>();
    if (!input || !input->uploadService || input->elementStride == 0u) {
        return ArtifactBuildResult::Failure("versioned buffer input/upload service/stride missing");
    }
    std::string replayError;
    auto shadow = ReplayVersionedGpuBufferShadow(*input, replayError);
    if (!shadow) return ArtifactBuildResult::Failure(std::move(replayError));

    Resource::ScopedECSRegistrationSuppression suppressECS;
    auto resource = CreateIndexedStructuredBuffer(
        static_cast<std::uint32_t>((std::max<std::uint64_t>)(input->capacity, 1u)),
        input->elementStride, input->unorderedAccess, input->indirectArguments);
    resource->SetName(input->debugName);

    std::shared_ptr<org::TrackedUploadTicket> ticket;
    if (!shadow->empty()) {
        ticket = input->uploadService->QueueTrackedStreamingUpload(
            shadow->data(), shadow->size(), resource, 0);
    }

    auto version = std::make_shared<PublishedGpuBufferVersion>();
    version->revision = context.revision;
    version->writeSequence = input->writeSequence;
    version->elementCount = input->elementCount;
    version->capacity = input->capacity;
    version->elementStride = input->elementStride;
    version->resource = resource;
    version->cpuShadow = std::move(shadow);

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = input->catalogOwner;
    root->publishRoot = false;
    root->fragment.revision = context.revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedGpuBufferVersion>(version);
    auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    resources->push_back(resource);
    root->catalogEntries.emplace_back(PublishedResourceKey{
        input->catalogOwner, input->catalogUsage, 0, 0, input->catalogVariant }, resources);
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)), TokenForTicket(ticket));
}

} // namespace

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph) {
    const ArtifactProducerRegistration registration{
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "VersionedGpuBufferArtifact::Build", BuildVersionedGpuBuffer };
    graph.RegisterProducer(ArtifactKind::BufferVersion, registration);
    graph.RegisterProducer(ArtifactKind::ActiveDrawList, registration);
}

} // namespace br::render
