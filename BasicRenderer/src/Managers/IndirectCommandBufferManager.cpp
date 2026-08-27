#include "Managers/IndirectCommandBufferManager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>
#include <spdlog/spdlog.h>

#include <BasicTelemetry/Tracy.h>

#include "Managers/Singletons/ResourceManager.h"
#include "Resources/ResourceGroup.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/DynamicResource.h"
#include "Render/IndirectCommand.h"
#include "Resources/Components.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Buffers/SortedUnsignedIntBuffer.h"
#include "Managers/ObjectManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/IndirectStateArtifacts.h"
#include "Render/GraphMigrationMode.h"
#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Render/VersionedGpuBufferArtifacts.h"

IndirectCommandBufferManager::IndirectCommandBufferManager() = default;

IndirectCommandBufferManager::~IndirectCommandBufferManager() {
    m_stopping.store(true, std::memory_order_release);
    if (m_observedObjectManager) m_observedObjectManager->SetActiveDrawSetMutationCallback({});
    if (m_buildScope.Valid()) m_buildScope.CancelAndWait();
}

void IndirectCommandBufferManager::RegisterWorkload(const DrawWorkloadKey& workloadKey) {
    {
        std::lock_guard lock(m_desiredMutex);
        EnsureWorkloadRegistered(workloadKey);
    }
    ScheduleDesiredBuild();
}

void IndirectCommandBufferManager::CreateBuffersForView(uint64_t viewID) {
    {
        std::lock_guard lock(m_desiredMutex);
        if (!m_viewIDs.insert(viewID).second) return;
        ++m_viewLifetimeRevisions[viewID];
        ++m_desiredMutationRevision;
    }
    ScheduleDesiredBuild();
}

void IndirectCommandBufferManager::UnregisterBuffers(uint64_t viewID) {
    {
        std::lock_guard lock(m_desiredMutex);
        if (m_viewIDs.erase(viewID) == 0) return;
        ++m_desiredMutationRevision;
    }
    ScheduleDesiredBuild();
}

void IndirectCommandBufferManager::UpdateBuffersForWorkload(const DrawWorkloadKey& workloadKey, unsigned int numDraws) {
    RequestWorkloadCount(workloadKey, numDraws);
}

void IndirectCommandBufferManager::UpdateBuffersForWorkloads(std::span<const WorkloadCountUpdate> updates) {
    RequestWorkloadCounts(updates);
}

void IndirectCommandBufferManager::RequestWorkloadCount(const DrawWorkloadKey& workloadKey, unsigned int numDraws) {
    const WorkloadCountUpdate update{ workloadKey, numDraws };
    RequestWorkloadCounts(std::span<const WorkloadCountUpdate>(&update, 1));
}

void IndirectCommandBufferManager::RequestWorkloadCounts(std::span<const WorkloadCountUpdate> updates) {
    if (updates.empty()) {
        return;
    }

    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> deduped;
    deduped.reserve(updates.size());
    for (const auto& update : updates) {
        deduped[update.workloadKey] = update.count;
    }

    bool changed = false;
    {
        std::lock_guard lock(m_desiredMutex);
        for (const auto& [workloadKey, count] : deduped) {
            EnsureWorkloadRegistered(workloadKey);
            auto& desired = m_workloadToRequestedCount[workloadKey];
            if (desired != count) {
                desired = count;
                ++m_desiredMutationRevision;
                changed = true;
            }
        }
    }
    if (changed) ScheduleDesiredBuild();
}

void IndirectCommandBufferManager::SetRendererStateServices(
    br::render::RendererStateRequestService* requests, org::runtime::IUploadService* uploads) {
    m_rendererStateRequests = requests;
    m_uploadService = uploads;
    if (requests && !m_buildScope.Valid()) {
        m_buildScope = TaskSchedulerManager::GetInstance().CreateScope(
            "IndirectCommandBufferManager::DesiredState");
    }
}

