#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/RendererStateArtifacts.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"

namespace org {
class DynamicGloballyIndexedResource;
class GloballyIndexedResource;
}

namespace br::render {

struct PublishedGpuBufferVersion;

struct ActiveDrawEntryDTO {
    std::uint32_t drawRecordIndex = 0;
    std::uint32_t generation = 0;
};

struct ViewLifetimeArtifact {
    std::uint64_t viewID = 0;
    std::uint64_t lifetimeRevision = 0;
};

struct IndirectWorkloadInputDTO {
    DrawWorkloadKey key;
    ArtifactKey activeListArtifactKey{ ArtifactKind::ActiveDrawList, 0, 0 };
    std::uint32_t requestedCount = 0;
    std::uint32_t residentDrawRecordCount = 0;
    std::uint32_t minimumCapacity = 0;
    std::uint64_t activeListRevision = 0;
    // Entry content is owned by the exact ActiveDrawList artifact. Keeping
    // only its logical extent makes aggregate construction O(workload count).
    std::uint64_t logicalEntryCount = 0;
    struct ArgumentArtifact {
        std::uint64_t viewID = 0;
        ArtifactKey key{ ArtifactKind::BufferVersion, 0, 0 };
    };
    std::vector<ArgumentArtifact> argumentArtifacts;
};

struct IndirectStateBuildInput {
    std::uint32_t incrementSize = 1000;
    std::vector<std::uint64_t> viewIDs;
    std::vector<IndirectWorkloadInputDTO> workloads;
    // Bridges the interval between dependency request and consumer
    // materialization. Exact version IDs are identity, not ownership.
    std::vector<ArtifactLease> dependencyLeases;
};

void RegisterIndirectStateProducer(AsyncStateGraph& graph);

} // namespace br::render
