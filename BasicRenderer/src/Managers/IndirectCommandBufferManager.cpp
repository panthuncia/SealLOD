#include "Managers/IndirectCommandBufferManager.h"

#include <algorithm>
#include <bit>
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
    std::vector<br::render::ArtifactAddress> retiredAddresses;
    {
        std::lock_guard lock(m_desiredMutex);
        if (m_viewIDs.erase(viewID) == 0) return;
        retiredAddresses.push_back({ br::render::ArtifactKind::ViewLifetime, viewID, 0 });
        retiredAddresses.reserve(m_workloadIDs.size() + 1u);
        for (const auto& [_, workloadID] : m_workloadIDs) {
            retiredAddresses.push_back({ br::render::ArtifactKind::BufferVersion, workloadID, viewID });
        }
        ++m_desiredMutationRevision;
    }
    if (m_rendererStateRequests) {
        for (const auto& address : retiredAddresses) m_rendererStateRequests->Release(address);
    }
    {
        std::lock_guard lock(m_submissionCacheMutex);
        for (const auto& address : retiredAddresses) m_submittedArtifacts.erase(address);
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
    if (m_rendererStateRequests != requests) {
        std::lock_guard lock(m_submissionCacheMutex);
        m_submittedArtifacts.clear();
    }
    m_rendererStateRequests = requests;
    m_uploadService = uploads;
    if (requests && !m_buildScope.Valid()) {
        m_buildScope = TaskSchedulerManager::GetInstance().CreateScope(
            "IndirectCommandBufferManager::DesiredState");
    }
	// Admission is gated by the renderer's atomically committed manifest in
	// PublishDesiredState. The graph's internal Published milestone can run far
	// ahead of frame retirement and exhaust every bounded backing ring with roots
	// that the renderer has never consumed.
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
	const auto objectBufferRequirement = objectManager.DesiredBufferStateRequirement();
	const auto publishedSource = br::render::PublishedStateSource::ProcessSource();
	const auto publishedState = publishedSource ? publishedSource->Load() : nullptr;
	if (publishedState) {
		std::lock_guard desiredLock(m_desiredMutex);
		const auto indirect = publishedState->indirectWorkloads.payload
			.Get<br::render::PublishedIndirectState>();
		for (const auto& active : indirect
			? indirect->activeListVersions
			: std::vector<br::render::PublishedIndirectState::ActiveListVersion>{}) {
			const auto& version = active.version;
			if (!version) continue;
			const auto workload = std::ranges::find_if(m_workloadIDs,
				[&](const auto& value) { return value.second == active.workloadID; });
			if (workload == m_workloadIDs.end()) continue;
			const auto journal = m_activeJournals.find(workload->first);
			if (journal != m_activeJournals.end() && journal->second.bufferJournal) {
				journal->second.bufferJournal->Acknowledge(version);
				if (auto pool = version->backingPool.lock(); version->backing) {
					pool->AcknowledgePublished(version->backing->backingGeneration, 3u);
				}
			}
		}
		auto observed = m_publishedRootRevision.load(std::memory_order_relaxed);
		while (observed < publishedState->indirectWorkloads.revision) {
			if (m_publishedRootRevision.compare_exchange_weak(observed,
				publishedState->indirectWorkloads.revision, std::memory_order_release,
				std::memory_order_relaxed)) {
				// Start the frame-slot rotation gate when the coherent root actually
				// becomes render-visible. Admission can precede publication by many
				// frames under load; measuring from admission allowed the successor
				// to start before old frame leases had rotated out.
				m_lastAdmissionRetirementEpoch.store(
					br::render::VersionedGpuBufferFrameRetirementEpoch(),
					std::memory_order_release);
				break;
			}
		}
	}
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
    bool changed = false;
    {
        std::lock_guard lock(m_desiredMutex);
        EnsureWorkloadRegistered(workloadKey);
        auto& journal = m_activeJournals[workloadKey];
        if (revision <= journal.revision) return;
		if (!journal.bufferJournal) {
			journal.bufferJournal = std::make_shared<br::render::VersionedGpuBufferJournal>(
				static_cast<std::uint32_t>(sizeof(br::render::ActiveDrawEntryDTO)));
			journal.backingPool =
				std::make_shared<br::render::VersionedGpuBufferBackingPool>();
		}
		const auto capacityFor = [](std::size_t rows) {
			const auto hinted = (std::max<std::size_t>)(rows + rows / 4u, rows + 512u);
			return static_cast<std::uint64_t>(std::bit_ceil((std::max<std::size_t>)(hinted, 1u)));
		};
		const auto entryBytes = [](const auto& source) {
			return source ? std::as_bytes(std::span(*source)) : std::span<const std::byte>{};
		};
        if (replace) {
            journal.base = std::move(entries);
            journal.appends.clear();
			const auto count = journal.base ? journal.base->size() : 0u;
			const auto bytes = entryBytes(journal.base);
			if (journal.bufferJournal->DesiredSequence() == 0u) {
				journal.bufferJournal->Initialize(bytes, count, capacityFor(count));
			} else {
				journal.bufferJournal->ReplaceImage(bytes, count, capacityFor(count));
			}
        } else if (entries && !entries->empty()) {
			const auto previous = journal.bufferJournal->CaptureDesired();
			const auto resultingCount = previous.elementCount + entries->size();
			journal.bufferJournal->RequestCapacity(capacityFor(resultingCount));
			journal.bufferJournal->AppendWrite(previous.elementCount,
				entryBytes(entries), resultingCount);
            journal.appends.push_back(std::move(entries));
        }
        journal.revision = revision;
        // The active-set journal is authoritative for graph-managed indirect
        // visibility. Static streaming appends here without updating legacy
        // Scene draw statistics, so leaving requestedCount on that polling path
        // clamped a complete active list to its startup sentinel count.
        std::uint64_t desiredCount = journal.base ? journal.base->size() : 0u;
        for (const auto& append : journal.appends) {
            if (append) desiredCount += append->size();
        }
        m_workloadToRequestedCount[workloadKey] = static_cast<unsigned int>(
            (std::min<std::uint64_t>)(desiredCount,
                (std::numeric_limits<unsigned int>::max)()));
        ++m_desiredMutationRevision;
        changed = true;
    }
    // Mutations are produced by the static-import bridge as well as Scene.
    // Keep the mailbox level-triggered instead of relying on a later Scene
    // polling update to happen to schedule its drain.
    if (changed) ScheduleDesiredBuild();
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
	for (const auto& [key, journal] : m_activeJournals) {
		if (journal.bufferJournal) {
			snapshot.activeCaptures.emplace(key, journal.bufferJournal->CaptureDesired());
		}
	}
    snapshot.viewIDs = m_viewIDs;
    snapshot.viewLifetimeRevisions = m_viewLifetimeRevisions;
    return snapshot;
}

