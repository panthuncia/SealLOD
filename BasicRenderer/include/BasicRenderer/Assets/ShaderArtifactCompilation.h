#pragma once

#include <BasicRenderer/Extensions/ShaderCompilationTypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

struct ShaderVariantRequest;

namespace br::assets {

void RegisterHeadlessStaticImportSettings();
void InitializeShaderArtifactCompiler();
void PrecompileShaderArtifact(const ShaderVariantRequest& request);
void PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle);

struct ShaderArtifactPruneSummary {
    std::size_t examined = 0;
    std::size_t removed = 0;
    std::uint64_t bytesRemoved = 0;
};
// Deletes shader artifacts neither written nor loaded within maxAge (a hit refreshes an artifact's write time).
// Artifact keys carry the shader sources' content digest, so artifacts of older sources are never reused, only
// left behind; this reclaims them.
ShaderArtifactPruneSummary PruneUnusedShaderArtifacts(std::chrono::hours maxAge);

} // namespace br::assets
