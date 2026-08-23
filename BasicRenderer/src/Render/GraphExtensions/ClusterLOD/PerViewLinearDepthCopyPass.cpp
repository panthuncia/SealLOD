#include "Render/GraphExtensions/ClusterLOD/PerViewLinearDepthCopyPass.h"

#include "Managers/ViewManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

PerViewLinearDepthCopyPass::PerViewLinearDepthCopyPass(bool writeProjectedDepth)
    : m_writeProjectedDepth(writeProjectedDepth) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/canonicalSurface.hlsl",
        L"PerViewPrimaryDepthCopyCS",
        {},
        "PerViewPrimaryDepthCopyPSO");
}

void PerViewLinearDepthCopyPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithShaderResource(Builtin::PrimaryCamera::VisibilityTexture)
        .WithUnorderedAccess(Builtin::PrimaryCamera::LinearDepthMap,
            Builtin::PrimaryCamera::ProjectedDepthTexture, Builtin::Surface::DeviceDepth);
    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void PerViewLinearDepthCopyPass::Setup() {
    m_pProjectedDepthTexture = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::ProjectedDepthTexture);
    m_pCanonicalDeviceDepth = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Surface::DeviceDepth);
}

PassReturn PerViewLinearDepthCopyPass::Execute(PassExecutionContext& executionContext) {
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};

    context.viewManager->ForEachView([&](uint64_t viewID) {
        const auto* view = context.viewManager->Get(viewID);
        if (!view || !view->gpu.visibilityBuffer || !view->gpu.linearDepthMap) {
            return;
        }

        rootConstants[UintRootConstant0] = view->gpu.visibilityBuffer->GetSRVInfo(0).slot.index;
        rootConstants[UintRootConstant1] = view->gpu.linearDepthMap->GetUAVShaderVisibleInfo(0).slot.index;
        rootConstants[UintRootConstant2] = view->gpu.visibilityBuffer->GetWidth();
        rootConstants[UintRootConstant3] = view->gpu.visibilityBuffer->GetHeight();

        // Only write projected depth for the primary camera view
        if (m_writeProjectedDepth && view->flags.primaryCamera && m_pProjectedDepthTexture) {
            rootConstants[UintRootConstant4] = m_pProjectedDepthTexture->GetUAVShaderVisibleInfo(0).slot.index;
            rootConstants[UintRootConstant7] = m_pCanonicalDeviceDepth
                ? m_pCanonicalDeviceDepth->GetUAVShaderVisibleInfo(0).slot.index : 0xFFFFFFFFu;
            // Extract M[2][2] and M[3][2] from the unjittered projection matrix (row-major)
            const auto& proj = view->cameraInfo.unjitteredProjection;
            rootConstants[UintRootConstant5] = as_uint(DirectX::XMVectorGetZ(proj.r[2])); // M[2][2]
            rootConstants[UintRootConstant6] = as_uint(DirectX::XMVectorGetZ(proj.r[3])); // M[3][2]
        } else {
            rootConstants[UintRootConstant4] = 0xFFFFFFFF; // sentinel: skip projected depth write
            rootConstants[UintRootConstant7] = 0xFFFFFFFF;
        }

        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            rootConstants);

        const uint32_t groupsX = (rootConstants[UintRootConstant2] + 7u) / 8u;
        const uint32_t groupsY = (rootConstants[UintRootConstant3] + 7u) / 8u;
        commandList.Dispatch(groupsX, groupsY, 1);
    });

    return {};
}

void PerViewLinearDepthCopyPass::Cleanup() {}
