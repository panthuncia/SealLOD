#include "VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.h"

#include <algorithm>
#include <vector>

#include <BasicTelemetry/Telemetry.h>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
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
    auto storage = input->directory->Covering(input->capacity, waitIdentity);
    if (waitIdentity != 0) {
        return ArtifactBuildResult::Suspend(ArtifactSuspension::External(waitIdentity,
            "geometry waits for a filled CLod residency bitset covering its groups"));
    }
    if (!storage.Get<RendererStateFragmentArtifact>()) {
        return ArtifactBuildResult::Failure("CLod residency storage has no fragment");
    }
    // Copied rather than required as an edge: this gate is one immutable version,
    // so a newer storage must not rebuild it. An exact edge named here would also
    // strand it, because a newer storage can reclaim that version while this
    // result waits to be applied.
    return ArtifactBuildResult::Ready(std::move(storage));
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
    // Gates are handed only storages that have already built.
    auto awaiter = m_requests.AwaitExact(result.Handle(), ArtifactReadiness::CpuReady,
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        [this, revision = result.Handle().version.revision, capacity](const ArtifactSnapshot& snapshot) {
            std::vector<std::uint64_t> satisfied;
            {
                std::lock_guard lock(m_mutex);
                if (m_newestRevision >= revision) return;
                m_newestRevision = revision;
                m_newestPayload = snapshot.payload;
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

ArtifactPayload CLodResidencyStorageDirectory::Covering(std::uint32_t capacity, std::uint64_t& waitIdentity) {
    auto requested = m_requestedCapacity.load(std::memory_order_relaxed);
    while (requested < capacity &&
        !m_requestedCapacity.compare_exchange_weak(requested, capacity, std::memory_order_acq_rel)) {
    }
    std::lock_guard lock(m_mutex);
    if (m_newestRevision != 0 && m_newestCapacity >= capacity) return m_newestPayload;
    waitIdentity = br::render::AllocateArtifactSuspensionIdentity();
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
