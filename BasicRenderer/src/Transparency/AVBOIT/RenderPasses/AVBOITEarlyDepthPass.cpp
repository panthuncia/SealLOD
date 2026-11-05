#include "Transparency/AVBOIT/RenderPasses/AVBOITEarlyDepthPass.h"
#include "Render/PassBuilders.h"

#include "BuiltinResources.h"
#include "Runtime/Device/DeviceManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
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
}

AVBOITEarlyDepthBindings AVBOITEarlyDepthPass::Declare(org::PassBuilder& builder)
{
    builder.ShaderResource(Builtin::CameraBuffer);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    org::GraphicsPipelineDesc pipeline;
    pipeline.name = "CLod.AVBOITEarlyDepth.PSO";
    pipeline.vertex = org::PipelineShaderSource{ L"shaders/ClusterLOD/AVBOITEarlyDepth.hlsl", L"AVBOITEarlyDepthTileVSMain", L"vs_6_6" };
    pipeline.pixel = org::PipelineShaderSource{ L"shaders/ClusterLOD/AVBOITEarlyDepth.hlsl", L"AVBOITEarlyDepthPSMain", L"ps_6_6" };

    rhi::RasterState rasterState{};
    rasterState.fill = rhi::FillMode::Solid;
    rasterState.cull = rhi::CullMode::None;
    rasterState.frontCCW = false;
    pipeline.raster = rasterState;

    rhi::BlendState blendState{};
    blendState.alphaToCoverage = false;
    blendState.independentBlend = false;
    blendState.numAttachments = 0;
    pipeline.blend = blendState;

    rhi::DepthStencilState depthState{};
    depthState.depthEnable = true;
    depthState.depthWrite = true;
    depthState.depthFunc = rhi::CompareOp::Always;
    pipeline.depth = depthState;

    rhi::RenderTargets renderTargets{};
    renderTargets.count = 0;
    pipeline.renderTargets = renderTargets;
    pipeline.depthFormat = rhi::Format::D32_Float;
    pipeline.sample = rhi::SampleDesc{ 1, 0 };
    pipeline.topology = rhi::PrimitiveTopology::TriangleStrip;

    org::CommandSignatureDesc signature;
    signature.args = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::Draw }
    };
    signature.byteStride = sizeof(CLodAVBOITEarlyDepthTileIndirectCommand);
    signature.layout = org::PipelineLayoutRef::StandardGraphics();
    signature.name = "CLod.AVBOITEarlyDepth.Signature";

    return {
        builder.Pipeline(std::move(pipeline)),
        builder.CommandSignature(std::move(signature)),
        builder.ShaderResource(m_configBuffer),
        builder.IndirectArguments(m_tileCommandsBuffer),
        builder.IndirectArguments(m_tileCountBuffer),
        builder.DepthReadWrite(m_earlyDepthTexture)
    };
}

AVBOITEarlyDepthFrameData AVBOITEarlyDepthPass::Prepare(
    const AVBOITEarlyDepthBindings& bindings, const org::PassPrepareContext& preparation) const {
    AVBOITEarlyDepthFrameData data;
    data.clear = preparation.ClearValue(bindings.depth.Resource());
    const auto& depthDesc = preparation.Describe(bindings.depth);
    data.width = depthDesc.texture.width; data.height = depthDesc.texture.height;
    data.maximumCount = static_cast<uint32_t>(preparation.Describe(bindings.arguments).buffer.sizeBytes
        / sizeof(CLodAVBOITEarlyDepthTileIndirectCommand));
    return data;
}

void AVBOITEarlyDepthPass::Record(const AVBOITEarlyDepthBindings& bindings,
    const AVBOITEarlyDepthFrameData& data, org::PassRecordContext& recording) {
    auto& commands = recording.Commands();
    rhi::DepthAttachment depth{};
    depth.dsv = recording.View(bindings.depth);
    depth.depthLoad = rhi::LoadOp::Clear; depth.depthStore = rhi::StoreOp::Store;
    depth.stencilLoad = rhi::LoadOp::DontCare; depth.stencilStore = rhi::StoreOp::DontCare;
    depth.clear = data.clear;
    rhi::PassBeginInfo pass{};
    pass.depth = &depth; pass.width = data.width; pass.height = data.height;
    pass.debugName = "CLod AVBOIT early depth";
    commands.BeginPass(pass);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);
    recording.BindGraphicsPipeline(bindings.pipeline);
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_EARLY_DEPTH_CONFIG_DESCRIPTOR_INDEX] = recording.Index(bindings.config);
    br::render::PushMiscConstants(recording, constants);
    recording.ExecuteIndirect(bindings.signature, bindings.arguments, 0, data.maximumCount, bindings.count, 0);
    commands.EndPass();
}
