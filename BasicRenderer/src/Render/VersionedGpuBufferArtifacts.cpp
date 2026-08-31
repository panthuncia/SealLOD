#include "Render/VersionedGpuBufferArtifacts.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <limits>
#include <mutex>
#include <numeric>

#include "Render/Runtime/IUploadService.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/RendererStateRequestService.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Utilities/Utilities.h"
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {
std::mutex g_backingRetirementMutex;
std::vector<std::weak_ptr<VersionedGpuBufferBackingPool>> g_backingRetirementWaiters;
std::atomic_uint64_t g_backingFrameRetirementEpoch{ 0 };

void RegisterBackingRetirementWaiter(
    const std::shared_ptr<VersionedGpuBufferBackingPool>& pool) {
    if (!pool) return;
    std::lock_guard lock(g_backingRetirementMutex);
    g_backingRetirementWaiters.push_back(pool);
}

std::atomic_uint64_t g_pooledBackingCount{ 0 };
std::atomic_uint64_t g_pooledBackingBytes{ 0 };

void EmitBackingPoolTelemetry() {
    if (!basic_telemetry::Enabled()) return;
    basic_telemetry::SetGauge("SARP.VersionedBuffer.PooledBackings",
        static_cast<std::int64_t>(g_pooledBackingCount.load(std::memory_order_relaxed)));
    basic_telemetry::SetGauge("SARP.VersionedBuffer.PooledBackingBytes",
        static_cast<std::int64_t>(g_pooledBackingBytes.load(std::memory_order_relaxed)));
}

struct ObjectBufferMetricNames {
    const char* appendOnly;
    const char* replace;
    const char* patchExpansion;
    const char* allocatedBytes;
    const char* capacityElements;
    const char* logicalElements;
};

const ObjectBufferMetricNames* ObjectBufferMetrics(const VersionedGpuBufferBuildInput& input) {
    if (!std::string_view(input.debugName).starts_with("Published::")) return nullptr;
    static constexpr ObjectBufferMetricNames perObject{
        "SARP.VersionedBuffer.Object.PerObject.AppendOnly",
        "SARP.VersionedBuffer.Object.PerObject.Replace",
        "SARP.VersionedBuffer.Object.PerObject.PatchBackingExpansion",
        "SARP.VersionedBuffer.Object.PerObject.AllocatedBackingBytes",
        "SARP.VersionedBuffer.Object.PerObject.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.PerObject.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames transforms{
        "SARP.VersionedBuffer.Object.Transform.AppendOnly",
        "SARP.VersionedBuffer.Object.Transform.Replace",
        "SARP.VersionedBuffer.Object.Transform.PatchBackingExpansion",
        "SARP.VersionedBuffer.Object.Transform.AllocatedBackingBytes",
        "SARP.VersionedBuffer.Object.Transform.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.Transform.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames drawRecords{
        "SARP.VersionedBuffer.Object.DrawRecord.AppendOnly",
        "SARP.VersionedBuffer.Object.DrawRecord.Replace",
        "SARP.VersionedBuffer.Object.DrawRecord.PatchBackingExpansion",
        "SARP.VersionedBuffer.Object.DrawRecord.AllocatedBackingBytes",
        "SARP.VersionedBuffer.Object.DrawRecord.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.DrawRecord.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames normals{
        "SARP.VersionedBuffer.Object.NormalMatrix.AppendOnly",
        "SARP.VersionedBuffer.Object.NormalMatrix.Replace",
        "SARP.VersionedBuffer.Object.NormalMatrix.PatchBackingExpansion",
        "SARP.VersionedBuffer.Object.NormalMatrix.AllocatedBackingBytes",
        "SARP.VersionedBuffer.Object.NormalMatrix.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.NormalMatrix.LogicalElementCount" };
    switch (input.catalogVariant) {
    case kObjectPerObjectVariant: return &perObject;
    case kObjectInstanceTransformVariant: return &transforms;
    case kObjectDrawRecordVariant: return &drawRecords;
    case kObjectNormalMatrixVariant: return &normals;
    default: return nullptr;
    }
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

std::uint64_t VersionedGpuBufferBackingPool::SubscribeAvailability(
    std::function<void(std::uint64_t)> callback) {
    if (!callback) return 0;
    const auto identity = AsyncStateGraph::AllocateSuspensionIdentity();
    bool registerPool = false;
    {
        std::lock_guard lock(m_mutex);
        m_waiters.emplace_back(identity, std::move(callback));
        registerPool = !m_registeredForRetirementWake;
        m_registeredForRetirementWake = true;
    }
    if (registerPool) RegisterBackingRetirementWaiter(shared_from_this());
    basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingRetirementSuspensions");
    return identity;
}

void VersionedGpuBufferBackingPool::NotifyAvailability() noexcept {
    std::vector<std::pair<std::uint64_t, std::function<void(std::uint64_t)>>> waiters;
    {
        std::lock_guard lock(m_mutex);
        waiters.swap(m_waiters);
        m_registeredForRetirementWake = false;
    }
    for (auto& [identity, callback] : waiters) {
        try { if (callback) callback(identity); }
        catch (...) {}
    }
    if (!waiters.empty()) {
        basic_telemetry::AddCounter(
            "SARP.VersionedBuffer.BackingRetirementWakes", waiters.size());
    }
}

void NotifyVersionedGpuBufferFrameRetirement() noexcept {
	g_backingFrameRetirementEpoch.fetch_add(1, std::memory_order_release);
    std::vector<std::weak_ptr<VersionedGpuBufferBackingPool>> waiting;
    {
        std::lock_guard lock(g_backingRetirementMutex);
        waiting.swap(g_backingRetirementWaiters);
    }
    for (auto& weak : waiting) {
        if (auto pool = weak.lock()) pool->NotifyAvailability();
    }
}

std::uint64_t VersionedGpuBufferFrameRetirementEpoch() noexcept {
	return g_backingFrameRetirementEpoch.load(std::memory_order_acquire);
}

void VersionedGpuBufferBackingPool::Retire(std::uint64_t backingGeneration) noexcept {
    if (backingGeneration == 0) return;
    std::lock_guard lock(m_mutex);
    const auto found = std::ranges::find_if(m_backings, [&](const auto& backing) {
        return backing && backing->backingGeneration == backingGeneration;
    });
    if (found == m_backings.end()) return;
    const auto byteCapacity = (*found)->byteCapacity;
    m_backings.erase(found);
    g_pooledBackingCount.fetch_sub(1, std::memory_order_relaxed);
    g_pooledBackingBytes.fetch_sub(byteCapacity, std::memory_order_relaxed);
    basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingRetired");
    EmitBackingPoolTelemetry();
}

void VersionedGpuBufferBackingPool::AcknowledgePublished(
    std::uint64_t backingGeneration, std::uint32_t framesInFlight) noexcept {
    if (backingGeneration == 0) return;
    std::lock_guard lock(m_mutex);
    const auto retirementEpoch = VersionedGpuBufferFrameRetirementEpoch();
    // GPU safety starts when the old backing stops being the active published
    // table, not when it first became active. A table may remain current for
    // thousands of frames; using its original publication epoch allowed it to
    // be rewritten immediately after replacement while older frame slots still
    // referenced its descriptors.
    if (m_activePublishedGeneration != 0 &&
        m_activePublishedGeneration != backingGeneration) {
        const auto previous = std::ranges::find_if(m_backings, [&](const auto& backing) {
            return backing && backing->backingGeneration == m_activePublishedGeneration;
        });
        if (previous != m_backings.end()) {
            (*previous)->lastPublishedRetirementEpoch = retirementEpoch;
        }
    }
    m_activePublishedGeneration = backingGeneration;
    m_framesInFlight = (std::max)(framesInFlight, 1u);
    const auto found = std::ranges::find_if(m_backings, [&](const auto& backing) {
        return backing && backing->backingGeneration == backingGeneration;
    });
    if (found != m_backings.end()) {
        (*found)->wasPublished = true;
        // This value is replaced with the actual retirement epoch when a
        // successor becomes published.
        (*found)->lastPublishedRetirementEpoch = retirementEpoch;
    }
}

PublishedGpuBufferVersion::~PublishedGpuBufferVersion() {
    // Pool wakeups are issued after the containing artifact or frame state has
    // completed destruction. Calling graph continuations from this destructor
    // can re-enter scheduling while sibling catalog holds are being torn down.
}

std::shared_ptr<BufferBackingArtifact> VersionedGpuBufferBackingPool::Acquire(
    std::uint64_t capacityClass, std::uint32_t elementStride,
    bool unorderedAccess, bool indirectArguments, std::string_view debugName,
    bool& expanded) {
    std::lock_guard lock(m_mutex);
    // The backing artifact is the explicit reuse lease. Render-graph and upload
    // infrastructure can retain the Resource object after all semantic users
    // have retired, so Resource::use_count is not a valid safety signal and
    // permanently exhausted the ring. Every published version and catalog
    // selection retains the backing artifact; pool-only ownership is therefore
    // the authoritative indication that the allocation is safe to rewrite.
    // Reclaim idle capacity classes eagerly; keeping every historical growth
    // class made streaming traversal permanently retain their VRAM.
    std::shared_ptr<BufferBackingArtifact> reusable;
    std::size_t matchingCapacityClass = 0;
    for (auto it = m_backings.begin(); it != m_backings.end();) {
        const auto& backing = *it;
        if (backing && backing->capacityClass == capacityClass) ++matchingCapacityClass;
        const auto retirementEpoch = VersionedGpuBufferFrameRetirementEpoch();
        const bool publicationRetired = backing && backing->wasPublished &&
            backing->backingGeneration != m_activePublishedGeneration &&
            retirementEpoch >= backing->lastPublishedRetirementEpoch + m_framesInFlight;
        const bool idle = backing &&
            (backing.use_count() == 1 || publicationRetired);
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
    // A full post-publication frame-slot rotation ensures the three frame
    // leases no longer span several mutable epochs before the next build is
    // admitted. At most frames-in-flight plus one unpublished successor are
    // therefore distinct. Keep the bound explicit: exhaustion suspends and
    // resumes from retirement rather than allocating past it.
    const auto maximumBackingsPerCapacityClass =
        static_cast<std::size_t>(m_framesInFlight) + 1u;
    if (matchingCapacityClass >= maximumBackingsPerCapacityClass) {
        expanded = false;
        basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingRingExhausted");
        return {};
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
	m_desiredBytes = std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end());
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
	if (!m_desiredBytes) m_desiredBytes = std::make_shared<std::vector<std::byte>>();
	else if (m_desiredBytes.use_count() != 1)
		m_desiredBytes = std::make_shared<std::vector<std::byte>>(*m_desiredBytes);
	// resultingElementCount describes the extent reached by this mutation. Sparse
	// allocator writes can target an older/lower range after a larger range has
	// already established the journal's logical extent; never truncate the
	// authoritative image in that case.
	const auto logicalElementCount = (std::max)(m_elementCount, resultingElementCount);
	const auto desiredSize = static_cast<std::size_t>(logicalElementCount * m_elementStride);
	m_desiredBytes->resize(desiredSize);
	std::copy(bytes.begin(), bytes.end(),
		m_desiredBytes->begin() + static_cast<std::size_t>(elementOffset * m_elementStride));
    m_elementCount = logicalElementCount;
    return m_writeSequence;
}

std::uint64_t VersionedGpuBufferJournal::ReplaceImage(
    std::span<const std::byte> bytes, std::uint64_t elementCount,
    std::uint64_t capacity) {
    if (m_elementStride == 0 || elementCount > capacity ||
        bytes.size() != elementCount * m_elementStride) {
        throw std::invalid_argument("invalid versioned GPU buffer replacement image");
    }
    std::lock_guard lock(m_mutex);
    m_capacity = (std::max)(m_capacity, capacity);
    m_elementCount = elementCount;
    ++m_writeSequence;
    m_desiredBytes = std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end());
    auto replacement = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    m_writes.push_back(VersionedGpuBufferWrite{
        m_writeSequence, 0u, std::move(replacement) });
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
    // AppendWrite seals every byte range in an immutable shared allocation.
    // A capture therefore only needs to copy the compact descriptors. Deep
    // copying and coalescing here made request construction proportional to all
    // unpublished bytes and put multi-megabyte memcpy/insert work on callers
    // such as the render host. Replay is a graph producer operation and can
    // consume adjacent descriptors directly.
    Capture capture;
	capture.writeSequence = m_writeSequence;
	capture.elementCount = m_elementCount;
	capture.capacity = m_capacity;
	capture.previous = m_previous;
	capture.writes = m_writes;
	capture.initialBytes = m_previous ? std::vector<std::byte>{} : m_initialBytes;
	capture.desiredBytes = m_desiredBytes;
	capture.journalBaseSequence = m_previous ? m_previous->writeSequence : 0u;
	return capture;
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
    input->gpuWritten = m_config.gpuWritten;
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
    return requests.SubmitLatest({ m_config.address, revision, {},
        ArtifactPayload::Make<VersionedGpuBufferBuildInput>(std::move(input)), fingerprint });
}

ArtifactRequestResult VersionedBufferFamily::RequestGpuWritten(
    RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
    std::uint64_t revision, std::uint64_t elementCount, std::uint64_t capacity) {
    if (revision == 0 || !m_config.gpuWritten) {
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
    input->gpuWritten = true;
    input->catalogOwner = m_config.catalogOwner;
    input->catalogUsage = m_config.catalogUsage;
    input->catalogVariant = m_config.catalogVariant;
    input->backingPool = m_backingPool;
    const auto fingerprint = revision ^ (elementCount << 1u) ^
        (input->capacity << 7u) ^ (m_config.catalogVariant << 17u) ^ 0x4750555752495445ull;
    return requests.SubmitLatest({ m_config.address, revision, {},
        ArtifactPayload::Make<VersionedGpuBufferBuildInput>(std::move(input)),
        fingerprint != 0u ? fingerprint : 1u });
}

ArtifactRequestResult VersionedBufferFamily::RequestContentSnapshot(
    RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
    std::span<const std::byte> bytes, std::uint64_t elementCount,
    std::uint64_t capacity) {
    std::uint64_t contentFingerprint = 1469598103934665603ull;
    for (const auto value : bytes) {
        contentFingerprint ^= static_cast<std::uint8_t>(value);
        contentFingerprint *= 1099511628211ull;
    }
    contentFingerprint ^= elementCount + (capacity << 1u);
    std::uint64_t contentRevision = 0;
    {
        std::lock_guard lock(m_mutex);
        const auto [found, inserted] = m_contentRevisions.try_emplace(
            contentFingerprint, 0u);
        if (inserted) found->second = ++m_nextContentRevision;
        contentRevision = found->second;
    }
    return RequestSnapshot(requests, uploads, contentRevision, bytes,
        elementCount, capacity);
}

ArtifactRequestResult VersionedBufferFamily::RequestCapture(
    RendererStateRequestService& requests, org::runtime::IUploadService& uploads,
    std::uint64_t revision, VersionedGpuBufferJournal::Capture capture) {
    if (revision == 0 || capture.writeSequence != revision) {
        return { ArtifactRequestStatus::ConflictingRevision, 0, {} };
    }
    // A journal sequence names its semantic desired image. Acknowledge may
    // subsequently compact the same image from a full replay into
    // previous+delta form, but that representation change must not mint a new
    // fingerprint for the already-requested immutable version.
    std::lock_guard familyLock(m_mutex);
    if (revision == m_lastJournalRevision && m_lastJournalHandle) {
        return { ArtifactRequestStatus::AlreadyDesired,
            m_lastJournalHandle.version.generation,
            m_lastJournalHandle.version,
            m_lastJournalHandle.lease };
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
	input->desiredBytes = std::move(capture.desiredBytes);
	input->journalBaseSequence = capture.journalBaseSequence;
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
    auto result = requests.SubmitLatest({ m_config.address, revision, {},
        ArtifactPayload::Make<VersionedGpuBufferBuildInput>(std::move(input)), fingerprint });
    if (result) {
        m_lastJournalRevision = revision;
        m_lastJournalHandle = result.Handle();
    }
    return result;
}

void VersionedBufferFamily::Acknowledge(
    std::shared_ptr<const PublishedGpuBufferVersion> version) {
    if (!version) return;
    if (auto pool = version->backingPool.lock(); version->backing) {
        pool->AcknowledgePublished(version->backing->backingGeneration, 3u);
    }
    std::lock_guard lock(m_mutex);
    if (!m_previous || version->revision > m_previous->revision) m_previous = std::move(version);
}

std::shared_ptr<const std::vector<std::byte>> ReplayVersionedGpuBufferAuthoritativeState(
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
	if (input.desiredBytes) {
		if (input.desiredBytes->size() != requiredBytes) {
			error = "versioned buffer desired image byte count does not match rows";
			return {};
		}
		return input.desiredBytes;
	}
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

ArtifactBuildResult BuildVersionedGpuBuffer(const ArtifactBuildContext& context,
    const std::function<void(std::uint64_t)>& notifySuspension) {
    const auto input = context.input.Get<VersionedGpuBufferBuildInput>();
    if (!input || !input->uploadService || input->elementStride == 0u) {
        return ArtifactBuildResult::Failure("versioned buffer input/upload service/stride missing");
    }
    if (context.stopRequested && context.stopRequested()) return ArtifactBuildResult::Cancelled();
    const bool fitsBacking = input->previous && input->previous->resource &&
        input->capacity <= input->previous->capacity;
    const bool appendOnly = fitsBacking && input->bytes.empty() &&
        std::ranges::all_of(input->writes, [&](const auto& write) {
        return !write.bytes || write.elementOffset >= input->previous->elementCount;
    });
    const auto mode = fitsBacking ? BufferRevisionMode::Patch : BufferRevisionMode::Replace;
    auto pool = input->backingPool ? input->backingPool
                                   : std::make_shared<VersionedGpuBufferBackingPool>();
    if (appendOnly) basic_telemetry::AddCounter("SARP.VersionedBuffer.AppendOnly");
    const auto capacityClass = mode == BufferRevisionMode::Replace
        ? ReplacementCapacity(*input) : input->previous->capacity;
    bool backingExpanded = false;
    auto backing = pool->Acquire(capacityClass, input->elementStride,
        input->unorderedAccess, input->indirectArguments, input->debugName,
        backingExpanded);
    if (!backing) {
        const auto identity = pool->SubscribeAvailability(
            notifySuspension);
        return ArtifactBuildResult::Suspend(ArtifactSuspension::Capacity(
            identity, "versioned GPU buffer backing ring exhausted"));
    }
    // Do not replay the CPU journal until a bounded backing-ring slot is
    // available. Ring exhaustion is an expected transient state; replaying on
    // every retry multiplied host work while waiting for frame retirement.
	std::string replayError;
	auto shadow = input->gpuWritten
		? std::make_shared<const std::vector<std::byte>>()
		: ReplayVersionedGpuBufferAuthoritativeState(*input, replayError);
    if (!shadow) return ArtifactBuildResult::Failure(std::move(replayError));
	if (input->desiredBytes) {
		basic_telemetry::AddCounter("SARP.VersionedBuffer.AuthoritativeStateReused");
		basic_telemetry::Record("SARP.VersionedBuffer.BytesReplayed", 0);
	} else {
		basic_telemetry::Record("SARP.VersionedBuffer.BytesReplayed", shadow->size());
	}
    if (context.stopRequested && context.stopRequested()) return ArtifactBuildResult::Cancelled();

    auto resource = backing->resource;
	basic_telemetry::Record("SARP.VersionedBuffer.RequestedCapacityElements", input->capacity);
	basic_telemetry::Record("SARP.VersionedBuffer.BackingCapacityElements", backing->capacityClass);
	basic_telemetry::Record("SARP.VersionedBuffer.LogicalElementCount", input->elementCount);
	if (backingExpanded) {
		basic_telemetry::Record("SARP.VersionedBuffer.AllocatedBackingBytes",
			backing->byteCapacity);
	}
    basic_telemetry::AddCounter(mode == BufferRevisionMode::Patch
        ? "SARP.VersionedBuffer.Patch" : "SARP.VersionedBuffer.Replace");
    if (mode == BufferRevisionMode::Patch && backingExpanded) {
        basic_telemetry::AddCounter("SARP.VersionedBuffer.PatchBackingExpansion");
    }
    if (const auto* metrics = ObjectBufferMetrics(*input)) {
        basic_telemetry::Record(metrics->capacityElements, backing->capacityClass);
        basic_telemetry::Record(metrics->logicalElements, input->elementCount);
        if (appendOnly) basic_telemetry::AddCounter(metrics->appendOnly);
        if (mode == BufferRevisionMode::Replace) basic_telemetry::AddCounter(metrics->replace);
        if (mode == BufferRevisionMode::Patch && backingExpanded) {
            basic_telemetry::AddCounter(metrics->patchExpansion);
        }
        if (backingExpanded) {
            basic_telemetry::Record(metrics->allocatedBytes, backing->byteCapacity);
        }
    }

    std::vector<std::shared_ptr<org::TrackedUploadTicket>> tickets;
    // Indirect argument buffers are transient UAV outputs. The culling pass
    // writes every command in the published logical range before ExecuteIndirect
    // consumes it, so uploading the producer's zero-filled CPU shadow is both
    // unnecessary and actively harmful: thousands of view/workload buffers can
    // otherwise serialize behind the tracked copy timeline during scene load.
    const bool needsInitialContent = !input->gpuWritten && !(input->unorderedAccess &&
        input->indirectArguments && input->bytes.empty() && input->writes.empty());
    std::uint64_t uploadedBytes = 0;
    std::uint64_t dirtyRangeCount = 0;
    if (needsInitialContent && !shadow->empty()) {
        struct DirtyRange { std::size_t offset = 0, size = 0; };
        std::vector<DirtyRange> dirtyRanges;
        const auto previousBackingShadow = backing->cpuShadow;
		const bool journalCoversBacking = !backingExpanded && input->desiredBytes &&
			backing->contentEpoch >= input->journalBaseSequence &&
			backing->contentEpoch <= input->writeSequence &&
			previousBackingShadow && previousBackingShadow->size() == shadow->size();
		if (journalCoversBacking) {
			for (const auto& write : input->writes) {
				if (write.sequence <= backing->contentEpoch || !write.bytes || write.bytes->empty()) continue;
				dirtyRanges.push_back({
					static_cast<std::size_t>(write.elementOffset * input->elementStride),
					write.bytes->size() });
			}
			std::ranges::sort(dirtyRanges, {}, &DirtyRange::offset);
			std::vector<DirtyRange> merged;
			for (const auto& range : dirtyRanges) {
				if (!merged.empty() && range.offset <= merged.back().offset + merged.back().size + 256u) {
					const auto end = (std::max)(merged.back().offset + merged.back().size,
						range.offset + range.size);
					merged.back().size = end - merged.back().offset;
				} else merged.push_back(range);
			}
			dirtyRanges = std::move(merged);
			basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingEpochJournalCatchup");
		} else if (!backingExpanded && previousBackingShadow &&
            previousBackingShadow->size() == shadow->size()) {
            constexpr std::size_t mergeGapBytes = 256;
            std::size_t cursor = 0;
            while (cursor < shadow->size()) {
                while (cursor < shadow->size() &&
                    (*previousBackingShadow)[cursor] == (*shadow)[cursor]) ++cursor;
                if (cursor == shadow->size()) break;
                const auto begin = cursor++;
                std::size_t lastDifference = begin;
                while (cursor < shadow->size()) {
                    if ((*previousBackingShadow)[cursor] != (*shadow)[cursor]) {
                        lastDifference = cursor;
                    } else if (cursor - lastDifference > mergeGapBytes) {
                        break;
                    }
                    ++cursor;
                }
                dirtyRanges.push_back({ begin, lastDifference - begin + 1u });
            }
        } else {
            dirtyRanges.push_back({ 0, shadow->size() });
        }
        const auto dirtyBytes = std::accumulate(dirtyRanges.begin(), dirtyRanges.end(),
            std::size_t{ 0 }, [](std::size_t total, const DirtyRange& range) {
                return total + range.size;
            });
        if (dirtyRanges.size() > 128u || dirtyBytes * 2u > shadow->size()) {
            dirtyRanges.assign(1u, DirtyRange{ 0, shadow->size() });
        }
        for (const auto& range : dirtyRanges) {
            tickets.push_back(input->uploadService->QueueTrackedStreamingUpload(
                shadow->data() + range.offset, range.size, resource, range.offset));
            uploadedBytes += range.size;
        }
        dirtyRangeCount = dirtyRanges.size();
    } else if (!needsInitialContent) {
        basic_telemetry::AddCounter("SARP.VersionedBuffer.TransientInitializationSkipped");
    }
    basic_telemetry::Record("SARP.VersionedBuffer.BytesUploaded", uploadedBytes);
    basic_telemetry::Record("SARP.VersionedBuffer.DirtyRanges", dirtyRangeCount);
    backing->contentEpoch = input->writeSequence;
    backing->cpuShadow = input->gpuWritten ? nullptr : shadow;

    auto version = std::make_shared<PublishedGpuBufferVersion>();
    version->revision = context.revision;
    version->writeSequence = input->writeSequence;
    version->elementCount = input->elementCount;
    version->capacity = backing->capacityClass;
    version->elementStride = input->elementStride;
    version->revisionMode = mode;
    version->contentVersion = context.revision;
    version->contentEpoch = input->writeSequence;
    version->backingEpoch = backing->backingGeneration;
    version->backing = std::move(backing);
    version->backingPool = pool;
    version->resource = resource;
    version->cpuShadow = input->gpuWritten ? nullptr : std::move(shadow);

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = input->catalogOwner;
    root->publishRoot = false;
    root->fragment.revision = context.revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedGpuBufferVersion>(version);
    root->fragment.resourceHolds.push_back(version->backing);
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
    auto notifySuspension = graph.MakeSuspensionNotifier();
    const ArtifactProducerRegistration registration{
        TaskLane::FrameCritical, TaskDomain::GpuBufferBuild,
        "VersionedGpuBufferArtifact::Build",
        [notifySuspension = std::move(notifySuspension)](
            const ArtifactBuildContext& context) {
            return BuildVersionedGpuBuffer(context, notifySuspension);
        } };
    graph.RegisterProducer(ArtifactKind::BufferVersion, registration);
    graph.RegisterProducer(ArtifactKind::ActiveDrawList, registration);
}

} // namespace br::render
