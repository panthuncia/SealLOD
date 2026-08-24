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
#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Render/VersionedGpuBufferArtifacts.h"

IndirectCommandBufferManager::IndirectCommandBufferManager() = default;

IndirectCommandBufferManager::~IndirectCommandBufferManager() {
}

void IndirectCommandBufferManager::RegisterWorkload(const DrawWorkloadKey& workloadKey) {
    EnsureWorkloadRegistered(workloadKey);
}

void IndirectCommandBufferManager::CreateBuffersForView(uint64_t viewID) {
    if (m_viewIDs.insert(viewID).second) {
        ++m_viewLifetimeRevisions[viewID];
        ++m_desiredMutationRevision;
    }
}

void IndirectCommandBufferManager::UnregisterBuffers(uint64_t viewID) {
    if (m_viewIDs.erase(viewID) != 0) ++m_desiredMutationRevision;
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

    for (const auto& [workloadKey, count] : deduped) {
        EnsureWorkloadRegistered(workloadKey);
        auto& desired = m_workloadToRequestedCount[workloadKey];
        if (desired != count) {
            desired = count;
            ++m_desiredMutationRevision;
        }
    }
}

void IndirectCommandBufferManager::SetRendererStateServices(
    br::render::RendererStateRequestService* requests, org::runtime::IUploadService* uploads) {
    m_rendererStateRequests = requests;
    m_uploadService = uploads;
}

