#include "Render/VersionedGpuBufferArtifacts.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <limits>
#include <mutex>

#include "Render/Runtime/IUploadService.h"
#include "Render/RendererStateRequestService.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Utilities/Utilities.h"
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {
std::atomic_uint64_t g_pooledBackingCount{ 0 };
std::atomic_uint64_t g_pooledBackingBytes{ 0 };

void EmitBackingPoolTelemetry() {
    if (!basic_telemetry::Enabled()) return;
    basic_telemetry::SetGauge("SARP.VersionedBuffer.PooledBackings",
        static_cast<std::int64_t>(g_pooledBackingCount.load(std::memory_order_relaxed)));
    basic_telemetry::SetGauge("SARP.VersionedBuffer.PooledBackingBytes",
        static_cast<std::int64_t>(g_pooledBackingBytes.load(std::memory_order_relaxed)));
}
}

VersionedGpuBufferBackingPool::~VersionedGpuBufferBackingPool() {
    std::lock_guard lock(m_mutex);
    for (const auto& backing : m_backings) {
        if (!backing) continue;
        g_pooledBackingCount.fetch_sub(1, std::memory_order_relaxed);
        g_pooledBackingBytes.fetch_sub(
            backing->byteCapacity, std::memory_order_relaxed);
    }
    EmitBackingPoolTelemetry();
}

std::shared_ptr<BufferBackingArtifact> VersionedGpuBufferBackingPool::Acquire(
    std::uint64_t capacityClass, std::uint32_t elementStride,
    bool unorderedAccess, bool indirectArguments, std::string_view debugName,
    bool& expanded) {
    std::lock_guard lock(m_mutex);
    // The pool is the only owner allowed to recycle a backing. A published
    // catalog can retain the Resource independently of its backing artifact,
    // so both ownership counts must be exclusive before reuse. Reclaim idle
    // capacity classes eagerly; keeping every historical growth class made
    // streaming traversal permanently retain their VRAM.
    std::shared_ptr<BufferBackingArtifact> reusable;
    for (auto it = m_backings.begin(); it != m_backings.end();) {
        const auto& backing = *it;
        const bool idle = backing && backing.use_count() == 1 && backing->resource.use_count() == 1;
        if (idle && backing->capacityClass == capacityClass && !reusable) {
            reusable = backing;
            ++it;
            continue;
        }
        if (idle) {
            g_pooledBackingCount.fetch_sub(1, std::memory_order_relaxed);
            g_pooledBackingBytes.fetch_sub(
                backing->byteCapacity, std::memory_order_relaxed);
            it = m_backings.erase(it);
            basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingReclaimed");
            continue;
        }
        ++it;
    }
    if (reusable) {
            expanded = false;
            basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingReused");
            EmitBackingPoolTelemetry();
            return reusable;
    }
    Resource::ScopedECSRegistrationSuppression suppressECS;
    auto resource = CreateIndexedStructuredBuffer(
        static_cast<std::uint32_t>((std::max<std::uint64_t>)(capacityClass, 1u)),
        elementStride, unorderedAccess, indirectArguments);
    resource->SetName(std::string(debugName));
    auto backing = std::make_shared<BufferBackingArtifact>();
    backing->resource = std::move(resource);
    backing->backingGeneration = m_nextGeneration++;
    backing->capacityClass = capacityClass;
    backing->byteCapacity = capacityClass * elementStride;
    m_backings.push_back(backing);
    g_pooledBackingCount.fetch_add(1, std::memory_order_relaxed);
    g_pooledBackingBytes.fetch_add(capacityClass * elementStride, std::memory_order_relaxed);
    basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingAllocated");
    EmitBackingPoolTelemetry();
    expanded = true;
    return backing;
}

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
    write.bytes = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
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
    std::vector<VersionedGpuBufferWrite> sealed;
    sealed.reserve(m_writes.size());
    for (const auto& write : m_writes) {
        if (!write.bytes || write.bytes->empty()) {
            sealed.push_back(write);
            continue;
        }
        if (!sealed.empty() && sealed.back().bytes &&
            sealed.back().elementOffset + sealed.back().bytes->size() / m_elementStride ==
                write.elementOffset) {
            auto bytes = std::const_pointer_cast<std::vector<std::byte>>(sealed.back().bytes);
            bytes->insert(bytes->end(), write.bytes->begin(), write.bytes->end());
            // The merged range closes every sequence through the later write.
            sealed.back().sequence = write.sequence;
            continue;
        }
        auto bytes = std::make_shared<std::vector<std::byte>>(
            write.bytes->begin(), write.bytes->end());
        sealed.push_back({ write.sequence, write.elementOffset, std::move(bytes) });
    }
    return Capture{ m_writeSequence, m_elementCount, m_capacity,
        m_previous, std::move(sealed), m_previous ? std::vector<std::byte>{} : m_initialBytes };
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

