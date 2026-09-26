#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Animation/SkeletonArtifact.h"

class Skeleton;
struct ClusterLODAssemblySkeletonData;

namespace SkeletonArtifactCache {

std::optional<SkeletonArtifactReference> Save(const ClusterLODAssemblySkeletonData& source, std::string* error = nullptr);
std::shared_ptr<const SkeletonArtifactData> Load(const SkeletonArtifactReference& reference, std::string* error = nullptr);
std::shared_ptr<Skeleton> ResolveSkeleton(const SkeletonArtifactReference& reference, std::string* error = nullptr);

}