void IndirectCommandBufferManager::PublishDesiredState(
	ObjectManager& objectManager, MaterialManager& materialManager) {
    BT_ZONE_SCOPE("IndirectCommandBufferManager::PublishDesiredState");
    if (!m_rendererStateRequests || !m_uploadService) return;
	if (!m_activeObserverInstalled) {
		m_activeObserverInstalled = true;
		m_observedObjectManager = &objectManager;
		objectManager.SetActiveDrawSetMutationCallback(
			[this](const DrawWorkloadKey& workloadKey, bool replace,
				std::uint64_t revision,
				std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>> entries) {
				OnActiveDrawSetMutation(
					workloadKey, replace, revision, std::move(entries));
			});
	}
	// The legacy object buffers remain authoritative while their graph migration is
	// in shadow mode. Depending on the shadow BufferVersion here made the indirect
	// path effectively active and could hold all visibility behind a comparison-only
	// artifact.
	const auto objectBufferRequirement =
		br::render::GraphActive(br::render::kObjectBufferGraphMigrationMode)
			? objectManager.DesiredBufferStateRequirement()
			: std::optional<br::render::ArtifactRequirement>{};
	const auto publishedSource = br::render::PublishedStateSource::ProcessSource();
	const auto publishedState = publishedSource ? publishedSource->Load() : nullptr;
	const auto materialRevision = publishedState
		? publishedState->materials.revision : 0u;
    const auto residentDrawRecordCount = objectManager.GetResidentInstanceDrawRecordCount();
    bool changed = false;
    {
        std::lock_guard lock(m_desiredMutex);
        const auto objectBufferRevision =
            objectBufferRequirement ? objectBufferRequirement->minimumRevision : 0u;
        // Materials are a startup readiness gate, not part of the indirect
        // artifact's content identity. Texture fallback upgrades can publish
        // many material revisions without changing any active draw entry,
        // view, capacity, or draw-record extent. Treating each one as an
        // indirect mutation repeatedly rebuilt and replaced the complete
        // visibility state while streaming was settling.
        const bool materialBecameReady = m_lastMaterialRevision == 0 && materialRevision != 0;
        if (m_lastObjectBufferRevision != objectBufferRevision ||
            materialBecameReady ||
            m_lastResidentDrawRecordCount != residentDrawRecordCount) {
            m_lastObjectBufferRevision = objectBufferRevision;
            m_objectBufferRequirement = objectBufferRequirement;
			if (materialBecameReady) m_lastMaterialRevision = materialRevision;
            m_lastResidentDrawRecordCount = residentDrawRecordCount;
            ++m_desiredMutationRevision;
            changed = true;
        }
        changed = changed || m_consumedMutationRevision != m_desiredMutationRevision;
    }
    if (changed) ScheduleDesiredBuild();
}

void IndirectCommandBufferManager::OnActiveDrawSetMutation(
    const DrawWorkloadKey& workloadKey, bool replace, std::uint64_t revision,
    std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>> entries) {
    {
        std::lock_guard lock(m_desiredMutex);
        EnsureWorkloadRegistered(workloadKey);
        auto& journal = m_activeJournals[workloadKey];
        if (revision <= journal.revision) return;
        if (replace) {
            journal.base = std::move(entries);
            journal.appends.clear();
        } else if (entries && !entries->empty()) {
            journal.appends.push_back(std::move(entries));
        }
        journal.revision = revision;
        ++m_desiredMutationRevision;
    }
}

IndirectCommandBufferManager::DesiredSnapshot
IndirectCommandBufferManager::CaptureDesiredSnapshotLocked() const {
    DesiredSnapshot snapshot;
    snapshot.revision = m_desiredMutationRevision;
    snapshot.activeMaterialRevision = m_lastMaterialRevision;
    snapshot.objectBufferRequirement = m_objectBufferRequirement;
    snapshot.residentDrawRecordCount = m_lastResidentDrawRecordCount;
    snapshot.incrementSize = m_incrementSize;
    snapshot.requestedCounts = m_workloadToRequestedCount;
    snapshot.capacities = m_workloadToCapacity;
    snapshot.workloadIDs = m_workloadIDs;
    snapshot.activeJournals = m_activeJournals;
    snapshot.viewIDs = m_viewIDs;
    snapshot.viewLifetimeRevisions = m_viewLifetimeRevisions;
    return snapshot;
}

