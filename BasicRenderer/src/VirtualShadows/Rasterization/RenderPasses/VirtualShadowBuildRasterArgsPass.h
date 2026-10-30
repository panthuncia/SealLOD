#pragma once

#include <memory>
#include <vector>

#include "BuiltinResources.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildArgsRootConstants.h"

namespace org { class Buffer; }

struct VirtualShadowBuildRasterArgsBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken histogram, offsets, arguments;
};
struct VirtualShadowBuildRasterArgsFrame {
    uint32_t numBuckets = 0;
};

class VirtualShadowBuildRasterArgsPass : public org::TypedRenderGraphPass<VirtualShadowBuildRasterArgsPass,
    VirtualShadowBuildRasterArgsFrame, VirtualShadowBuildRasterArgsBindings> {
public:
    VirtualShadowBuildRasterArgsPass(
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false)
        : m_histogramBuffer(std::move(histogramBuffer))
        , m_offsetsBuffer(std::move(offsetsBuffer))
        , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
        , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly)
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/virtualShadowBlockExpansion.hlsl",
            L"CLodVirtualShadowBuildRasterArgsCSMain",
            {},
            "CLod_VirtualShadowBuildRasterArgsPSO");
    }

    VirtualShadowBuildRasterArgsBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        declaration.ConstantBuffer(Builtin::PerFrameBuffer);
        return {m_pso, declaration.ShaderResource(m_histogramBuffer),
            declaration.ShaderResource(m_offsetsBuffer),
            declaration.UnorderedAccess(m_indirectArgsBuffer)};
    }

    void Update(const org::UpdateExecutionContext& executionContext) override
    {
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return;
        }

        auto* updateContext = executionContext.hostData->Get<UpdateContext>();
        auto& context = *updateContext;
        const uint32_t numBuckets = context.preparedRasterBucketCount;
        if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(RasterizeClustersCommand)) {
            m_indirectArgsBuffer->ResizeStructured(numBuckets);
        }
    }

    VirtualShadowBuildRasterArgsFrame Prepare(const VirtualShadowBuildRasterArgsBindings&,
        const org::PassPrepareContext& preparation) const {

        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return {};
        }

        return {preparation.preparationData->Get<UpdateContext>()->preparedRasterBucketCount};
    }

    // Zero buckets (or a skipped frame) dispatches nothing.
    static void Record(const VirtualShadowBuildRasterArgsBindings& bindings,
        const VirtualShadowBuildRasterArgsFrame& frame, org::PassRecordContext& recording) {
        br::render::MiscConstants constants{};
        constants[CLOD_VSM_BUILD_ARGS_HISTOGRAM_DESCRIPTOR_INDEX] = recording.Index(bindings.histogram);
        constants[CLOD_VSM_BUILD_ARGS_OFFSETS_DESCRIPTOR_INDEX] = recording.Index(bindings.offsets);
        constants[CLOD_VSM_BUILD_ARGS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = recording.Index(bindings.arguments);
        constants[CLOD_VSM_BUILD_ARGS_NUM_BUCKETS] = frame.numBuckets;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (frame.numBuckets + 63u) / 64u);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
