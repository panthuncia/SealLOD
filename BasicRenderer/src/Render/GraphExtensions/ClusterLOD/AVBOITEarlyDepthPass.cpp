#include "Render/GraphExtensions/ClusterLOD/AVBOITEarlyDepthPass.h"

#include "BuiltinResources.h"
#include "Render/ShaderAPI.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITEarlyDepthRootConstants.h"

AVBOITEarlyDepthPass::AVBOITEarlyDepthPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::Buffer> tileCommandsBuffer,
    std::shared_ptr<org::Buffer> tileCountBuffer,
    std::shared_ptr<org::PixelBuffer> earlyDepthTexture)
    : m_configBuffer(std::move(configBuffer))
    , m_tileCommandsBuffer(std::move(tileCommandsBuffer))
    , m_tileCountBuffer(std::move(tileCountBuffer))
    , m_earlyDepthTexture(std::move(earlyDepthTexture))
{
    auto dev = DeviceManager::GetInstance().GetDevice();

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/ClusterLOD/AVBOITEarlyDepth.hlsl", L"AVBOITEarlyDepthTileVSMain", L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/AVBOITEarlyDepth.hlsl", L"AVBOITEarlyDepthPSMain", L"ps_6_6" };
    auto compiledBundle = PSOManager::GetInstance().CompileShaders(shaderInfoBundle);

    auto& layout = PSOManager::GetInstance().GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiledBundle.vertexShader.Get()), "AVBOITEarlyDepthTileVSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiledBundle.pixelShader.Get()), "AVBOITEarlyDepthPSMain" };

    rhi::RasterState rasterState{};
    rasterState.fill = rhi::FillMode::Solid;
    rasterState.cull = rhi::CullMode::None;
    rasterState.frontCCW = false;
    rhi::SubobjRaster soRaster{ rasterState };

    rhi::BlendState blendState{};
    blendState.alphaToCoverage = false;
    blendState.independentBlend = false;
    blendState.numAttachments = 0;
    rhi::SubobjBlend soBlend{ blendState };

    rhi::DepthStencilState depthState{};
    depthState.depthEnable = true;
    depthState.depthWrite = true;
    depthState.depthFunc = rhi::CompareOp::Always;
    rhi::SubobjDepth soDepth{ depthState };

    rhi::RenderTargets renderTargets{};
    renderTargets.count = 0;
    rhi::SubobjRTVs soRTV{ renderTargets };
    rhi::SubobjDSV soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSample{ rhi::SampleDesc{ 1, 0 } };
    rhi::SubobjPrimitiveTopology soTopology{ rhi::PrimitiveTopology::TriangleStrip };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSample),
        rhi::Make(soTopology),
    };

    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod AVBOIT early depth PSO");
    }

    pso->SetName("CLod.AVBOITEarlyDepth.PSO");
    m_pso = org::PipelineState(std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots,
        PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout), soLayout.layout);

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::Draw }
    };
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    result = dev.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(CLodAVBOITEarlyDepthTileIndirectCommand) },
        layout.GetHandle(),
        *m_commandSignature);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod AVBOIT early depth command signature");
    }
}

AVBOITEarlyDepthBindings AVBOITEarlyDepthPass::Declare(org::PassBuilder& builder)
{
    builder.WithShaderResource(Builtin::CameraBuffer);
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {
        builder.BindShaderResource(m_configBuffer),
        builder.BindIndirectArguments(m_tileCommandsBuffer),
        builder.BindIndirectArguments(m_tileCountBuffer),
        builder.BindDepthReadWrite(m_earlyDepthTexture)
    };
}

AVBOITEarlyDepthFrameData AVBOITEarlyDepthPass::Prepare(
    const AVBOITEarlyDepthBindings& bindings, const org::PassPrepareContext& preparation) const {
    AVBOITEarlyDepthFrameData data;
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    data.enabled = true;
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.signature = preparation.CaptureCommandSignature(m_commandSignature);
    data.arguments = preparation.CaptureResource(bindings.arguments);
    data.count = preparation.CaptureResource(bindings.count);
    data.depth = preparation.CaptureView(bindings.depth, {org::BindlessViewKind::DepthStencil});
    data.clear = preparation.ClearValue(bindings.depth);
    const auto& depthDesc = preparation.Describe(bindings.depth);
    data.width = depthDesc.texture.width; data.height = depthDesc.texture.height;
    data.maximumCount = static_cast<uint32_t>(preparation.Describe(bindings.arguments).buffer.sizeBytes
        / sizeof(CLodAVBOITEarlyDepthTileIndirectCommand));
    data.constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_CONFIG_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;
    return data;
}

void AVBOITEarlyDepthPass::Record(const AVBOITEarlyDepthBindings&,
    const AVBOITEarlyDepthFrameData& data, org::PassRecordContext& recording) {
    if (!data.enabled) return;
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    rhi::DepthAttachment depth{};
    depth.dsv = recording.Resolve(data.depth);
    depth.depthLoad = rhi::LoadOp::Clear; depth.depthStore = rhi::StoreOp::Store;
    depth.stencilLoad = rhi::LoadOp::DontCare; depth.stencilStore = rhi::StoreOp::DontCare;
    depth.clear = data.clear;
    rhi::PassBeginInfo pass{};
    pass.depth = &depth; pass.width = data.width; pass.height = data.height;
    pass.debugName = "CLod AVBOIT early depth";
    commands.BeginPass(pass);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);
    commands.BindLayout(recording.ResolveLayout(data.program));
    commands.BindPipeline(recording.Resolve(data.program));
    if (!data.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::AllGraphics, 0,
        org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
        static_cast<uint32_t>(data.descriptorIndices.size()), data.descriptorIndices.data());
    commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, data.constants.data());
    commands.ExecuteIndirect(data.signature, recording.Resolve(data.arguments).GetHandle(), 0,
        recording.Resolve(data.count).GetHandle(), 0, data.maximumCount);
    commands.EndPass();
}
