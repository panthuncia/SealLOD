#pragma once

#include <memory>
#include <vector>

#include "BuiltinResources.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "../../../../shaders/PerPassRootConstants/clodVirtualShadowBuildArgsRootConstants.h"

namespace org { class Buffer; }

struct VirtualShadowBuildRasterArgsBindings {
    org::ResourceBindingToken histogram, offsets, arguments;
};

class VirtualShadowBuildRasterArgsPass : public org::TypedRenderGraphPass<VirtualShadowBuildRasterArgsPass,
    br::render::PreparedComputeDispatch, VirtualShadowBuildRasterArgsBindings> {
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
        declaration.WithConstantBuffer(Builtin::PerFrameBuffer);
        return {declaration.BindShaderResource(m_histogramBuffer),
            declaration.BindShaderResource(m_offsetsBuffer),
            declaration.BindUnorderedAccess(m_indirectArgsBuffer)};
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

    br::render::PreparedComputeDispatch Prepare(const VirtualShadowBuildRasterArgsBindings& bindings,
        const org::PassPrepareContext& preparation) const {

        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return {};
        }

        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        const uint32_t numBuckets = context.preparedRasterBucketCount;
        if (numBuckets == 0u) return {};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[CLOD_VSM_BUILD_ARGS_HISTOGRAM_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.histogram, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[CLOD_VSM_BUILD_ARGS_OFFSETS_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.offsets, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[CLOD_VSM_BUILD_ARGS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.arguments, {org::BindlessViewKind::UnorderedAccess}).index;
        data.constants[CLOD_VSM_BUILD_ARGS_NUM_BUCKETS] = numBuckets;
        data.groupsX = (numBuckets + 63u) / 64u;
        return data;
    }

    static void Record(const VirtualShadowBuildRasterArgsBindings&,
        const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
