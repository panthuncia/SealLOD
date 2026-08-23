#pragma once

#include <string>

#include "Animation/SkeletonArtifact.h"

namespace SkeletonArtifactValidation {

// Validates the on-disk dependency without consulting the process-local
// Skeleton registry. CLOD metadata must not be considered a cache hit when
// its referenced artifact is missing, stale, truncated, or corrupt.
bool Validate(const SkeletonArtifactReference& reference, std::string* error = nullptr);

}
