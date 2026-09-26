#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <BasicRenderer/Extensions/ShaderCompilationTypes.h>
#include <rhi.h>
#include <BasicRenderer/Pipeline/PipelineKind.h>

namespace br::extensions {



rhi::PipelineLayoutHandle GetComputePipelineLayout();
rhi::PipelineLayoutHandle GetGraphicsPipelineLayout();
org::PipelineState MakeComputePipeline(
    rhi::PipelineLayoutHandle layout,
    const wchar_t* shaderPath,
    const wchar_t* entryPoint,
    std::vector<DxcDefine> defines = {},
    const char* debugName = nullptr,
    std::shared_ptr<const void> layoutOwner = {});
ShaderBundle CompileShaders(const ShaderInfoBundle& shaders);
org::PipelineState RegisterPipeline(
    org::PipelineState state,
    std::string id,
    std::string displayName,
    PipelineKind kind,
    std::function<org::PipelineState()> rebuild);

} // namespace br::extensions