void IndirectCommandBufferManager::ScheduleDesiredBuild() {
    if (m_stopping.load(std::memory_order_acquire) || !m_buildScope.Valid() ||
        !m_rendererStateRequests || !m_uploadService) return;
	if (m_admittedRootRevision.load(std::memory_order_acquire) >
		m_publishedRootRevision.load(std::memory_order_acquire)) {
		basic_telemetry::AddCounter("SARP.Indirect.DesiredStateMailboxCoalesced");
		return;
	}
	// During startup a frame can take hundreds of milliseconds. Wall-clock
	// debounce alone can then mint every backing in a ring before even one GPU
	// frame slot retires. Require a full frame-slot rotation between coherent
	// indirect publications; mutations remain in the latest-wins mailbox and the
	// per-frame PublishDesiredState call supplies the level-triggered retry. The
	// epoch is reset again on actual publication so a slow build cannot consume
	// the rotation interval before its resources become visible.
	constexpr std::uint64_t retirementEpochsPerPublication = 3u;
	const auto lastAdmissionRetirement =
		m_lastAdmissionRetirementEpoch.load(std::memory_order_acquire);
	const auto currentRetirement =
		br::render::VersionedGpuBufferFrameRetirementEpoch();
	if (lastAdmissionRetirement != 0u &&
		currentRetirement < lastAdmissionRetirement + retirementEpochsPerPublication) {
		basic_telemetry::AddCounter("SARP.Indirect.DesiredStateRetirementCoalesced");
		return;
	}
	if (
        m_buildScheduled.exchange(true, std::memory_order_acq_rel)) return;
    // This is a latest-wins mailbox.  A direct submit lets the worker race each
    // producer mutation and can mint thousands of nearly-identical indirect
    // roots during bulk scene import. Static discovery arrives continuously
    // for seconds; a frame-scale delay still creates more immutable roots than
	// the GPU can retire while uploads are saturated. Four publications per
	// second keeps progressive loading responsive while the retirement gate
	// prevents low-frame-rate bursts from outrunning bounded backing rings.
    const auto accepted = TaskSchedulerManager::GetInstance().ScheduleAfter(
		m_buildScope, std::chrono::milliseconds(250), TaskLane::Streaming,
        TaskDomain::General, "IndirectCommandBufferManager::BuildDesiredState",
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
    }
    const auto snapshotRevision = snapshot.revision;
    const auto submitted = BuildDesiredState(std::move(snapshot));
    bool newerDesiredState = false;
    {
        std::lock_guard lock(m_desiredMutex);
        if (submitted) m_consumedMutationRevision = snapshotRevision;
        newerDesiredState = submitted &&
            m_consumedMutationRevision != m_desiredMutationRevision;
    }

    // Publish at most one coalesced snapshot per frame. The preparation task
    // runs outside the single-slot RendererState domain so it cannot delay the
    // graph producers that consume these requests.
    m_buildScheduled.store(false, std::memory_order_release);
    // A mutation arriving while the single drain was active observes
    // m_buildScheduled=true and cannot enqueue a second task. Restore the
    // level-triggered mailbox wake after releasing the slot.
    if (newerDesiredState) ScheduleDesiredBuild();
}

