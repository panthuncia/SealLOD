#include "Managers/Singletons/CommandSignatureManager.h"
#include "Render/IndirectCommand.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Utilities/Utilities.h"

void CommandSignatureManager::Initialize() {

    auto device = DeviceManager::GetInstance().GetDevice();

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::DispatchMesh }
    };
    auto& graphicsLayout = PSOManager::GetInstance().GetRootSignature();
    auto result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, std::size(args)), sizeof(DispatchMeshIndirectCommand) },
        graphicsLayout.GetHandle(), m_dispatchMeshCommandSignature);

    rhi::IndirectArg args2[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    auto& computeLayout = PSOManager::GetInstance().GetComputeRootSignature();
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args2, std::size(args2)), sizeof(DispatchIndirectCommand) },
        computeLayout.GetHandle(), m_dispatchCommandSignature);

    rhi::IndirectArg rawDispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(rawDispatchArgs, std::size(rawDispatchArgs)), sizeof(D3D12_DISPATCH_ARGUMENTS) },
        computeLayout.GetHandle(), m_rawDispatchCommandSignature);

    // Used by the visibility buffer material evaluation pass
    rhi::IndirectArg materialEvaluationArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 4 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(materialEvaluationArgs, 2), sizeof(MaterialEvaluationIndirectCommand) },
        computeLayout.GetHandle(), m_materialEvaluationCommandSignature);

    rhi::IndirectArg terrainRegionMaterialEvaluationArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 5 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    result = device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(terrainRegionMaterialEvaluationArgs, 2), sizeof(TerrainRegionMaterialEvaluationIndirectCommand) },
        computeLayout.GetHandle(), m_terrainRegionMaterialEvaluationCommandSignature);

}

void CommandSignatureManager::Cleanup() {
    m_dispatchMeshCommandSignature.Reset();
    m_dispatchCommandSignature.Reset();
    m_rawDispatchCommandSignature.Reset();
    m_materialEvaluationCommandSignature.Reset();
    m_terrainRegionMaterialEvaluationCommandSignature.Reset();
}
