#pragma once

#include <cstdint>
#include <span>

class IndirectCommandBufferManager;
struct DrawWorkloadKey;
struct WorkloadCountUpdate;

namespace br::render {

// Serialized scene-ingestion boundary for indirect workload membership/counts.
// Callers publish intent; the backing manager owns journals and artifact builds.
class StaticWorkloadRequestService {
public:
    void Configure(IndirectCommandBufferManager* workloads) noexcept;
    [[nodiscard]] bool Available() const noexcept;
    void PublishCount(const DrawWorkloadKey& workload, unsigned int count) const;
    void PublishCounts(std::span<const WorkloadCountUpdate> updates) const;
    void RemoveView(std::uint64_t viewID) const;

private:
    IndirectCommandBufferManager* m_workloads = nullptr;
};

}
