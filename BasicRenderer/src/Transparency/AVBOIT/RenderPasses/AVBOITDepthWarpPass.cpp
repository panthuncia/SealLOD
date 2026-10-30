#include "Transparency/AVBOIT/RenderPasses/AVBOITDepthWarpPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITDepthWarpRootConstants.h"

AVBOITDepthWarpPass::AVBOITDepthWarpPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
    std::shared_ptr<org::Buffer> depthWarpLUTBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_occupancyHistogramBuffer(std::move(occupancyHistogramBuffer))
    , m_depthWarpLUTBuffer(std::move(depthWarpLUTBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITDepthWarp.hlsl",
        L"CLodAVBOITDepthWarpCS",
        {},
        "CLod.AVBOITDepthWarp.PSO");
}

AVBOITDepthWarpBindings AVBOITDepthWarpPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, builder.ShaderResource(m_configBuffer), builder.ShaderResource(m_occupancyHistogramBuffer), builder.UnorderedAccess(m_depthWarpLUTBuffer)};
}

AVBOITDepthWarpFrame AVBOITDepthWarpPass::Prepare(const AVBOITDepthWarpBindings&, const org::PassPrepareContext&) const {
    if (!m_configBuffer || !m_occupancyHistogramBuffer || !m_depthWarpLUTBuffer) {
        return {};
    }

    const uint32_t groupCountX =
        (CLodAVBOITDepthWarpLUTResolution + 63u) / 64u;
    return {groupCountX, 1u};
}

void AVBOITDepthWarpPass::Record(const AVBOITDepthWarpBindings& bindings, const AVBOITDepthWarpFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_DEPTH_WARP_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_DEPTH_WARP_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    constants[CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_DESCRIPTOR_INDEX] = context.Index(bindings.lut);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
