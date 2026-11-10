#pragma once

#include <BasicRenderer/Extensions/ShaderCompilationTypes.h>

#include <cstddef>
#include <string>
#include <string_view>

struct ShaderVariantRequest;

namespace br::assets {

void RegisterHeadlessStaticImportSettings();
void InitializeShaderArtifactCompiler();
void PrecompileShaderArtifact(const ShaderVariantRequest& request);
void PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle);
// Every shader bundle the renderer has compiled for D3D12 (its compile manifest, beside the shader store), resolved through
// the store again: after a shader edit, the variants only the runtime requests (terrain material evaluation, pass
// pipelines) are ready before it starts. Returns how many resolved.
std::size_t PrecompileRecordedShaderArtifacts();

// Ends a batch of the requests above: saves the shader store's index and logs one line, which this returns, giving what
// every request so far cost and why anything was not a plain hit. The store evicts when it opens (over
// SARP_SHADER_STORE_MAX_MB, default 4096: unreferenced artifacts first, then the longest unused, keeping whatever the
// last three starts used), so there is nothing to prune afterwards.
std::string FinishShaderArtifactBatch(std::string_view batch);

} // namespace br::assets
