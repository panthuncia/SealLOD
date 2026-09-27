#include "Transparency/AVBOIT/RenderPasses/AVBOITEarlyDepthBuildPass.h"

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
    return {builder.BindShaderResource(m_configBuffer), builder.BindShaderResource(m_zeroTransmittanceSliceTexture), builder.BindUnorderedAccess(m_tileCommandsBuffer), builder.BindUnorderedAccess(m_tileCountBuffer)};
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

br::render::PreparedComputeDispatch AVBOITEarlyDepthBuildPass::Prepare(const AVBOITEarlyDepthBuildBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_zeroTransmittanceSliceTexture || !m_tileCommandsBuffer || !m_tileCountBuffer) {
        return {};
    }

    const auto* renderContext = preparation.preparationData->Get<UpdateContext>();
    auto& context = *renderContext;

    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);

    auto& misc = data.constants;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_CONFIG_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_ZERO_SLICE_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.zeroSlice, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMANDS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.commands, {org::BindlessViewKind::UnorderedAccess}).index;
    misc[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_BUILD_COMMAND_COUNT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.count, {org::BindlessViewKind::UnorderedAccess}).index;

    const auto& zeroSlice = preparation.Describe(bindings.zeroSlice);
    const uint32_t groupCountX = (zeroSlice.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (zeroSlice.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    data.groupsX = groupCountX; data.groupsY = groupCountY; data.groupsZ = 1u;
    return data;
}

void AVBOITEarlyDepthBuildPass::Record(const AVBOITEarlyDepthBuildBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
