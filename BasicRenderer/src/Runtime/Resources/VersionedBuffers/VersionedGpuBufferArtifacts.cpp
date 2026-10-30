#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "Render/PublicationBindingBundle.h"
#include "Render/RenderGraph/ExecutionState.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/RendererStateArtifacts.h>
#include <ORGModuleServices/Async/VersionedBufferProducer.h>
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Utilities/Utilities.h"
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {

struct ObjectBufferMetricNames {
    const char* appendOnly;
    const char* replace;
    const char* capacityElements;
    const char* logicalElements;
};

const ObjectBufferMetricNames* ObjectBufferMetrics(const VersionedGpuBufferBuildInput& input, std::uint64_t variant) {
    if (!std::string_view(input.debugName).starts_with("Published::")) return nullptr;
    static constexpr ObjectBufferMetricNames perObject{
        "SARP.VersionedBuffer.Object.PerObject.AppendOnly",
        "SARP.VersionedBuffer.Object.PerObject.Replace",
        "SARP.VersionedBuffer.Object.PerObject.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.PerObject.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames transforms{
        "SARP.VersionedBuffer.Object.Transform.AppendOnly",
        "SARP.VersionedBuffer.Object.Transform.Replace",
        "SARP.VersionedBuffer.Object.Transform.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.Transform.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames drawRecords{
        "SARP.VersionedBuffer.Object.DrawRecord.AppendOnly",
        "SARP.VersionedBuffer.Object.DrawRecord.Replace",
        "SARP.VersionedBuffer.Object.DrawRecord.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.DrawRecord.LogicalElementCount" };
    static constexpr ObjectBufferMetricNames normals{
        "SARP.VersionedBuffer.Object.NormalMatrix.AppendOnly",
        "SARP.VersionedBuffer.Object.NormalMatrix.Replace",
        "SARP.VersionedBuffer.Object.NormalMatrix.BackingCapacityElements",
        "SARP.VersionedBuffer.Object.NormalMatrix.LogicalElementCount" };
    switch (variant) {
    case kObjectPerObjectVariant: return &perObject;
    case kObjectInstanceTransformVariant: return &transforms;
    case kObjectDrawRecordVariant: return &drawRecords;
    case kObjectNormalMatrixVariant: return &normals;
    default: return nullptr;
    }
}

std::optional<std::string> ReadEnvironment(std::string_view name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, std::string(name).c_str()) != 0 || !value) return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
}

bool PersistentPublicationsEnabled() {
    static const bool enabled = [] {
        const auto value = ReadEnvironment("SARP_PERSISTENT_PUBLICATIONS");
        return value && !value->empty() && (*value)[0] == '1';
    }();
    return enabled;
}

// These buffers bypass the higher-level factories that normally attach semantic memory ownership. Tag every backing at allocation,
// including unpublished successors, so active and frame-retired versions keep an attributable category.
void TagBacking(org::Buffer& buffer, std::string_view name) {
    const char* hint = nullptr;
    if (name.starts_with("SARPGrass.Scratch.")) hint = "Grass transient scratch";
    else if (name.starts_with("SARPGrass.")) hint = "Grass persistent resources";
    else if (name.starts_with("Published::") || name.starts_with("PublishedIndirect") || name.starts_with("PublishedActive"))
        hint = "Versioned scene publication";
    if (!hint) return;
    org::memory::SetResourceUsageHint(buffer, hint);
    org::memory::SetResourceMemoryIdentifier(buffer, std::string(name));
}

void EmitPoolTelemetry(org::services::BackingPoolEvent event, std::string_view, std::uint64_t value) {
    using Event = org::services::BackingPoolEvent;
    switch (event) {
    case Event::Allocated:
        basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingAllocated");
        basic_telemetry::Record("SARP.VersionedBuffer.AllocatedBackingBytes", value);
        break;
    case Event::Reused: basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingReused"); break;
    case Event::Released:
        basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingReclaimed");
        basic_telemetry::AddCounter("SARP.VersionedBuffer.PooledBackingReclaimedBytes", static_cast<std::int64_t>(value));
        break;
    case Event::Exhausted: basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingRingExhausted"); break;
    case Event::Suspended: basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingRetirementSuspensions"); break;
    case Event::Woken:
        basic_telemetry::AddCounter("SARP.VersionedBuffer.BackingRetirementWakes", static_cast<std::int64_t>(value));
        break;
    default: break;  // per-pool gauges: the census is the pool's (BackingPool::Count)
    }
}

