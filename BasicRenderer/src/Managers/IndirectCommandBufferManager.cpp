#include "Managers/IndirectCommandBufferManager.h"

#include <algorithm>
#include <limits>

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
#include "Managers/Singletons/RendererECSManager.h"
#include "Render/MemoryIntrospectionAPI.h"

namespace
{
    std::string GetDebugNameForTechnique(TechniqueDescriptor technique) {
        std::string result;
        if (technique.compileFlags & MaterialCompileBlend) result += "Blend|";
        if (technique.compileFlags & MaterialCompileAlphaTest) result += "AlphaTest|";
        if (technique.compileFlags & MaterialCompileDoubleSided) result += "DoubleSided|";
        if (technique.compileFlags & MaterialCompileTextureStreaming) result += "TextureStreaming|";
        if (result.empty()) result = "None";
        else result.pop_back();
        return result;
    }

    std::shared_ptr<Buffer> CreateIndirectCommandBufferResource(
        const DrawWorkloadKey& workloadKey,
        uint64_t viewID,
        unsigned int capacity) {
        auto res = CreateIndexedStructuredBuffer(capacity, sizeof(DispatchMeshIndirectCommand), true, true);
        res->SetName(
            "IndirectCommandBuffer(flags=" + GetDebugNameForTechnique(TechniqueDescriptor{ {}, workloadKey.compileFlags })
            + ", phase=" + std::to_string(workloadKey.renderPhase.hash)
            + ", clodOnly=" + std::to_string(workloadKey.clodOnly ? 1 : 0)
            + ", view=" + std::to_string(viewID) + ")");
        org::memory::SetResourceUsageHint(*res, "Indirect command buffers");
        return res;
    }

    std::shared_ptr<DynamicGloballyIndexedResource> CreateIndirectWorkloadResource(
        const DrawWorkloadKey& workloadKey,
        uint64_t viewID,
        unsigned int capacity) {
        auto dyn = std::make_shared<DynamicGloballyIndexedResource>(
            CreateIndirectCommandBufferResource(workloadKey, viewID, capacity));
        auto entity = dyn->GetECSEntity();
        entity.set<Components::Resource>({ dyn });
        entity.add<Components::ParticipatesInPass>(RendererECSManager::GetInstance().GetRenderPhaseEntity(workloadKey.renderPhase));
        entity.add<Components::IsIndirectArguments>();
        if (workloadKey.clodOnly) {
            entity.add<Components::CLodOnlyDrawWorkload>();
        }
        else {
            entity.add<Components::GeneralDrawWorkload>();
        }
        return dyn;
    }
}

IndirectCommandBufferManager::IndirectCommandBufferManager() {
    m_indirectCommandsResourceGroup = std::make_shared<ResourceGroup>("IndirectCommandBuffers");
}

IndirectCommandBufferManager::~IndirectCommandBufferManager() {
}

void IndirectCommandBufferManager::RegisterWorkload(const DrawWorkloadKey& workloadKey) {
    EnsureWorkloadRegistered(workloadKey);
}

void IndirectCommandBufferManager::CreateBuffersForView(uint64_t viewID) {
    PerViewBuffers perView;

    // Create one buffer per workload with current capacity (may be 0 if not yet sized)
    for (auto const& [workloadKey, cap] : m_workloadToCapacity) {
        unsigned int size = cap;
        if (size == 0) continue; // not yet sized, will be created on first UpdateBuffersForFlags

        auto dyn = CreateIndirectWorkloadResource(workloadKey, viewID, size);
        m_indirectCommandsResourceGroup->AddResource(dyn);
        perView.buffersByWorkload[workloadKey] = { dyn, 0 };

        // Set the workload count to the last published value for this workload.
        auto itCount = m_workloadToPublishedCount.find(workloadKey);
        if (itCount != m_workloadToPublishedCount.end()) {
            perView.buffersByWorkload[workloadKey].count = itCount->second;
        }
    }

    m_viewIDToBuffers[viewID] = perView;
}

void IndirectCommandBufferManager::UnregisterBuffers(uint64_t viewID) {
    auto it = m_viewIDToBuffers.find(viewID);
    if (it == m_viewIDToBuffers.end()) return;

    auto& perView = it->second;

    for (auto& [_, dyn] : perView.buffersByWorkload) {
        m_indirectCommandsResourceGroup->RemoveResource(dyn.buffer->GetResource().get());
    }

    m_viewIDToBuffers.erase(it);
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
        m_workloadToRequestedCount[workloadKey] = count;
    }
}

