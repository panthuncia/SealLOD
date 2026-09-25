#include "Render/CLodResidencyStorageArtifacts.h"

#include <algorithm>
#include <vector>

#include <BasicTelemetry/Telemetry.h>
#include <spdlog/spdlog.h>

#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Resources/Buffers/Buffer.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildCLodResidencyStorage(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<CLodResidencyStorageInput>();
    if (!input || input->capacity == 0 || !input->nonResidentBits || context.key != CLodResidencyStorageAddress()) {
        return ArtifactBuildResult::Failure("CLod residency storage input invalid");
    }
    auto fragment = std::make_shared<RendererStateFragmentArtifact>();
    fragment->kind = PublishedFragmentKind::Geometry;
    // A dependency of the geometry cut, never a Geometry root of its own.
    fragment->publishRoot = false;
    fragment->fragment.revision = context.revision;
    fragment->fragment.payload = ArtifactPayload::Make<CLodResidencyStorageInput>(input);
    fragment->fragment.resourceHolds.push_back(input->nonResidentBits);
    auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    resources->push_back(input->nonResidentBits);
    fragment->catalogEntries.emplace_back(PublishedResourceKey{
        PublishedFragmentKind::Geometry, PublishedResourceUsage::ShaderResource,
        0, 0, kCLodNonResidentBitsCatalogVariant }, std::move(resources));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(fragment)));
}

ArtifactBuildResult BuildCLodResidencyCapacityGate(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<CLodResidencyCapacityGateInput>();
    if (!input || !input->directory || context.key != CLodResidencyCapacityGateAddress(input->capacity)) {
        return ArtifactBuildResult::Failure("CLod residency capacity gate input invalid");
    }
    std::uint64_t waitIdentity = 0;
    const auto storage = input->directory->Covering(input->capacity, waitIdentity);
    if (!storage) {
        return ArtifactBuildResult::Suspend(ArtifactSuspension::External(waitIdentity,
            "geometry waits for a filled CLod residency bitset covering its groups"));
    }
    // Exact, not Latest: this gate is one immutable version, and a newer storage
    // must not rebuild it (and with it the geometry cut that requires it).
    const auto dependency = std::ranges::find_if(context.dependencies, [&](const ArtifactSnapshot& value) {
        return value.Version() == storage.version;
    });
    if (dependency == context.dependencies.end()) {
        return ArtifactBuildResult::Needs({ Exact(storage, ArtifactReadiness::CpuReady) });
    }
    if (!dependency->payload.Get<RendererStateFragmentArtifact>()) {
        return ArtifactBuildResult::Failure("CLod residency storage dependency has no fragment");
    }
    return ArtifactBuildResult::Ready(dependency->payload);
}

} // namespace

CLodResidencyStorageDirectory::CLodResidencyStorageDirectory(RendererStateRequestService& requests)
    : m_requests(requests), m_notify(requests.MakeSuspensionNotifier()) {}

void CLodResidencyStorageDirectory::PublishStorage(
    std::uint32_t capacity, std::shared_ptr<org::Buffer> nonResidentBits) {
    auto input = std::make_shared<CLodResidencyStorageInput>();
    input->capacity = capacity;
    input->nonResidentBits = std::move(nonResidentBits);
    ArtifactRequestResult result;
    {
        std::lock_guard lock(m_mutex);
        const auto revision = ++m_revision;
        result = m_requests.Request(CLodResidencyStorageAddress(), revision, {},
            ArtifactPayload::Make<CLodResidencyStorageInput>(std::move(input)), revision ^ capacity);
    }
    if (!result) {
        basic_telemetry::AddCounter("SARP.CLodResidencyStorage.RequestRejected");
        return;
    }
    // Gates are handed only versions that have already built, so an exact
    // requirement on one can never observe it superseded before completion.
    auto awaiter = m_requests.AwaitExact(result.Handle(), ArtifactReadiness::CpuReady,
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        [this, handle = result.Handle(), capacity](const ArtifactSnapshot&) {
            std::vector<std::uint64_t> satisfied;
            {
                std::lock_guard lock(m_mutex);
                if (m_newest && m_newest.version.revision >= handle.version.revision) return;
                m_newest = handle;
                m_newestCapacity = capacity;
                const auto end = m_waiting.upper_bound(capacity);
                for (auto it = m_waiting.begin(); it != end; ++it) satisfied.push_back(it->second);
                m_waiting.erase(m_waiting.begin(), end);
            }
            basic_telemetry::SetGauge("SARP.CLodResidencyStorage.PublishedCapacity",
                static_cast<std::int64_t>(capacity));
            for (const auto identity : satisfied) m_notify(identity);
        },
        [](const ArtifactTermination& termination) {
            basic_telemetry::AddCounter("SARP.CLodResidencyStorage.Terminated");
            spdlog::error("CLod residency storage revision {} terminated: {}",
                termination.version.revision, termination.error);
        });
    std::lock_guard lock(m_mutex);
    m_awaiters.push_back(std::move(awaiter));
}

ArtifactVersionHandle CLodResidencyStorageDirectory::Covering(std::uint32_t capacity, std::uint64_t& waitIdentity) {
    auto requested = m_requestedCapacity.load(std::memory_order_relaxed);
    while (requested < capacity &&
        !m_requestedCapacity.compare_exchange_weak(requested, capacity, std::memory_order_acq_rel)) {
    }
    std::lock_guard lock(m_mutex);
    if (m_newest && m_newestCapacity >= capacity) return m_newest;
    waitIdentity = AsyncStateGraph::AllocateSuspensionIdentity();
    m_waiting.emplace(capacity, waitIdentity);
    return {};
}

void RegisterCLodResidencyStorageProducers(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::CLodResidencyStorage, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "CLodResidencyStorage::Build", BuildCLodResidencyStorage });
    graph.RegisterProducer(ArtifactKind::CLodResidencyCapacityGate, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "CLodResidencyCapacityGate::Build", BuildCLodResidencyCapacityGate });
}

} // namespace br::render
