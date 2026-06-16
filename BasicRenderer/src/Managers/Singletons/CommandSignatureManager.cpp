#include "Managers/Singletons/CommandSignatureManager.h"
#include "Render/IndirectCommand.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Utilities/Utilities.h"

#include <spdlog/spdlog.h>

namespace {
void LogCommandSignatureResult(const char* name, rhi::Result result) {
    if (!rhi::IsOk(result)) {
        spdlog::error(
            "CommandSignatureManager::Initialize failed to create {} command signature: result={}",
            name,
            static_cast<uint32_t>(result));
    }
}
}

void CommandSignatureManager::Initialize() {

    auto device = DeviceManager::GetInstance().GetDevice();

    if (DeviceManager::GetInstance().GetMeshShadersSupported()) {
        rhi::IndirectArg args[] = {
            {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
            {.kind = rhi::IndirectArgKind::DispatchMesh }
        };
        auto& graphicsLayout = PSOManager::GetInstance().GetRootSignature();
        const auto result = device.CreateCommandSignature(
            rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, std::size(args)), sizeof(DispatchMeshIndirectCommand) },
            graphicsLayout.GetHandle(), m_dispatchMeshCommandSignature);
        LogCommandSignatureResult("dispatch mesh", result);
    }
    else {
        spdlog::info("CommandSignatureManager::Initialize skipping dispatch mesh command signature because mesh shaders are not supported.");
    }

    rhi::IndirectArg args2[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    auto& computeLayout = PSOManager::GetInstance().GetComputeRootSignature();
    auto result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args2, std::size(args2)), sizeof(DispatchIndirectCommand) },
        computeLayout.GetHandle(), m_dispatchCommandSignature);
    LogCommandSignatureResult("dispatch", result);

    rhi::IndirectArg rawDispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(rawDispatchArgs, std::size(rawDispatchArgs)), sizeof(D3D12_DISPATCH_ARGUMENTS) },
        computeLayout.GetHandle(), m_rawDispatchCommandSignature);
    LogCommandSignatureResult("raw dispatch", result);

    // Used by the visibility buffer material evaluation pass
    rhi::IndirectArg materialEvaluationArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 4 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(materialEvaluationArgs, 2), sizeof(MaterialEvaluationIndirectCommand) },
        computeLayout.GetHandle(), m_materialEvaluationCommandSignature);
    LogCommandSignatureResult("material evaluation", result);

    rhi::IndirectArg terrainRegionMaterialEvaluationArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 5 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(terrainRegionMaterialEvaluationArgs, 2), sizeof(TerrainRegionMaterialEvaluationIndirectCommand) },
        computeLayout.GetHandle(), m_terrainRegionMaterialEvaluationCommandSignature);
    LogCommandSignatureResult("terrain region material evaluation", result);

}

void CommandSignatureManager::Cleanup() {
    m_dispatchMeshCommandSignature.Reset();
    m_dispatchCommandSignature.Reset();
    m_rawDispatchCommandSignature.Reset();
    m_materialEvaluationCommandSignature.Reset();
    m_terrainRegionMaterialEvaluationCommandSignature.Reset();
}
