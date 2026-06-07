#include "Managers/IndirectCommandBufferManager.h"

#include "Managers/Singletons/ResourceManager.h"
#include "Resources/ResourceGroup.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/DynamicResource.h"
#include "Render/IndirectCommand.h"
#include "Resources/Components.h"
#include "Resources/Buffers/Buffer.h"
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
        rg::memory::SetResourceUsageHint(*res, "Indirect command buffers");
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

        // Set the workload count to the last known value for this workload
        auto itCount = m_workloadToLastCount.find(workloadKey);
        if (itCount != m_workloadToLastCount.end()) {
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
    const WorkloadCountUpdate update{ workloadKey, numDraws };
    UpdateBuffersForWorkloads(std::span<const WorkloadCountUpdate>(&update, 1));
}

void IndirectCommandBufferManager::UpdateBuffersForWorkloads(std::span<const WorkloadCountUpdate> updates) {
    if (updates.empty()) {
        return;
    }

    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> deduped;
    deduped.reserve(updates.size());
    for (const auto& update : updates) {
        deduped[update.workloadKey] = update.count;
    }

    std::vector<WorkloadCountUpdate> changed;
    changed.reserve(deduped.size());
    for (const auto& [workloadKey, count] : deduped) {
        EnsureWorkloadRegistered(workloadKey);
        const auto previousCount = m_workloadToLastCount[workloadKey];
        const auto previousCapacity = m_workloadToCapacity[workloadKey];
        const auto nextCapacity = RoundUp(count);
        if (previousCount == count && nextCapacity <= previousCapacity) {
            continue;
        }

        m_workloadToLastCount[workloadKey] = count;
        if (nextCapacity > previousCapacity) {
            m_workloadToCapacity[workloadKey] = nextCapacity;
        }
        changed.push_back(WorkloadCountUpdate{ workloadKey, count });
    }
    if (changed.empty()) {
        return;
    }

    for (auto& [viewID, perView] : m_viewIDToBuffers) {
        for (const auto& update : changed) {
            const auto capacity = m_workloadToCapacity[update.workloadKey];
            auto it = perView.buffersByWorkload.find(update.workloadKey);
            if (capacity == 0) {
                if (it != perView.buffersByWorkload.end()) {
                    it->second.count = update.count;
                }
                continue;
            }

            if (it != perView.buffersByWorkload.end()) {
                if (!it->second.buffer ||
                    !it->second.buffer->GetResource() ||
                    capacity > RoundUp(it->second.count)) {
                    it->second.buffer->SetResource(CreateIndirectCommandBufferResource(update.workloadKey, viewID, capacity));
                }
                it->second.count = update.count;
                continue;
            }

            auto dyn = CreateIndirectWorkloadResource(update.workloadKey, viewID, capacity);
            perView.buffersByWorkload.emplace(update.workloadKey, IndirectWorkload{ dyn, update.count });
            m_indirectCommandsResourceGroup->AddResource(dyn);
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
        if (key.renderPhase == phase && key.clodOnly == clodOnly) {
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
        if (key.renderPhase == phase && key.clodOnly == clodOnly) {
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
    if (!m_workloadToLastCount.count(workloadKey)) {
        m_workloadToLastCount[workloadKey] = 0;
    }
}