VersionedBufferFamily::VersionedBufferFamily(Config config)
    : m_config(std::move(config)),
      m_backingPool(std::make_shared<VersionedGpuBufferBackingPool>()) {
    if (m_config.elementStride == 0 || m_config.address.kind == ArtifactKind::Generic) {
        throw std::invalid_argument("invalid versioned buffer family configuration");
    }
}

ArtifactRequestResult VersionedBufferFamily::RequestSnapshot(
    RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
    std::uint64_t revision, std::span<const std::byte> bytes,
    std::uint64_t elementCount, std::uint64_t capacity) {
    if (revision == 0 || bytes.size() != elementCount * m_config.elementStride) {
        return { ArtifactRequestStatus::ConflictingRevision, 0, {} };
    }
    auto input = std::make_shared<VersionedGpuBufferBuildInput>();
    input->uploadService = &uploads;
    input->debugName = m_config.debugName;
    input->writeSequence = revision;
    input->elementStride = m_config.elementStride;
    input->elementCount = elementCount;
    input->capacity = (std::max<std::uint64_t>)((std::max)(capacity, elementCount), 1u);
    input->unorderedAccess = m_config.unorderedAccess;
    input->indirectArguments = m_config.indirectArguments;
    input->catalogOwner = m_config.catalogOwner;
    input->catalogUsage = m_config.catalogUsage;
    input->catalogVariant = m_config.catalogVariant;
    input->backingPool = m_backingPool;
    {
        std::lock_guard lock(m_mutex);
        input->previous = m_previous;
    }
    input->bytes.assign(bytes.begin(), bytes.end());
    auto fingerprint = revision ^ (m_config.catalogVariant << 17u) ^
        (elementCount << 1u) ^ (static_cast<std::uint64_t>(m_config.elementStride) << 33u) ^
        0x5642554646414dull;
    for (const auto value : bytes) {
        fingerprint ^= static_cast<std::uint8_t>(value);
        fingerprint *= 1099511628211ull;
    }
    if (fingerprint == 0) fingerprint = 1;
    return requests.Request(m_config.address, revision, {},
        ArtifactPayload::Make<VersionedGpuBufferBuildInput>(std::move(input)), fingerprint);
}

ArtifactRequestResult VersionedBufferFamily::RequestCapture(
    RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
    std::uint64_t revision, VersionedGpuBufferJournal::Capture capture) {
    if (revision == 0 || capture.writeSequence != revision) {
        return { ArtifactRequestStatus::ConflictingRevision, 0, {} };
    }
    auto input = std::make_shared<VersionedGpuBufferBuildInput>();
    input->uploadService = &uploads;
    input->debugName = m_config.debugName;
    input->writeSequence = capture.writeSequence;
    input->elementStride = m_config.elementStride;
    input->elementCount = capture.elementCount;
    input->capacity = capture.capacity;
    input->unorderedAccess = m_config.unorderedAccess;
    input->indirectArguments = m_config.indirectArguments;
    input->catalogOwner = m_config.catalogOwner;
    input->catalogUsage = m_config.catalogUsage;
    input->catalogVariant = m_config.catalogVariant;
    input->backingPool = m_backingPool;
    input->previous = std::move(capture.previous);
    input->writes = std::move(capture.writes);
    input->bytes = std::move(capture.initialBytes);
    std::uint64_t fingerprint = revision ^ (capture.elementCount << 1u) ^
        (capture.capacity << 7u) ^ (m_config.catalogVariant << 17u) ^ 0x5642464a4f5552ull;
    const auto hashBytes = [&fingerprint](std::span<const std::byte> values) {
        for (const auto value : values) {
            fingerprint ^= static_cast<std::uint8_t>(value);
            fingerprint *= 1099511628211ull;
        }
    };
    hashBytes(input->bytes);
    for (const auto& write : input->writes) {
        fingerprint ^= write.sequence ^ (write.elementOffset << 3u);
        if (write.bytes) hashBytes(*write.bytes);
    }
    if (fingerprint == 0) fingerprint = 1;
    return requests.Request(m_config.address, revision, {},
        ArtifactPayload::Make<VersionedGpuBufferBuildInput>(std::move(input)), fingerprint);
}

