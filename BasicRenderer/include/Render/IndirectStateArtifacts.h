#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Materials/TechniqueDescriptor.h"
#include "Render/AsyncStateGraph.h"

namespace org {
class DynamicGloballyIndexedResource;
class GloballyIndexedResource;
}

namespace br::render {

struct PublishedIndirectState;

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
    std::vector<ActiveDrawEntryDTO> activeEntries;
    struct ArgumentArtifact {
        std::uint64_t viewID = 0;
        ArtifactKey key{ ArtifactKind::BufferVersion, 0, 0 };
    };
    std::vector<ArgumentArtifact> argumentArtifacts;
};

struct IndirectStateBuildInput {
    bool materializeResources = false;
    std::uint32_t incrementSize = 1000;
    std::vector<std::uint64_t> viewIDs;
    std::vector<IndirectWorkloadInputDTO> workloads;
};

struct PublishedIndirectWorkload {
    std::uint64_t viewID = 0;
    DrawWorkloadKey key;
    std::shared_ptr<org::GloballyIndexedResource> indirectArguments;
    std::shared_ptr<org::GloballyIndexedResource> activeDrawList;
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
    std::uint64_t activeListRevision = 0;
};

struct PublishedIndirectState {
    std::vector<PublishedIndirectWorkload> workloads;

    [[nodiscard]] std::vector<const PublishedIndirectWorkload*> Find(
        std::uint64_t viewID, const RenderPhase& phase, bool clodOnly) const;
};

void RegisterIndirectStateProducer(AsyncStateGraph& graph);

} // namespace br::render