std::shared_ptr<const org::compiler::PreparedBackingState> CapturePreparedBackingState(org::Buffer& resource) {
    const auto snapshot = org::PublicationBindingBundle::Capture(resource);
    if (!snapshot) throw std::runtime_error("persistent buffer producer has no native snapshot");
    auto initial = std::make_shared<org::compiler::PreparedBackingState>();
    initial->graphResourceID = snapshot->resourceID;
    initial->resource = snapshot->resource.GetHandle();
    initial->shape = { (std::max)(1u, resource.GetMipLevels()), (std::max)(1u, resource.GetArraySize()), resource.HasLayout() };
    initial->heapType = snapshot->description.heapType;
    initial->aliasHeap = snapshot->aliasHeap;
    initial->aliasHeapIdentity = snapshot->aliasHeap.get();
    initial->aliasPoolID = snapshot->aliasPoolID;
    initial->aliasOffset = snapshot->aliasOffset;
    initial->aliasSize = snapshot->aliasSize;
    auto regions = std::make_shared<std::vector<org::compiler::PreparedStateRegion>>();
    if (const auto* tracker = resource.GetStateTracker()) for (const auto& segment : tracker->GetSegments()) {
        const auto range = org::ResolveRangeSpec(segment.rangeSpec, initial->shape.mips, initial->shape.slices);
        if (!range.isEmpty()) regions->push_back({ { range.firstMip, range.mipCount, range.firstSlice, range.sliceCount },
            { static_cast<uint64_t>(segment.state.access), static_cast<uint64_t>(segment.state.layout),
                static_cast<uint64_t>(segment.state.sync), rhi::AccessTypeIsWriteType(segment.state.access) } });
    }
    if (regions->empty()) regions->push_back({ { 0, initial->shape.mips, 0, initial->shape.slices },
        { static_cast<uint64_t>(rhi::ResourceAccessType::None), static_cast<uint64_t>(rhi::ResourceLayout::Undefined),
            static_cast<uint64_t>(rhi::ResourceSyncState::None), false } });
    initial->regions = std::move(regions);
    return initial;
}

ArtifactPayload WrapVersion(const std::shared_ptr<const PublishedGpuBufferVersion>& version, const VersionedGpuBufferBuildInput& input) {
    const auto* tag = static_cast<const VersionedBufferCatalogTag*>(input.tag.get());
    if (!tag) throw std::invalid_argument("versioned buffer input has no catalog tag");
    const bool replace = version->revisionMode == BufferRevisionMode::Replace;
    const bool appendOnly = !replace && input.bytes.empty() && input.image && input.previous &&
        std::ranges::all_of(input.writes, [&](const auto& write) {
            // Writes at or below `previous` are kept for the pool's trim floor; they are already in it.
            return write.sequence <= input.previous->writeSequence || write.byteSize == 0u ||
                write.byteOffset >= input.previous->elementCount * input.elementStride;
        });
    basic_telemetry::AddCounter(replace ? "SARP.VersionedBuffer.Replace" : "SARP.VersionedBuffer.Patch");
    if (appendOnly) basic_telemetry::AddCounter("SARP.VersionedBuffer.AppendOnly");
    basic_telemetry::Record("SARP.VersionedBuffer.RequestedCapacityElements", input.capacity);
    basic_telemetry::Record("SARP.VersionedBuffer.BackingCapacityElements", version->capacity);
    basic_telemetry::Record("SARP.VersionedBuffer.LogicalElementCount", input.elementCount);
    if (const auto* metrics = ObjectBufferMetrics(input, tag->variant)) {
        basic_telemetry::Record(metrics->capacityElements, version->capacity);
        basic_telemetry::Record(metrics->logicalElements, input.elementCount);
        if (appendOnly) basic_telemetry::AddCounter(metrics->appendOnly);
        if (replace) basic_telemetry::AddCounter(metrics->replace);
    }

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = tag->owner;
    root->publishRoot = false;
    root->fragment.revision = version->revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedGpuBufferVersion>(version);
    // The version's lease keeps its backing out of reuse while the fragment (and every frame selecting it) lives.
    root->fragment.resourceHolds.push_back(version);
    auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    resources->push_back(version->resource);
    root->catalogEntries.emplace_back(PublishedResourceKey{ tag->owner, tag->usage, 0, 0, tag->variant }, resources);
    return ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root));
}

} // namespace

std::shared_ptr<const void> MakeVersionedBufferTag(
    PublishedFragmentKind owner, PublishedResourceUsage usage, std::uint64_t variant) {
    return std::make_shared<const VersionedBufferCatalogTag>(VersionedBufferCatalogTag{ owner, usage, variant });
}

VersionedBufferRuntime::VersionedBufferRuntime(std::uint32_t framesInFlight)
    : m_clock(std::make_shared<org::services::RetirementClock>()),
      m_framesInFlight((std::max)(framesInFlight, 1u)) {}

void VersionedBufferRuntime::NotifyFrameRetirement() noexcept {
    // Retirement notifications arrive from several threads; each is one epoch (the clock keeps the maximum).
    m_clock->Advance(m_retirements.fetch_add(1, std::memory_order_acq_rel) + 1u);
}