void VersionedBufferFamily::Acknowledge(
    std::shared_ptr<const PublishedGpuBufferVersion> version) {
    if (!version) return;
    std::lock_guard lock(m_mutex);
    if (!m_previous || version->revision > m_previous->revision) m_previous = std::move(version);
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
        const auto& bytes = write.bytes;
        if (bytes && bytes->size() % input.elementStride != 0u) {
            error = "versioned buffer journal write is not row aligned";
            return {};
        }
        if (write.elementOffset > (std::numeric_limits<std::size_t>::max)() / input.elementStride) {
            error = "versioned buffer journal offset overflows address space";
            return {};
        }
        const auto byteOffset = static_cast<std::size_t>(write.elementOffset * input.elementStride);
        const auto byteCount = bytes ? bytes->size() : 0u;
        if (byteOffset > shadow->size() || byteCount > shadow->size() - byteOffset) {
            error = "versioned buffer journal write exceeds desired size";
            return {};
        }
        if (bytes) std::copy(bytes->begin(), bytes->end(), shadow->begin() + byteOffset);
        lastSequence = write.sequence;
    }
    // A full snapshot is self-contained. Its revision need not be contiguous
    // with the previously published journal sequence; only delta captures must
    // prove that their journal closes through the requested sequence.
    if (input.bytes.empty() && ((!input.writes.empty() && lastSequence != input.writeSequence) ||
        (input.writes.empty() && input.previous && input.writeSequence != input.previous->writeSequence))) {
        error = "versioned buffer is not closed through requested write sequence";
        return {};
    }
    return shadow;
}

