#include <BasicRenderer/Extensions/PipelineAccess.h>

#include "Pipeline/PipelineState/PSOManager.h"

namespace br::extensions {


rhi::PipelineLayoutHandle GetComputePipelineLayout()
{
    return PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
}

rhi::PipelineLayoutHandle GetGraphicsPipelineLayout()
{
    return PSOManager::GetInstance().GetRootSignature().GetHandle();
}

org::PipelineState MakeComputePipeline(
    rhi::PipelineLayoutHandle layout,
    const wchar_t* shaderPath,
    const wchar_t* entryPoint,
    std::vector<DxcDefine> defines,
    const char* debugName,
    std::shared_ptr<const void> layoutOwner)
{
    return PSOManager::GetInstance().MakeComputePipeline(
        layout, shaderPath, entryPoint, std::move(defines), debugName, std::move(layoutOwner));
}

ShaderBundle CompileShaders(const ShaderInfoBundle& shaders)
{
    return PSOManager::GetInstance().CompileShaders(shaders);
}

org::PipelineState RegisterPipeline(
    org::PipelineState state,
    std::string id,
    std::string displayName,
    PipelineKind kind,
    std::function<org::PipelineState()> rebuild)
{
    return PSOManager::GetInstance().RegisterExternalPipeline(
        std::move(state), std::move(id), std::move(displayName),
        kind, std::move(rebuild));
}

} // namespace br::extensions
