#pragma once

#include <array>
#include <memory>

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

struct ClusterSoftwareRasterPageJobBuildArgsBindings {
    std::array<org::ResourceBindingToken, 2> counts, arguments;
};

class ClusterSoftwareRasterPageJobBuildArgsPass : public org::TypedRenderGraphPass<ClusterSoftwareRasterPageJobBuildArgsPass,
    br::render::PreparedComputeDispatchSequence, ClusterSoftwareRasterPageJobBuildArgsBindings> {
public:
    ClusterSoftwareRasterPageJobBuildArgsPass(
        std::shared_ptr<org::Buffer> rigidPageJobCountBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobCountBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobIndirectArgsBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false)
        : m_pageJobCountBuffers{ std::move(rigidPageJobCountBuffer), std::move(skinnedPageJobCountBuffer) }
        , m_pageJobIndirectArgsBuffers{ std::move(rigidPageJobIndirectArgsBuffer), std::move(skinnedPageJobIndirectArgsBuffer) }
        , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly)
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobBuildIndirectArgsCSMain",
            {},
            "CLod_SoftwarePageJobBuildIndirectArgsPSO");
    }

    ClusterSoftwareRasterPageJobBuildArgsBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        return {{declaration.BindShaderResource(m_pageJobCountBuffers[0]),
                    declaration.BindShaderResource(m_pageJobCountBuffers[1])},
            {declaration.BindUnorderedAccess(m_pageJobIndirectArgsBuffers[0]),
                declaration.BindUnorderedAccess(m_pageJobIndirectArgsBuffers[1])}};
    }

    br::render::PreparedComputeDispatchSequence Prepare(
        const ClusterSoftwareRasterPageJobBuildArgsBindings& bindings,
        const org::PassPrepareContext& preparation) const {

        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return {};
        }
        if (!CLodVSMRasterModeUsesLargeClusterPageJob(
                SettingsManager::GetInstance().getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)())) {
            return {};
        }

        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatchSequence data{};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        for (uint32_t variantIndex = 0; variantIndex < m_pageJobCountBuffers.size(); ++variantIndex) {
            br::render::PreparedComputeDispatchSequence::Step step{};
            step.constants[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX] = preparation.ResolveView(
                bindings.counts[variantIndex], {org::BindlessViewKind::ShaderResource}).index;
            step.constants[CLOD_RASTER_PAGE_JOB_INDIRECT_ARGS_DESCRIPTOR_INDEX] = preparation.ResolveView(
                bindings.arguments[variantIndex], {org::BindlessViewKind::UnorderedAccess}).index;
            step.groupsX = 1;
            data.steps.push_back(std::move(step));
        }
        return data;
    }

    static void Record(const ClusterSoftwareRasterPageJobBuildArgsBindings&,
        const br::render::PreparedComputeDispatchSequence& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatchSequence(data, recording);
    }

private:
    org::PipelineState m_pso;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobCountBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobIndirectArgsBuffers;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