namespace {

std::shared_ptr<const GpuSubmissionSet> TokenForTicket(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuSubmissionSet>();
    GpuQueueSubmission submission;
    {
        std::lock_guard lock(ticket->timelineMutex);
        submission.timelineOwner = ticket->timelineOwner;
        submission.value = ticket->timelineValue;
    }
    token->isComplete = [ticket] { return ticket->Complete(); };
    token->isSubmitted = [ticket] {
        const auto state = ticket->state.load(std::memory_order_acquire);
        return state == org::TrackedUploadTicketState::Submitted ||
            state == org::TrackedUploadTicketState::Completed;
    };
    submission.currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    submission.currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
	token->submissions.push_back(std::move(submission));
	token->describe = [ticket] {
		const auto state = ticket->state.load(std::memory_order_acquire);
		std::lock_guard lock(ticket->timelineMutex);
		const bool timelineComplete = ticket->isTimelineComplete &&
			ticket->isTimelineComplete(ticket->timelineValue);
		return std::format("ticket-state={} timeline-complete={}",
			static_cast<unsigned>(state), timelineComplete);
	};
	token->subscribe = [ticket](std::function<void()> callback) {
        ticket->SetChangeCallback(callback);
        if (callback) callback();
    };
	token->cancel = [ticket] { return ticket->Cancel(); };
    return token;
}

std::shared_ptr<const GpuSubmissionSet> TokenForTickets(
    std::vector<std::shared_ptr<org::TrackedUploadTicket>> tickets) {
    std::erase(tickets, nullptr);
    if (tickets.empty()) return {};
    if (tickets.size() == 1) return TokenForTicket(tickets.front());
    auto token = std::make_shared<GpuSubmissionSet>();
    auto shared = std::make_shared<const std::vector<std::shared_ptr<org::TrackedUploadTicket>>>(
        std::move(tickets));
    token->submissions.reserve(shared->size());
    for (const auto& ticket : *shared) {
        GpuQueueSubmission submission;
        {
            std::lock_guard lock(ticket->timelineMutex);
            submission.timelineOwner = ticket->timelineOwner;
            submission.value = ticket->timelineValue;
        }
        submission.currentTimelineOwner = [ticket] {
            std::lock_guard lock(ticket->timelineMutex);
            return ticket->timelineOwner;
        };
        submission.currentValue = [ticket] {
            std::lock_guard lock(ticket->timelineMutex);
            return ticket->timelineValue;
        };
        token->submissions.push_back(std::move(submission));
    }
    token->isComplete = [shared] {
        return std::ranges::all_of(*shared, [](const auto& ticket) { return ticket->Complete(); });
    };
    token->isSubmitted = [shared] {
        return std::ranges::all_of(*shared, [](const auto& ticket) {
            const auto state = ticket->state.load(std::memory_order_acquire);
            return state == org::TrackedUploadTicketState::Submitted ||
                state == org::TrackedUploadTicketState::Completed;
        });
    };
    token->subscribe = [shared](std::function<void()> callback) {
        for (const auto& ticket : *shared) ticket->SetChangeCallback(callback);
        if (callback) callback();
    };
    token->cancel = [shared] {
        bool cancelled = false;
        for (const auto& ticket : *shared) cancelled = ticket->Cancel() || cancelled;
        return cancelled;
    };
    token->describe = [shared] { return std::format("batched-tickets={}", shared->size()); };
    return token;
}

std::uint64_t ReplacementCapacity(const VersionedGpuBufferBuildInput& input) {
    const auto required = (std::max<std::uint64_t>)(input.capacity, 1u);
    const auto rounded = std::bit_ceil(required);
    if (!input.previous) return rounded;
    const auto grown = input.previous->capacity + (std::max<std::uint64_t>)(input.previous->capacity / 2u, 1u);
    return (std::max)(rounded, grown);
}

ArtifactBuildResult BuildVersionedGpuBuffer(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<VersionedGpuBufferBuildInput>();
    if (!input || !input->uploadService || input->elementStride == 0u) {
        return ArtifactBuildResult::Failure("versioned buffer input/upload service/stride missing");
    }
    std::string replayError;
    auto shadow = ReplayVersionedGpuBufferShadow(*input, replayError);
    if (!shadow) return ArtifactBuildResult::Failure(std::move(replayError));

    const bool fitsBacking = input->previous && input->previous->resource &&
        input->capacity <= input->previous->capacity;
    const bool appendOnly = fitsBacking && input->bytes.empty() &&
        std::ranges::all_of(input->writes, [&](const auto& write) {
        return !write.bytes || write.elementOffset >= input->previous->elementCount;
    });
    const auto mode = fitsBacking ? BufferRevisionMode::Patch : BufferRevisionMode::Replace;
    auto pool = input->backingPool ? input->backingPool
                                   : std::make_shared<VersionedGpuBufferBackingPool>();
    std::shared_ptr<BufferBackingArtifact> backing;
    bool backingExpanded = false;
    if (appendOnly) {
        backing = input->previous->backing;
        if (!backing) {
            backing = std::make_shared<BufferBackingArtifact>();
            backing->resource = input->previous->resource;
            backing->capacityClass = input->previous->capacity;
        }
    } else {
        const auto capacityClass = mode == BufferRevisionMode::Replace
            ? ReplacementCapacity(*input) : input->previous->capacity;
        backing = pool->Acquire(capacityClass, input->elementStride,
            input->unorderedAccess, input->indirectArguments, input->debugName,
            backingExpanded);
    }
    auto resource = backing->resource;
    basic_telemetry::AddCounter(mode == BufferRevisionMode::Patch
        ? "SARP.VersionedBuffer.Patch" : "SARP.VersionedBuffer.Replace");
    if (mode == BufferRevisionMode::Patch && backingExpanded) {
        basic_telemetry::AddCounter("SARP.VersionedBuffer.PatchBackingExpansion");
    }

    std::vector<std::shared_ptr<org::TrackedUploadTicket>> tickets;
    // Indirect argument buffers are transient UAV outputs. The culling pass
    // writes every command in the published logical range before ExecuteIndirect
    // consumes it, so uploading the producer's zero-filled CPU shadow is both
    // unnecessary and actively harmful: thousands of view/workload buffers can
    // otherwise serialize behind the tracked copy timeline during scene load.
    const bool needsInitialContent = !(input->unorderedAccess &&
        input->indirectArguments && input->bytes.empty() && input->writes.empty());
    if (appendOnly) {
        for (const auto& write : input->writes) {
            if (!write.bytes || write.bytes->empty()) continue;
            tickets.push_back(input->uploadService->QueueTrackedStreamingUpload(
                write.bytes->data(), write.bytes->size(), resource,
                write.elementOffset * input->elementStride));
        }
    } else if (needsInitialContent && !shadow->empty()) {
        tickets.push_back(input->uploadService->QueueTrackedStreamingUpload(
            shadow->data(), shadow->size(), resource, 0));
    } else if (!needsInitialContent) {
        basic_telemetry::AddCounter("SARP.VersionedBuffer.TransientInitializationSkipped");
    }

    auto version = std::make_shared<PublishedGpuBufferVersion>();
    version->revision = context.revision;
    version->writeSequence = input->writeSequence;
    version->elementCount = input->elementCount;
    version->capacity = backing->capacityClass;
    version->elementStride = input->elementStride;
    version->revisionMode = mode;
    version->contentVersion = context.revision;
    version->backing = std::move(backing);
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
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)),
        TokenForTickets(std::move(tickets)));
}

} // namespace

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph) {
    const ArtifactProducerRegistration registration{
        TaskLane::Streaming, TaskDomain::RendererState,
        "VersionedGpuBufferArtifact::Build", BuildVersionedGpuBuffer };
    graph.RegisterProducer(ArtifactKind::BufferVersion, registration);
    graph.RegisterProducer(ArtifactKind::ActiveDrawList, registration);
}

} // namespace br::render
