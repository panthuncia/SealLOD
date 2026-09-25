#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#include "Render/AsyncStateGraph.h"

class Skeleton;
namespace org { class Resource; }

namespace br::render {
struct PublishedGpuBufferVersion;
inline constexpr std::uint64_t PoseInverseBindTableVariant = 1;
inline constexpr std::uint64_t PoseBoneTransformTableVariant = 2;
inline constexpr std::uint64_t PoseInverseSkinTableVariant = 3;
inline constexpr std::uint64_t PoseInstanceInfoTableVariant = 4;

struct PublishedSkeletonInstance {
    std::shared_ptr<const Skeleton> baseSkeleton;
    std::uint32_t instanceSlot = 0xFFFFFFFFu;
    std::uint32_t transformOffsetMatrices = 0;
    std::uint32_t inverseSkinOffsetMatrices = 0;
    std::uint32_t boneCount = 0;
};

struct PoseStateBuildInput {
    std::uint64_t activeInstanceRevision = 0;
    std::vector<PublishedSkeletonInstance> activeInstances;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    // Immutable CPU images selected with the pose publication. Only inverse
    // bind data currently receives an immutable GPU table version. Bone/skin
    // and transient instance tables are frame-written palette outputs.
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
};

struct PublishedPoseState {
    std::uint64_t activeInstanceRevision = 0;
    std::vector<PublishedSkeletonInstance> activeInstances;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
    std::vector<std::shared_ptr<const PublishedGpuBufferVersion>> tableVersions;
};

void RegisterPoseStateProducer(AsyncStateGraph& graph);

} // namespace br::render
