#pragma once

#include <BasicRenderer/Extensions/ShaderCompilationTypes.h>

struct ShaderVariantRequest;

namespace br::assets {

void RegisterHeadlessStaticImportSettings();
void InitializeShaderArtifactCompiler();
void PrecompileShaderArtifact(const ShaderVariantRequest& request);
void PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle);

} // namespace br::assets