std::shared_ptr<VersionedGpuBufferBackingPool> VersionedBufferRuntime::CreatePool(std::string name) const {
    org::services::BackingPoolHooks hooks;
    hooks.allocate = [](const org::services::BackingRequest& request) {
        if (request.stride == 0u) throw std::invalid_argument("versioned GPU buffer backings are structured");
        org::Resource::ScopedECSRegistrationSuppression suppressECS;
        auto buffer = CreateIndexedStructuredBuffer(
            static_cast<std::uint32_t>((std::max<std::uint64_t>)(request.capacityClass, 1u)),
            request.stride, request.flags.unorderedAccess, request.flags.indirectArguments);
        buffer->SetName(std::string(request.name));
        org::services::BackingAllocation allocation;
        allocation.srvIndex = buffer->GetSRVInfo(0).slot.index;
        allocation.buffer = std::move(buffer);
        return allocation;
    };
    hooks.tag = TagBacking;
    hooks.telemetry = EmitPoolTelemetry;
    hooks.env = ReadEnvironment;
    // The pool is single-acquirer; BasicRenderer builds versions of one pool on several graph workers.
    hooks.exclusive = [mutex = std::make_shared<std::mutex>()](const std::function<void()>& body) {
        std::lock_guard lock(*mutex);
        body();
    };
    // Publication, admitted-but-unsubmitted frames, submitted frame slots and asynchronous diagnostics can overlap. Keep the
    // pool bounded, but large enough that exact lifetime retirement, not premature mutation, is the steady-state reuse
    // mechanism: exhaustion suspends the build until a backing retires.
    org::services::BackingPool::Config config;
    config.name = std::move(name);
    config.maxPerClass = m_framesInFlight * 2u + 2u;
    config.framesInFlight = m_framesInFlight;
    return org::services::BackingPool::Create(std::move(hooks), m_clock, std::move(config));
}

void AcknowledgePublishedVersion(std::shared_ptr<const PublishedGpuBufferVersion>& published,
    std::shared_ptr<const PublishedGpuBufferVersion> version, const VersionedBufferRuntime& runtime) {
    if (!version || published == version) return;
    // GPU safety starts when the old backing stops being the published table, not when it first became it: a table can stay
    // current for thousands of frames while older frame slots still reference its descriptors.
    if (published && published->lease.Get() != version->lease.Get()) published->lease.Bind(runtime.SupersededSafeAt());
    published = std::move(version);
}

namespace {

org::services::VersionedBufferFamily<RendererGraphBinding>::Config FamilyConfig(
    const VersionedBufferRuntime& runtime, VersionedBufferFamily::Config config) {
    if (config.elementStride == 0 || config.address.kind == ArtifactKind::Generic) {
        throw std::invalid_argument("invalid versioned buffer family configuration");
    }
    org::services::VersionedBufferFamily<RendererGraphBinding>::Config shared;
    shared.address = config.address;
    shared.elementStride = config.elementStride;
    shared.flags.unorderedAccess = config.unorderedAccess;
    shared.flags.indirectArguments = config.indirectArguments;
    shared.tag = MakeVersionedBufferTag(config.catalogOwner, config.catalogUsage, config.catalogVariant);
    shared.gpuWritten = config.gpuWritten;
    shared.framesInFlight = runtime.FramesInFlight();
    shared.pool = runtime.CreatePool(config.debugName);
    shared.debugName = std::move(config.debugName);
    return shared;
}

} // namespace

VersionedBufferFamily::VersionedBufferFamily(const VersionedBufferRuntime& runtime, Config config)
    : org::services::VersionedBufferFamily<RendererGraphBinding>(FamilyConfig(runtime, std::move(config))) {}

void RegisterVersionedGpuBufferProducer(AsyncStateGraph& graph) {
    org::services::VersionedBufferProducerHooks<RendererGraphBinding> hooks;
    hooks.decorate = [](PublishedGpuBufferVersion& version, const VersionedGpuBufferBuildInput&) {
        // Immutable producer seed, captured before visibility. It owns no semantic consumer pin, so journal retention cannot
        // keep a backing from reuse.
        if (PersistentPublicationsEnabled() && version.resource) version.hostState = CapturePreparedBackingState(*version.resource);
    };
    hooks.wrap = [](const std::shared_ptr<const PublishedGpuBufferVersion>& version, const VersionedGpuBufferBuildInput& input,
        const ArtifactBuildContext&) { return WrapVersion(version, input); };
    static constexpr std::array kinds{ ArtifactKind::BufferVersion, ArtifactKind::ActiveDrawList };
    org::services::RegisterVersionedBufferProducer<RendererGraphBinding>(graph, kinds, TaskLane::FrameCritical,
        TaskDomain::GpuBufferBuild, "VersionedGpuBufferArtifact::Build", std::move(hooks));
}

} // namespace br::render
