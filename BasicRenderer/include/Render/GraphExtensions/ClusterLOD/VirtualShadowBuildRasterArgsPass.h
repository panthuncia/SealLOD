#pragma once

#include <memory>
#include <vector>

#include "BuiltinResources.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "RenderPasses/Base/ComputePass.h"
#include "../../../../shaders/PerPassRootConstants/clodVirtualShadowBuildArgsRootConstants.h"

namespace org { class Buffer; }
using org::Buffer;

class VirtualShadowBuildRasterArgsPass : public ComputePass {
public:
    VirtualShadowBuildRasterArgsPass(
        std::shared_ptr<Buffer> histogramBuffer,
        std::shared_ptr<Buffer> offsetsBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
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

    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_histogramBuffer, m_offsetsBuffer)
            .WithUnorderedAccess(m_indirectArgsBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {}

    void Update(const UpdateExecutionContext& executionContext) override
    {
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return;
        }

        auto* updateContext = executionContext.hostData->Get<UpdateContext>();
        auto& context = *updateContext;
        const uint32_t numBuckets = context.materialManager->GetRasterBucketCount();
        if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(RasterizeClustersCommand)) {
            m_indirectArgsBuffer->ResizeStructured(numBuckets);
        }
    }

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return {};
        }

        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;
        const uint32_t numBuckets = context.materialManager->GetRasterBucketCount();
        if (numBuckets == 0u) {
            return {};
        }

        commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

        uint32_t misc[NumMiscUintRootConstants] = {};
        misc[CLOD_VSM_BUILD_ARGS_HISTOGRAM_DESCRIPTOR_INDEX] = m_histogramBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_VSM_BUILD_ARGS_OFFSETS_DESCRIPTOR_INDEX] = m_offsetsBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_VSM_BUILD_ARGS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = m_indirectArgsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        misc[CLOD_VSM_BUILD_ARGS_NUM_BUCKETS] = numBuckets;
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);
        commandList.Dispatch((numBuckets + 63u) / 64u, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_histogramBuffer;
    std::shared_ptr<Buffer> m_offsetsBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
