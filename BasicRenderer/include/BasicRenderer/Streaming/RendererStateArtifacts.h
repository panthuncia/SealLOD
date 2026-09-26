#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>

namespace org { class DynamicGloballyIndexedResource; class GloballyIndexedResource; }

namespace br::render {
struct PublishedGpuBufferVersion;
inline constexpr std::uint64_t kObjectPerObjectVariant = 1;
inline constexpr std::uint64_t kObjectInstanceTransformVariant = 2;
inline constexpr std::uint64_t kObjectDrawRecordVariant = 3;
inline constexpr std::uint64_t kObjectNormalMatrixVariant = 4;
inline constexpr std::uint64_t kObjectVisibilityGenerationVariant = 5;
inline constexpr std::uint64_t kObjectSkinnedPlacementVariant = 6;
inline constexpr std::uint64_t kObjectActiveSkinnedPlacementVariant = 7;
struct PublishedActiveSkinnedPlacement {
    std::uint32_t drawRecordIndex = 0;
    std::uint32_t generation = 0;
};


struct ObjectBufferDependencyDTO {
    ArtifactKey key{ ArtifactKind::BufferVersion, 0, 0 };
    std::uint64_t revision = 0;
    std::uint32_t elementStride = 0;
    std::uint64_t catalogVariant = 0;
};


struct PublishedObjectBufferState {
    std::vector<ObjectBufferDependencyDTO> buffers;
    std::vector<std::shared_ptr<const PublishedGpuBufferVersion>> versions;
    std::uint64_t coveredMutationGeneration = 0;
    std::uint32_t residentTransformCount = 0;
    std::shared_ptr<const std::vector<SkinnedAssemblyPlacementGPU>> placementRecords;
    std::shared_ptr<const std::vector<PublishedActiveSkinnedPlacement>> activePlacementEntries;

    [[nodiscard]] std::shared_ptr<const PublishedGpuBufferVersion> FindVersion(
        std::uint64_t catalogVariant) const;
};


struct PublishedIndirectWorkload {
    std::uint64_t viewID = 0;
    DrawWorkloadKey key;
    std::shared_ptr<org::GloballyIndexedResource> indirectArguments;
    std::shared_ptr<org::GloballyIndexedResource> activeDrawList;
    // The bindless index is part of this immutable publication. Consumers must
    // use it with the retained resource version instead of consulting a live
    // descriptor registry while preparing a queued frame.
    std::uint32_t activeDrawListSRVIndex = 0;
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
    std::uint64_t activeListRevision = 0;
};


struct PublishedIndirectState {
    // Exact graph-owned visibility-generation buffer selected with the active-list
    // closure. Culling must not fetch this from an independently advancing
    // DrawRecords catalog slot or generations can be compared across cuts.
    std::shared_ptr<org::GloballyIndexedResource> visibilityGenerations;
    std::uint32_t visibilityGenerationsSRVIndex = 0;
    // Rows this version owns. Active lists may name newer records; the backing
    // beyond this count holds another version's generations, so culling must
    // treat any index at or past it as not visible.
    std::uint32_t visibilityGenerationCount = 0;
    ArtifactVersionID drawRecordsRoot{};
    struct ActiveListVersion {
        std::uint64_t workloadID = 0;
        std::shared_ptr<const PublishedGpuBufferVersion> version;
    };
    std::vector<ActiveListVersion> activeListVersions;
    std::vector<PublishedIndirectWorkload> workloads;

    [[nodiscard]] std::vector<const PublishedIndirectWorkload*> Find(
        std::uint64_t viewID, const RenderPhase& phase, bool clodOnly) const;
};


}