void IndirectCommandBufferManager::PublishDesiredState(
	ObjectManager& objectManager, MaterialManager& materialManager) {
    BT_ZONE_SCOPE("IndirectCommandBufferManager::PublishDesiredState");
    if (!m_rendererStateRequests || !m_uploadService) return;
	const auto objectBufferRequirement = objectManager.DesiredBufferStateRequirement();
	const auto materialRevision = materialManager.DesiredPublishedStateRevision();
    const auto residentDrawRecordCount = objectManager.GetResidentInstanceDrawRecordCount();
    bool changed = m_consumedMutationRevision != m_desiredMutationRevision ||
        m_lastObjectBufferRevision != (objectBufferRequirement ? objectBufferRequirement->minimumRevision : 0u) ||
        m_lastMaterialRevision != materialRevision ||
        m_lastResidentDrawRecordCount != residentDrawRecordCount;
    for (const auto& [key, requestedCount] : m_workloadToRequestedCount) {
        auto active = objectManager.TryGetActiveDrawSetIndices(key);
        if (!active) continue;
        auto& snapshot = m_activeSnapshots[key];
        if (snapshot.revision != active->MutationRevision()) {
            const auto entries = active->SnapshotActiveEntries();
            snapshot.entries.clear();
            snapshot.entries.reserve(entries.size());
            for (const auto& entry : entries) {
                snapshot.entries.push_back({ entry.drawRecordIndex, entry.generation });
            }
            snapshot.revision = active->MutationRevision();
            changed = true;
        }
    }
    if (!changed) {
        if (m_graphRevision != 0 && ++m_graphDiagnosticTicks >= 120u) {
            m_graphDiagnosticTicks = 0;
            const auto diagnostic = m_rendererStateRequests->Diagnose(
                { br::render::ArtifactKind::IndirectWorkload, 0, 0 });
            const auto source = br::render::PublishedStateSource::ProcessSource();
            const auto published = source ? source->Load() : nullptr;
            spdlog::info(
                "Indirect state progress: desiredRevision={} artifactRevision={} readiness={} blockers={} ageMs={} publishedRevision={} chain='{}' error='{}'",
                diagnostic.desiredRevision, diagnostic.artifact.revision,
                static_cast<unsigned int>(diagnostic.artifact.readiness), diagnostic.blockers.size(),
                diagnostic.stateAge.count() / 1000,
                published ? published->indirectWorkloads.revision : 0u,
                diagnostic.blockerChain, diagnostic.error);
        }
        return;
    }

    auto input = std::make_shared<br::render::IndirectStateBuildInput>();
    input->materializeResources = true;
    input->incrementSize = m_incrementSize;
    input->viewIDs.reserve(m_viewIDs.size());
    for (const auto viewID : m_viewIDs) input->viewIDs.push_back(viewID);
    input->workloads.reserve(m_workloadToRequestedCount.size());
    std::vector<br::render::ArtifactRequirement> requirements;
    requirements.reserve(m_workloadToRequestedCount.size());
	if (objectBufferRequirement) requirements.push_back(*objectBufferRequirement);
	if (materialRevision != 0) {
		requirements.push_back({ { br::render::ArtifactKind::MaterialTable, 0, 0 },
			materialRevision, br::render::ArtifactReadiness::GpuReady });
	}
    for (const auto viewID : input->viewIDs) {
        const auto lifetimeRevision = m_viewLifetimeRevisions.at(viewID);
        const br::render::ArtifactKey lifetimeKey{
            br::render::ArtifactKind::ViewLifetime, viewID, 0 };
        auto lifetime = std::make_shared<br::render::ViewLifetimeArtifact>(
            br::render::ViewLifetimeArtifact{ viewID, lifetimeRevision });
        (void)m_rendererStateRequests->Request(lifetimeKey, lifetimeRevision, {},
            br::render::ArtifactPayload::Make<br::render::ViewLifetimeArtifact>(std::move(lifetime)));
        requirements.push_back({ lifetimeKey, lifetimeRevision,
            br::render::ArtifactReadiness::GpuReady });
    }
    std::uint64_t sourceEntryCount = 0;
    std::uint64_t safeDrawCount = 0;
    std::uint32_t nonEmptyWorkloads = 0;
    for (const auto& [key, requestedCount] : m_workloadToRequestedCount) {
        auto active = objectManager.TryGetActiveDrawSetIndices(key);
        if (!active) continue;
        const auto cached = m_activeSnapshots.find(key);
        if (cached == m_activeSnapshots.end()) continue;
        const auto& entries = cached->second.entries;
        sourceEntryCount += entries.size();
        const auto drawRecordExtent = objectManager.GetResidentInstanceDrawRecordCount();
        const auto invalidEntry = std::ranges::find_if(entries, [drawRecordExtent](const auto& entry) {
            return entry.drawRecordIndex >= drawRecordExtent;
        });
        if (invalidEntry != entries.end()) {
            spdlog::error(
                "Indirect state rejected workload with out-of-range draw record: index={} extent={} flags={} phase={} clodOnly={}",
                invalidEntry->drawRecordIndex, drawRecordExtent, static_cast<std::uint64_t>(key.compileFlags),
                key.renderPhase.hash, key.clodOnly);
            continue;
        }
        br::render::IndirectWorkloadInputDTO dto{};
        dto.key = key;
        dto.activeListArtifactKey = {
            br::render::ArtifactKind::ActiveDrawList,
            WorkloadID(key), 0 };
        dto.requestedCount = requestedCount;
        dto.residentDrawRecordCount = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            objectManager.GetResidentInstanceDrawRecordCount(),
            (std::numeric_limits<std::uint32_t>::max)()));
        dto.minimumCapacity = m_workloadToCapacity[key];
        dto.activeListRevision = cached->second.revision;
        dto.activeEntries = entries;

        auto activeInput = std::make_shared<br::render::VersionedGpuBufferBuildInput>();
        activeInput->uploadService = m_uploadService;
        activeInput->debugName = "PublishedActiveDrawList";
        activeInput->writeSequence = dto.activeListRevision;
        activeInput->elementStride = sizeof(br::render::ActiveDrawEntryDTO);
        activeInput->elementCount = dto.activeEntries.size();
        activeInput->capacity = dto.activeEntries.size();
        activeInput->catalogOwner = br::render::PublishedFragmentKind::ActiveDrawLists;
        activeInput->catalogUsage = br::render::PublishedResourceUsage::ActiveDrawList;
        activeInput->catalogVariant = static_cast<std::uint64_t>(key.compileFlags) |
            (static_cast<std::uint64_t>(key.skinnedShadowCaster) << 62u) |
            (static_cast<std::uint64_t>(key.clodOnly) << 63u);
        activeInput->bytes.resize(dto.activeEntries.size() * sizeof(br::render::ActiveDrawEntryDTO));
        if (!activeInput->bytes.empty()) {
            std::memcpy(activeInput->bytes.data(), dto.activeEntries.data(), activeInput->bytes.size());
        }
        const auto activeRevision = (std::max<std::uint64_t>)(dto.activeListRevision, 1u);
        (void)m_rendererStateRequests->Request(dto.activeListArtifactKey, activeRevision, {},
            br::render::ArtifactPayload::Make<br::render::VersionedGpuBufferBuildInput>(std::move(activeInput)));
        requirements.push_back({ dto.activeListArtifactKey, activeRevision,
            br::render::ArtifactReadiness::GpuReady, br::render::DependencyPolicy::AllOf });
        const auto safeCount = static_cast<unsigned int>((std::min<std::uint64_t>)({
            requestedCount, active->ResidentSize(), dto.residentDrawRecordCount }));
        safeDrawCount += safeCount;
        nonEmptyWorkloads += safeCount != 0u ? 1u : 0u;
        m_workloadToPublishedCount[key] = safeCount;
        m_workloadToCapacity[key] = (std::max)(m_workloadToCapacity[key],
            safeCount == 0u ? 0u : RoundUp(safeCount));
        dto.minimumCapacity = m_workloadToCapacity[key];
        if (safeCount != 0u) {
            for (const auto viewID : input->viewIDs) {
                const br::render::ArtifactKey argumentKey{
                    br::render::ArtifactKind::BufferVersion,
                    WorkloadID(key),
                    viewID };
                auto argumentInput = std::make_shared<br::render::VersionedGpuBufferBuildInput>();
                argumentInput->uploadService = m_uploadService;
                argumentInput->debugName = "PublishedIndirectArguments";
                argumentInput->writeSequence = dto.minimumCapacity;
                argumentInput->elementStride = sizeof(DispatchMeshIndirectCommand);
                argumentInput->elementCount = dto.minimumCapacity;
                argumentInput->capacity = dto.minimumCapacity;
                argumentInput->unorderedAccess = true;
                argumentInput->indirectArguments = true;
                argumentInput->catalogOwner = br::render::PublishedFragmentKind::IndirectWorkloads;
                argumentInput->catalogUsage = br::render::PublishedResourceUsage::IndirectArguments;
                argumentInput->catalogVariant = static_cast<std::uint64_t>(key.compileFlags) |
                    (static_cast<std::uint64_t>(key.skinnedShadowCaster) << 62u) |
                    (static_cast<std::uint64_t>(key.clodOnly) << 63u);
                const auto argumentRevision = (std::max<std::uint64_t>)(dto.minimumCapacity, 1u);
                (void)m_rendererStateRequests->Request(argumentKey, argumentRevision, {},
                    br::render::ArtifactPayload::Make<br::render::VersionedGpuBufferBuildInput>(
                        std::move(argumentInput)));
                requirements.push_back({ argumentKey, argumentRevision,
                    br::render::ArtifactReadiness::GpuReady, br::render::DependencyPolicy::AllOf });
                dto.argumentArtifacts.push_back({ viewID, argumentKey });
            }
        }
        input->workloads.push_back(std::move(dto));
    }
    m_consumedMutationRevision = m_desiredMutationRevision;
    m_lastObjectBufferRevision = objectBufferRequirement ? objectBufferRequirement->minimumRevision : 0u;
    m_lastMaterialRevision = materialRevision;
    m_lastResidentDrawRecordCount = residentDrawRecordCount;
    m_graphDiagnosticTicks = 0;
    ++m_graphRevision;
    spdlog::info(
        "Indirect state request: revision={} views={} workloads={}/{} sourceEntries={} safeDraws={} drawRecordExtent={} dependencies={}",
        m_graphRevision, input->viewIDs.size(), nonEmptyWorkloads, input->workloads.size(),
        sourceEntryCount, safeDrawCount, objectManager.GetResidentInstanceDrawRecordCount(), requirements.size());
    (void)m_rendererStateRequests->Request(
        { br::render::ArtifactKind::IndirectWorkload, 0, 0 }, m_graphRevision, std::move(requirements),
        br::render::ArtifactPayload::Make<br::render::IndirectStateBuildInput>(std::move(input)));
}

void IndirectCommandBufferManager::SetIncrementSize(unsigned int incrementSize) {
    const auto desired = incrementSize == 0u ? 1u : incrementSize;
    if (m_incrementSize != desired) {
        m_incrementSize = desired;
        ++m_desiredMutationRevision;
    }
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

std::uint64_t IndirectCommandBufferManager::WorkloadID(const DrawWorkloadKey& workloadKey) const {
    const auto found = m_workloadIDs.find(workloadKey);
    return found == m_workloadIDs.end() ? 0u : found->second;
}