void IndirectCommandBufferManager::ScheduleDesiredBuild() {
    if (m_stopping.load(std::memory_order_acquire) || !m_buildScope.Valid() ||
        !m_rendererStateRequests || !m_uploadService ||
        m_buildScheduled.exchange(true, std::memory_order_acq_rel)) return;
    const auto accepted = TaskSchedulerManager::GetInstance().Submit(
        m_buildScope, TaskLane::Streaming, TaskDomain::General,
        "IndirectCommandBufferManager::BuildDesiredState",
        [this](const br::TaskContext& context) { DrainDesiredBuild(context); });
    if (!accepted) m_buildScheduled.store(false, std::memory_order_release);
}

void IndirectCommandBufferManager::DrainDesiredBuild(const br::TaskContext& context) {
    if (context.StopRequested() || m_stopping.load(std::memory_order_acquire)) {
        m_buildScheduled.store(false, std::memory_order_release);
        return;
    }
    DesiredSnapshot snapshot;
    {
        std::lock_guard lock(m_desiredMutex);
        if (m_consumedMutationRevision == m_desiredMutationRevision) {
            m_buildScheduled.store(false, std::memory_order_release);
            return;
        }
        snapshot = CaptureDesiredSnapshotLocked();
        m_consumedMutationRevision = snapshot.revision;
    }
    BuildDesiredState(std::move(snapshot));

    // Publish at most one coalesced snapshot per frame. The preparation task
    // runs outside the single-slot RendererState domain so it cannot delay the
    // graph producers that consume these requests.
    m_buildScheduled.store(false, std::memory_order_release);
}

