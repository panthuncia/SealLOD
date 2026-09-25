#include "Render/StaticWorkloadRequestService.h"

#include "Managers/IndirectCommandBufferManager.h"

namespace br::render {

void StaticWorkloadRequestService::Configure(
    IndirectCommandBufferManager* workloads) noexcept {
    m_workloads = workloads;
}

bool StaticWorkloadRequestService::Available() const noexcept {
    return m_workloads != nullptr;
}

void StaticWorkloadRequestService::PublishCount(
    const DrawWorkloadKey& workload, unsigned int count) const {
    if (!m_workloads) return;
    m_workloads->RegisterWorkload(workload);
    m_workloads->UpdateBuffersForWorkload(workload, count);
}

void StaticWorkloadRequestService::PublishCounts(
    std::span<const WorkloadCountUpdate> updates) const {
    if (!m_workloads) return;
    for (const auto& update : updates) m_workloads->RegisterWorkload(update.workloadKey);
    m_workloads->UpdateBuffersForWorkloads(updates);
}

void StaticWorkloadRequestService::RemoveView(std::uint64_t viewID) const {
    if (m_workloads) m_workloads->UnregisterBuffers(viewID);
}

}