void IndirectCommandBufferManager::CommitGpuVisibleSnapshot(ObjectManager& objectManager) {
    BT_ZONE_SCOPE("IndirectCommandBufferManager::CommitGpuVisibleSnapshot");
    BT_ZONE_VALUE(static_cast<std::uint64_t>(m_viewIDToBuffers.size()) * m_workloadToRequestedCount.size());
    for (auto& [viewID, perView] : m_viewIDToBuffers) {
        for (auto& [workloadKey, requestedCount] : m_workloadToRequestedCount) {
            BT_ZONE_SCOPE("IndirectCommandBufferManager::CommitGpuVisibleSnapshot::Workload");
            EnsureWorkloadRegistered(workloadKey);
            auto activeDrawSet = objectManager.TryGetActiveDrawSetIndices(workloadKey);
            const auto logicalActiveCount = activeDrawSet
                ? static_cast<uint64_t>(activeDrawSet->Size())
                : 0u;
            const auto residentActiveCount = activeDrawSet
                ? activeDrawSet->ResidentSize()
                : 0u;
            const auto residentDrawRecords = objectManager.GetResidentInstanceDrawRecordCount();
            const auto safeCount64 = (std::min<uint64_t>)(
                (std::min<uint64_t>)(requestedCount, logicalActiveCount),
                (std::min<uint64_t>)(residentActiveCount, residentDrawRecords));
            const auto safeCount = static_cast<unsigned int>((std::min<uint64_t>)(
                safeCount64,
                std::numeric_limits<unsigned int>::max()));
            const auto capacity = safeCount > 0u ? RoundUp(safeCount) : 0u;
            auto it = perView.buffersByWorkload.find(workloadKey);

            m_workloadToPublishedCount[workloadKey] = safeCount;
            if (capacity > m_workloadToCapacity[workloadKey]) {
                m_workloadToCapacity[workloadKey] = capacity;
            }

            if (capacity == 0u) {
                if (it != perView.buffersByWorkload.end()) {
                    it->second.count = 0u;
                    it->second.activeDrawCount = 0u;
                    it->second.activeDrawSetIndices = activeDrawSet;
                }
                continue;
            }

            if (it != perView.buffersByWorkload.end()) {
                if (!it->second.buffer ||
                    !it->second.buffer->GetResource() ||
                    capacity > RoundUp(it->second.count)) {
                    BT_ZONE_SCOPE("IndirectCommandBufferManager::CommitGpuVisibleSnapshot::ReplaceResource");
                    it->second.buffer->SetResource(CreateIndirectCommandBufferResource(workloadKey, viewID, capacity));
                }
                it->second.count = safeCount;
                it->second.activeDrawCount = safeCount;
                it->second.activeDrawSetIndices = activeDrawSet;
                continue;
            }

            {
                BT_ZONE_SCOPE("IndirectCommandBufferManager::CommitGpuVisibleSnapshot::CreateWorkload");
                auto dyn = CreateIndirectWorkloadResource(workloadKey, viewID, capacity);
                perView.buffersByWorkload.emplace(
                    workloadKey,
                    IndirectWorkload{ dyn, safeCount, safeCount, activeDrawSet });
                m_indirectCommandsResourceGroup->AddResource(dyn);
            }
        }
    }
}

void IndirectCommandBufferManager::SetIncrementSize(unsigned int incrementSize) {
    m_incrementSize = incrementSize == 0u ? 1u : incrementSize;
}

std::vector<std::pair<MaterialCompileFlags, IndirectWorkload>>
IndirectCommandBufferManager::GetBuffersForRenderPhase(uint64_t viewID, const RenderPhase& phase, bool clodOnly) const {
    std::vector<std::pair<MaterialCompileFlags, IndirectWorkload>> out;

    auto vIt = m_viewIDToBuffers.find(viewID);
    if (vIt == m_viewIDToBuffers.end()) return out;
    auto const& perView = vIt->second;

    for (auto const& [key, wl] : perView.buffersByWorkload) {
        if (key.renderPhase == phase && key.clodOnly == clodOnly && wl.buffer && wl.count > 0u) {
            out.emplace_back(key.compileFlags, wl);
        }
    }
    return out;
}

std::vector<IndirectBufferEntry>
IndirectCommandBufferManager::GetViewIndirectBuffersForRenderPhase(uint64_t viewID, const RenderPhase& phase, bool clodOnly) const {
    std::vector<IndirectBufferEntry> out;

    auto vit = m_viewIDToBuffers.find(viewID);
    if (vit == m_viewIDToBuffers.end()) return out;

    auto const& perView = vit->second;
    for (auto const& [key, wl] : perView.buffersByWorkload) {
        if (key.renderPhase == phase && key.clodOnly == clodOnly && wl.buffer && wl.count > 0u) {
            out.push_back(IndirectBufferEntry{ viewID, key, wl });
        }
    }
    return out;
}

// -------------------- helpers --------------------

void IndirectCommandBufferManager::EnsureWorkloadRegistered(const DrawWorkloadKey& workloadKey) {
    if (!m_workloadToCapacity.count(workloadKey)) {
        m_workloadToCapacity[workloadKey] = 0;
    }
    if (!m_workloadToRequestedCount.count(workloadKey)) {
        m_workloadToRequestedCount[workloadKey] = 0;
    }
    if (!m_workloadToPublishedCount.count(workloadKey)) {
        m_workloadToPublishedCount[workloadKey] = 0;
    }
}