void IndirectCommandBufferManager::BuildDesiredState(DesiredSnapshot snapshot) {
    const auto started = std::chrono::steady_clock::now();
    auto input = std::make_shared<br::render::IndirectStateBuildInput>();
    input->materializeResources = true;
    input->incrementSize = snapshot.incrementSize;
    input->viewIDs.assign(snapshot.viewIDs.begin(), snapshot.viewIDs.end());
    std::ranges::sort(input->viewIDs);
    input->workloads.reserve(snapshot.requestedCounts.size());

    std::vector<br::render::ArtifactRequirement> requirements;
    // Do not publish graph-backed visibility against the empty startup
    // material fragment. The current publisher cannot yet retain a rapidly
    // advancing material root in every indirect closure without starving the
    // independently publishable material fragment, so active-material
    // readiness is an ordering gate here rather than a graph dependency.
    if (snapshot.activeMaterialRevision == 0) return;
    if (br::render::GraphActive(br::render::kObjectBufferGraphMigrationMode) &&
        !snapshot.objectBufferRequirement) return;
    if (snapshot.objectBufferRequirement) requirements.push_back(*snapshot.objectBufferRequirement);
    for (const auto viewID : input->viewIDs) {
        const auto lifetimeRevision = snapshot.viewLifetimeRevisions.at(viewID);
        const br::render::ArtifactKey lifetimeKey{
            br::render::ArtifactKind::ViewLifetime, viewID, 0 };
        auto lifetime = std::make_shared<br::render::ViewLifetimeArtifact>(
            br::render::ViewLifetimeArtifact{ viewID, lifetimeRevision });
        (void)m_rendererStateRequests->Request(lifetimeKey, lifetimeRevision, {},
            br::render::ArtifactPayload::Make<br::render::ViewLifetimeArtifact>(
                std::move(lifetime)));
        requirements.push_back({
            lifetimeKey, lifetimeRevision, br::render::ArtifactReadiness::GpuReady });
    }

    std::uint64_t sourceEntryCount = 0;
    std::uint64_t deferredEntryCount = 0;
    std::uint64_t safeDrawCount = 0;
    std::uint32_t nonEmptyWorkloads = 0;
    const auto roundUp = [increment = snapshot.incrementSize](unsigned int value) {
        return ((value + increment - 1u) / increment) * increment;
    };
    for (const auto& [key, requestedCount] : snapshot.requestedCounts) {
        const auto journalFound = snapshot.activeJournals.find(key);
        const auto idFound = snapshot.workloadIDs.find(key);
        if (journalFound == snapshot.activeJournals.end() ||
            idFound == snapshot.workloadIDs.end() || journalFound->second.revision == 0) continue;
        const auto& journal = journalFound->second;
        std::size_t entryCount = journal.base ? journal.base->size() : 0u;
        for (const auto& append : journal.appends) {
            if (append) entryCount += append->size();
        }
        std::vector<br::render::ActiveDrawEntryDTO> entries;
        entries.reserve(entryCount);
        const auto appendEntries = [&entries](const auto& source) {
            if (!source) return;
            for (const auto& entry : *source) {
                entries.push_back({ entry.drawRecordIndex, entry.generation });
            }
        };
        appendEntries(journal.base);
        for (const auto& append : journal.appends) appendEntries(append);
        sourceEntryCount += entries.size();

        // Active-set mutation and the logical draw-record extent are observed
        // independently. A newly appended entry can therefore arrive one snapshot
        // before its extent. Defer only those entries; rejecting the whole workload
        // made every otherwise-valid static draw disappear. PublishDesiredState
        // observes the later extent change and schedules the successor snapshot.
        const auto originalEntryCount = entries.size();
        std::erase_if(entries, [&](const auto& entry) {
            return entry.drawRecordIndex >= snapshot.residentDrawRecordCount;
        });
        deferredEntryCount += originalEntryCount - entries.size();

        br::render::IndirectWorkloadInputDTO dto{};
        dto.key = key;
        dto.activeListArtifactKey = {
            br::render::ArtifactKind::ActiveDrawList, idFound->second, 0 };
        dto.requestedCount = requestedCount;
        dto.residentDrawRecordCount = static_cast<std::uint32_t>(
            (std::min<std::uint64_t>)(snapshot.residentDrawRecordCount,
                (std::numeric_limits<std::uint32_t>::max)()));
        dto.activeListRevision = journal.revision;
        dto.activeEntries = entries;

        auto activeInput = std::make_shared<br::render::VersionedGpuBufferBuildInput>();
        activeInput->uploadService = m_uploadService;
        activeInput->debugName = "PublishedActiveDrawList";
        activeInput->writeSequence = dto.activeListRevision;
        activeInput->elementStride = sizeof(br::render::ActiveDrawEntryDTO);
        activeInput->elementCount = entries.size();
        activeInput->capacity = entries.size();
        activeInput->catalogOwner = br::render::PublishedFragmentKind::ActiveDrawLists;
        activeInput->catalogUsage = br::render::PublishedResourceUsage::ActiveDrawList;
        activeInput->catalogVariant = static_cast<std::uint64_t>(key.compileFlags) |
            (static_cast<std::uint64_t>(key.skinnedShadowCaster) << 62u) |
            (static_cast<std::uint64_t>(key.clodOnly) << 63u);
        activeInput->bytes.resize(entries.size() * sizeof(br::render::ActiveDrawEntryDTO));
        if (!activeInput->bytes.empty()) {
            std::memcpy(activeInput->bytes.data(), entries.data(), activeInput->bytes.size());
        }
        const auto activeRevision = (std::max<std::uint64_t>)(journal.revision, 1u);
        (void)m_rendererStateRequests->Request(dto.activeListArtifactKey, activeRevision, {},
            br::render::ArtifactPayload::Make<br::render::VersionedGpuBufferBuildInput>(
                std::move(activeInput)));
        requirements.push_back({
            dto.activeListArtifactKey, activeRevision, br::render::ArtifactReadiness::GpuReady });

        const auto safeCount = static_cast<unsigned int>((std::min<std::uint64_t>)({
            requestedCount, entries.size(), dto.residentDrawRecordCount }));
        safeDrawCount += safeCount;
        nonEmptyWorkloads += safeCount != 0u ? 1u : 0u;
        auto& capacity = snapshot.capacities[key];
        capacity = (std::max)(capacity, safeCount == 0u ? 0u : roundUp(safeCount));
        dto.minimumCapacity = capacity;
        if (safeCount != 0u) {
            for (const auto viewID : input->viewIDs) {
                const br::render::ArtifactKey argumentKey{
                    br::render::ArtifactKind::BufferVersion, idFound->second, viewID };
                auto argumentInput = std::make_shared<br::render::VersionedGpuBufferBuildInput>();
                argumentInput->uploadService = m_uploadService;
                argumentInput->debugName = "PublishedIndirectArguments";
                argumentInput->writeSequence = capacity;
                argumentInput->elementStride = sizeof(DispatchMeshIndirectCommand);
                argumentInput->elementCount = capacity;
                argumentInput->capacity = capacity;
                argumentInput->unorderedAccess = true;
                argumentInput->indirectArguments = true;
                argumentInput->catalogOwner =
                    br::render::PublishedFragmentKind::IndirectWorkloads;
                argumentInput->catalogUsage =
                    br::render::PublishedResourceUsage::IndirectArguments;
                argumentInput->catalogVariant =
                    static_cast<std::uint64_t>(key.compileFlags) |
                    (static_cast<std::uint64_t>(key.skinnedShadowCaster) << 62u) |
                    (static_cast<std::uint64_t>(key.clodOnly) << 63u);
                const auto argumentRevision =
                    (std::max<std::uint64_t>)(capacity, 1u);
                (void)m_rendererStateRequests->Request(
                    argumentKey, argumentRevision, {},
                    br::render::ArtifactPayload::Make<br::render::VersionedGpuBufferBuildInput>(
                        std::move(argumentInput)));
                requirements.push_back({
                    argumentKey, argumentRevision, br::render::ArtifactReadiness::GpuReady });
                dto.argumentArtifacts.push_back({ viewID, argumentKey });
            }
        }
        input->workloads.push_back(std::move(dto));
    }

    {
        std::lock_guard lock(m_desiredMutex);
        for (const auto& [key, capacity] : snapshot.capacities) {
            m_workloadToCapacity[key] = (std::max)(m_workloadToCapacity[key], capacity);
        }
    }
    (void)m_rendererStateRequests->Request(
        { br::render::ArtifactKind::IndirectWorkload, 0, 0 },
        snapshot.revision, std::move(requirements),
        br::render::ArtifactPayload::Make<br::render::IndirectStateBuildInput>(
            std::move(input)), snapshot.revision);
    basic_telemetry::Record("SARP.Indirect.DesiredStateWorkerBuildNs",
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()));
    basic_telemetry::SetGauge("SARP.Indirect.DesiredStateSourceEntries",
        static_cast<std::int64_t>(sourceEntryCount));
    basic_telemetry::SetGauge("SARP.Indirect.DesiredStateDeferredEntries",
        static_cast<std::int64_t>(deferredEntryCount));
    basic_telemetry::SetGauge("SARP.Indirect.DesiredStateSafeDraws",
        static_cast<std::int64_t>(safeDrawCount));
    basic_telemetry::SetGauge("SARP.Indirect.DesiredStateNonEmptyWorkloads",
        static_cast<std::int64_t>(nonEmptyWorkloads));
}

void IndirectCommandBufferManager::SetIncrementSize(unsigned int incrementSize) {
    const auto desired = incrementSize == 0u ? 1u : incrementSize;
    {
        std::lock_guard lock(m_desiredMutex);
        if (m_incrementSize == desired) return;
        m_incrementSize = desired;
        ++m_desiredMutationRevision;
    }
    ScheduleDesiredBuild();
}

// -------------------- helpers --------------------

void IndirectCommandBufferManager::EnsureWorkloadRegistered(const DrawWorkloadKey& workloadKey) {
    bool inserted = false;
    if (!m_workloadToCapacity.count(workloadKey)) {
        m_workloadToCapacity[workloadKey] = 0;
        inserted = true;
    }
    if (!m_workloadToRequestedCount.count(workloadKey)) {
        m_workloadToRequestedCount[workloadKey] = 0;
    }
    if (!m_workloadToPublishedCount.count(workloadKey)) {
        m_workloadToPublishedCount[workloadKey] = 0;
    }
    if (!m_workloadIDs.contains(workloadKey)) {
        m_workloadIDs.emplace(workloadKey, m_nextWorkloadID++);
        inserted = true;
    }
    if (inserted) ++m_desiredMutationRevision;
}
