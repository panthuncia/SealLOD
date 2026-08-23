#include "Render/GraphExtensions/ClusterLOD/AVBOITEarlyDepthBuildPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITEarlyDepthBuildRootConstants.h"

AVBOITEarlyDepthBuildPass::AVBOITEarlyDepthBuildPass(
    std::shared_ptr<Buffer> configBuffer,
    std::shared_ptr<PixelBuffer> zeroTransmittanceSliceTexture,
    std::shared_ptr<Buffer> tileCommandsBuffer,
    std::shared_ptr<Buffer> tileCountBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_zeroTransmittanceSliceTexture(std::move(zeroTransmittanceSliceTexture))
    , m_tileCommandsBuffer(std::move(tileCommandsBuffer))
    , m_tileCountBuffer(std::move(tileCountBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITEarlyDepthBuild.hlsl",
        L"CLodAVBOITEarlyDepthBuildCS",
        {},
        "CLod.AVBOITEarlyDepthBuild.PSO");
}

void AVBOITEarlyDepthBuildPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_configBuffer, m_zeroTransmittanceSliceTexture)
        .WithUnorderedAccess(m_tileCommandsBuffer, m_tileCountBuffer);
}

void AVBOITEarlyDepthBuildPass::Setup()
{
}

void AVBOITEarlyDepthBuildPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    if (!m_zeroTransmittanceSliceTexture || !m_tileCommandsBuffer || !m_tileCountBuffer) {
        return;
    }

    const uint32_t tileCapacity = m_zeroTransmittanceSliceTexture->GetWidth() * m_zeroTransmittanceSliceTexture->GetHeight();
    if (m_tileCommandsBuffer->GetSize() < static_cast<size_t>(tileCapacity) * sizeof(CLodAVBOITEarlyDepthTileIndirectCommand)) {
        m_tileCommandsBuffer->ResizeStructured(tileCapacity);
    }

    const uint32_t zeroCount = 0u;
    BUFFER_UPLOAD(
        &zeroCount,
        sizeof(uint32_t),
        org::runtime::UploadTarget::FromShared(m_tileCountBuffer),
        0);
}

PassReturn AVBOITEarlyDepthBuildPass::Execute(PassExecutionContext& executionContext)
{
    if (!m_configBuffer || !m_zeroTransmittanceSliceTexture || !m_tileCommandsBuffer || !m_tileCountBuffer) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_CONFIG_DESCRIPTOR_INDEX] = m_configBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_ZERO_SLICE_DESCRIPTOR_INDEX] = m_zeroTransmittanceSliceTexture->GetSRVInfo(0).slot.index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMANDS_DESCRIPTOR_INDEX] = m_tileCommandsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMAND_COUNT_DESCRIPTOR_INDEX] = m_tileCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        misc);

    const uint32_t groupCountX = (m_zeroTransmittanceSliceTexture->GetWidth() + 7u) / 8u;
    const uint32_t groupCountY = (m_zeroTransmittanceSliceTexture->GetHeight() + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    commandList.Dispatch(groupCountX, groupCountY, 1u);
    return {};
}

void AVBOITEarlyDepthBuildPass::Cleanup()
{
}