bool IndirectCommandBufferManager::BuildDesiredState(DesiredSnapshot snapshot) {
    const auto started = std::chrono::steady_clock::now();
    auto input = std::make_shared<br::render::IndirectStateBuildInput>();
    input->materializeResources = true;
    input->incrementSize = snapshot.incrementSize;
    input->viewIDs.assign(snapshot.viewIDs.begin(), snapshot.viewIDs.end());
    std::ranges::sort(input->viewIDs);
    input->workloads.reserve(snapshot.requestedCounts.size());

    std::vector<br::render::ArtifactRequirement> requirements;
    // Exact dependency IDs do not themselves own lifetime. Keep every request
    // handle alive until the indirect root has synchronously installed its
    // recipe pins. Without this bridge lease, a fast buffer producer can reach
    // GPU-ready and be reclaimed while this loop is still assembling the rest
    // of the workload closure, permanently blocking the eventual consumer on
    // a generation that no longer exists.
    std::vector<br::render::ArtifactVersionHandle> dependencyHandles;
    const auto cachedHandle = [this](br::render::ArtifactAddress address,
        std::uint64_t revision) -> br::render::ArtifactVersionHandle {
        std::lock_guard lock(m_submissionCacheMutex);
        const auto found = m_submittedArtifacts.find(address);
        return found != m_submittedArtifacts.end() && found->second.revision == revision
            ? found->second.handle : br::render::ArtifactVersionHandle{};
    };
    const auto cacheHandle = [this](br::render::ArtifactAddress address,
        std::uint64_t revision, const br::render::ArtifactVersionHandle& handle) {
        std::lock_guard lock(m_submissionCacheMutex);
        m_submittedArtifacts.insert_or_assign(address, SubmittedArtifact{ revision, handle });
    };
    if (snapshot.activeMaterialRevision == 0) {
        basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage", 1);
        return false;
    }
    requirements.push_back(br::render::ReadyGate(
        br::render::ArtifactAddress{ br::render::ArtifactKind::MaterialTable, 0, 0 },
        br::render::ArtifactReadiness::UploadSubmitted));
    if (!snapshot.objectBufferRequirement) {
        basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage", 2);
        return false;
    }
    if (snapshot.objectBufferRequirement) {
        requirements.push_back(br::render::LatestAtLeast(
            snapshot.objectBufferRequirement->key,
            snapshot.objectBufferRequirement->minimumRevision,
            snapshot.objectBufferRequirement->requiredReadiness));
    }
    for (const auto viewID : input->viewIDs) {
        const auto lifetimeRevision = snapshot.viewLifetimeRevisions.at(viewID);
        const br::render::ArtifactKey lifetimeKey{
            br::render::ArtifactKind::ViewLifetime, viewID, 0 };
        auto lifetimeHandle = cachedHandle(lifetimeKey, lifetimeRevision);
        if (!lifetimeHandle) {
            auto lifetime = std::make_shared<br::render::ViewLifetimeArtifact>(
                br::render::ViewLifetimeArtifact{ viewID, lifetimeRevision });
            const auto lifetimeRequest = m_rendererStateRequests->RequestExact(
                lifetimeKey, lifetimeRevision, {},
                br::render::ArtifactPayload::Make<br::render::ViewLifetimeArtifact>(
                    std::move(lifetime)),
                (viewID << 1u) ^ lifetimeRevision ^ 0x564945574c494645ull);
            if (!lifetimeRequest) {
                basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage", 3);
                basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStatus",
                    static_cast<std::int64_t>(lifetimeRequest.status));
                return false;
            }
            lifetimeHandle = lifetimeRequest.Handle();
            cacheHandle(lifetimeKey, lifetimeRevision, lifetimeHandle);
        }
        requirements.push_back(br::render::LatestAtLeast(
            lifetimeKey, lifetimeRevision, br::render::ArtifactReadiness::GpuReady));
        dependencyHandles.push_back(std::move(lifetimeHandle));
    }

    std::uint64_t sourceEntryCount = 0;
    std::uint64_t deferredEntryCount = 0;
    std::uint64_t safeDrawCount = 0;
    std::uint32_t nonEmptyWorkloads = 0;
    const auto roundUp = [increment = snapshot.incrementSize](unsigned int value) {
        const auto rounded = ((static_cast<std::uint64_t>(value) + increment - 1u) /
            increment) * increment;
        return static_cast<unsigned int>((std::min<std::uint64_t>)(rounded,
            (std::numeric_limits<unsigned int>::max)()));
    };
    const auto growCapacity = [&](unsigned int current, unsigned int required) {
        if (required <= current) return current;
        // A +increment capacity class minted one immutable buffer version for
        // nearly every thousand discovered draws. Large loads consequently
        // produced hundreds of generations per workload/view before any frame
        // could retire them. Keep the requested granularity, but grow capacity
        // geometrically so ordinary appends remain within the current class.
        const auto geometric = static_cast<std::uint64_t>(current) +
            (std::max<std::uint64_t>)(current / 2u, snapshot.incrementSize);
        return (std::max)(roundUp(required), static_cast<unsigned int>(
            (std::min<std::uint64_t>)(geometric,
                (std::numeric_limits<unsigned int>::max)())));
    };
    for (const auto& [key, requestedCount] : snapshot.requestedCounts) {
        const auto journalFound = snapshot.activeJournals.find(key);
		const auto captureFound = snapshot.activeCaptures.find(key);
        const auto idFound = snapshot.workloadIDs.find(key);
        if (journalFound == snapshot.activeJournals.end() ||
			captureFound == snapshot.activeCaptures.end() ||
			idFound == snapshot.workloadIDs.end() || journalFound->second.revision == 0) continue;
        const auto& journal = journalFound->second;
		const auto& activeCapture = captureFound->second;
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
        // independently. Keep the complete immutable active-list version even
        // when its draw-record rows have not reached the coherent root yet. The
        // root's safe count below temporarily clamps execution, and the later
        // extent-only mutation can then reuse this complete list. Filtering here
        // permanently cached a truncated (often empty) artifact under the active
        // journal revision, so an extent-only successor could never recover the
        // missing static entries.
        deferredEntryCount += static_cast<std::uint64_t>(std::ranges::count_if(
            entries, [&](const auto& entry) {
                return entry.drawRecordIndex >= snapshot.residentDrawRecordCount;
            }));

        br::render::IndirectWorkloadInputDTO dto{};
        dto.key = key;
        dto.activeListArtifactKey = {
            br::render::ArtifactKind::ActiveDrawList, idFound->second, 0 };
        dto.requestedCount = requestedCount;
        dto.residentDrawRecordCount = static_cast<std::uint32_t>(
            (std::min<std::uint64_t>)(snapshot.residentDrawRecordCount,
                (std::numeric_limits<std::uint32_t>::max)()));
        dto.activeListRevision = activeCapture.writeSequence;
        dto.activeEntries = entries;

        const auto activeRevision = (std::max<std::uint64_t>)(journal.revision, 1u);
        auto activeListHandle = cachedHandle(dto.activeListArtifactKey, activeRevision);
        if (!activeListHandle) {
            auto activeInput = std::make_shared<br::render::VersionedGpuBufferBuildInput>();
            activeInput->uploadService = m_uploadService;
            activeInput->debugName = "PublishedActiveDrawList";
            activeInput->writeSequence = dto.activeListRevision;
            activeInput->elementStride = sizeof(br::render::ActiveDrawEntryDTO);
			activeInput->elementCount = activeCapture.elementCount;
			activeInput->capacity = activeCapture.capacity;
            activeInput->catalogOwner = br::render::PublishedFragmentKind::ActiveDrawLists;
            activeInput->catalogUsage = br::render::PublishedResourceUsage::ActiveDrawList;
            activeInput->catalogVariant = static_cast<std::uint64_t>(key.compileFlags) |
                (static_cast<std::uint64_t>(key.skinnedShadowCaster) << 62u) |
                (static_cast<std::uint64_t>(key.clodOnly) << 63u);
			activeInput->previous = activeCapture.previous;
			activeInput->backingPool = journal.backingPool;
			activeInput->writes = activeCapture.writes;
			activeInput->image = activeCapture.image;
			activeInput->journalBaseSequence = activeCapture.journalBaseSequence;
            const auto activeListRequest = m_rendererStateRequests->SubmitLatest({
                dto.activeListArtifactKey, activeRevision, {},
                br::render::ArtifactPayload::Make<br::render::VersionedGpuBufferBuildInput>(
                    std::move(activeInput)),
                (activeRevision << 1u) ^ idFound->second ^ 0x414354495645ull });
            if (!activeListRequest) {
                basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage", 4);
                basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStatus",
                    static_cast<std::int64_t>(activeListRequest.status));
                return false;
            }
            activeListHandle = activeListRequest.Handle();
            cacheHandle(dto.activeListArtifactKey, activeRevision, activeListHandle);
        }
        requirements.push_back(br::render::Exact(
            activeListHandle.version, br::render::ArtifactReadiness::UploadSubmitted));
        dependencyHandles.push_back(std::move(activeListHandle));

        const auto safeCount = static_cast<unsigned int>((std::min<std::uint64_t>)({
            requestedCount, entries.size(), dto.residentDrawRecordCount }));
        safeDrawCount += safeCount;
        nonEmptyWorkloads += safeCount != 0u ? 1u : 0u;
        auto& capacity = snapshot.capacities[key];
        capacity = safeCount == 0u ? capacity : growCapacity(capacity, safeCount);
        dto.minimumCapacity = capacity;
        if (safeCount != 0u) {
            for (const auto viewID : input->viewIDs) {
				// A view ID may be destroyed and later reused. Capacity alone is
				// therefore not a version identity: after Release it could match a
				// signature tombstone and return AlreadyDesired without recreating the
				// address node. Include the monotonic view lifetime incarnation.
				const auto lifetimeRevision = snapshot.viewLifetimeRevisions.at(viewID);
				const auto argumentRevision =
					(lifetimeRevision << 32u) |
					(std::max<std::uint64_t>)(capacity, 1u);
                const br::render::ArtifactKey argumentKey{
					br::render::ArtifactKind::BufferVersion, idFound->second, viewID };
                auto argumentHandle = cachedHandle(argumentKey, argumentRevision);
                if (!argumentHandle) {
                    auto argumentInput =
                        std::make_shared<br::render::VersionedGpuBufferBuildInput>();
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
                    const auto argumentRequest = m_rendererStateRequests->SubmitLatest({
                        argumentKey, argumentRevision, {},
                        br::render::ArtifactPayload::Make<
                            br::render::VersionedGpuBufferBuildInput>(std::move(argumentInput)),
                        (argumentRevision << 1u) ^ idFound->second ^ viewID ^
                            0x415247554d454e54ull });
                    if (!argumentRequest) {
                        basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage", 5);
                        basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStatus",
                            static_cast<std::int64_t>(argumentRequest.status));
                        return false;
                    }
                    argumentHandle = argumentRequest.Handle();
                    cacheHandle(argumentKey, argumentRevision, argumentHandle);
                }
                // Argument buffers contain no persistent CPU-authored content;
                // newer incarnations only increase capacity for the same view
                // address. Resolve the latest safe backing instead of pinning
                // an obsolete view-lifetime allocation.
                requirements.push_back(br::render::LatestAtLeast(
                    argumentKey, argumentRevision,
                    br::render::ArtifactReadiness::UploadSubmitted));
                dependencyHandles.push_back(std::move(argumentHandle));
                dto.argumentArtifacts.push_back({ viewID, argumentKey });
            }
        }
        input->workloads.push_back(std::move(dto));
    }

    input->dependencyLeases.reserve(dependencyHandles.size());
    for (const auto& handle : dependencyHandles) {
        input->dependencyLeases.push_back(handle.lease);
    }

    {
        std::lock_guard lock(m_desiredMutex);
        for (const auto& [key, capacity] : snapshot.capacities) {
            m_workloadToCapacity[key] = (std::max)(m_workloadToCapacity[key], capacity);
        }
    }
    const auto rootRequest = m_rendererStateRequests->SubmitLatest({
        { br::render::ArtifactKind::IndirectWorkload, 0, 0 },
        snapshot.revision, std::move(requirements),
        br::render::ArtifactPayload::Make<br::render::IndirectStateBuildInput>(
            std::move(input)), snapshot.revision });
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
    basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStage",
        rootRequest ? 0 : 6);
    if (!rootRequest) {
        basic_telemetry::SetGauge("SARP.Indirect.DesiredStateAdmissionFailureStatus",
            static_cast<std::int64_t>(rootRequest.status));
    }
	if (rootRequest) {
		m_admittedRootRevision.store(snapshot.revision, std::memory_order_release);
		m_lastAdmissionRetirementEpoch.store(
			br::render::VersionedGpuBufferFrameRetirementEpoch(),
			std::memory_order_release);
	}
    return static_cast<bool>(rootRequest);
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
