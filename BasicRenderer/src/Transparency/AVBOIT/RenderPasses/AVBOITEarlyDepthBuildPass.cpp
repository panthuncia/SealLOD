#include "Transparency/AVBOIT/RenderPasses/AVBOITEarlyDepthBuildPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITEarlyDepthBuildRootConstants.h"

AVBOITEarlyDepthBuildPass::AVBOITEarlyDepthBuildPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture,
    std::shared_ptr<org::Buffer> tileCommandsBuffer,
    std::shared_ptr<org::Buffer> tileCountBuffer)
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

AVBOITEarlyDepthBuildBindings AVBOITEarlyDepthBuildPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, builder.ShaderResource(m_configBuffer), builder.ShaderResource(m_zeroTransmittanceSliceTexture), builder.UnorderedAccess(m_tileCommandsBuffer), builder.UnorderedAccess(m_tileCountBuffer)};
}

void AVBOITEarlyDepthBuildPass::Update(const org::UpdateExecutionContext& executionContext)
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
    UploadBufferData(
        &zeroCount,
        sizeof(uint32_t),
        org::runtime::UploadTarget::FromShared(m_tileCountBuffer),
        0);
}

AVBOITEarlyDepthBuildFrame AVBOITEarlyDepthBuildPass::Prepare(const AVBOITEarlyDepthBuildBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (!m_configBuffer || !m_zeroTransmittanceSliceTexture || !m_tileCommandsBuffer || !m_tileCountBuffer) {
        return {};
    }

    const auto& zeroSlice = preparation.Describe(bindings.zeroSlice);
    const uint32_t groupCountX = (zeroSlice.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (zeroSlice.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    return {groupCountX, groupCountY};
}

void AVBOITEarlyDepthBuildPass::Record(const AVBOITEarlyDepthBuildBindings& bindings, const AVBOITEarlyDepthBuildFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_ZERO_SLICE_DESCRIPTOR_INDEX] = context.Index(bindings.zeroSlice);
    constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMANDS_DESCRIPTOR_INDEX] = context.Index(bindings.commands);
    constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMAND_COUNT_DESCRIPTOR_INDEX] = context.Index(bindings.count);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
