#pragma once

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodRayTracingSystem.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

class RayTracedReflectionsPass : public ComputePass {
public:
    RayTracedReflectionsPass() {
        m_setupPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/rayTracingSetup.hlsl",
            L"CLodRayTracingSetupCSMain",
            {},
            "CLod.RayTracing.Setup.PSO");
        m_tlasSetupPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/rayTracingTlasSetup.hlsl",
            L"CLodRayTracingTlasSetupCSMain",
            {},
            "CLod.RayTracing.TLASSetup.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(
            Builtin::Color::HDRColorTarget,
            Builtin::PrimaryCamera::DepthTexture,
            Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo,
            Builtin::CameraBuffer,
            Builtin::Environment::CurrentPrefilteredCubemap);
        builder->WithUnorderedAccess(Builtin::PostProcessing::ScreenSpaceReflections);

        ResourceState outState{
            .access = rhi::ResourceAccessType::Common,
            .layout = rhi::ResourceLayout::Common,
            .sync = rhi::ResourceSyncState::All,
        };

        builder->WithInternalTransition(
            ResourceIdentifierAndRange(Builtin::PostProcessing::ScreenSpaceReflections, {}),
            outState);
    }

    void Setup() override {
        m_output = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::ScreenSpaceReflections);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        if (!renderContext || !m_output) {
            return {};
        }

        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(
            renderContext->textureDescriptorHeap.GetHandle(),
            renderContext->samplerDescriptorHeap.GetHandle());

        bool traceSubmitted = false;
        if (renderContext->clodRayTracingSystem
            && renderContext->clodRayTracingSystem->HasGpuClasBuildInputs()) {
            auto* clodRt = renderContext->clodRayTracingSystem;
            auto pageSources = clodRt->GetPageSourceBuffer();
            auto buildInfos = clodRt->GetClasBuildInfoBuffer();
            if (pageSources && buildInfos) {
                pageSources->Materialize();
                buildInfos->Materialize();

                commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
                commandList.BindPipeline(m_setupPso.GetAPIPipelineState().GetHandle());
                BindResourceDescriptorIndices(commandList, m_setupPso.GetResourceDescriptorSlots());

                const auto& stats = clodRt->GetStats();
                uint32_t rootConstants[NumMiscUintRootConstants] = {};
                rootConstants[CLOD_RT_SETUP_PAGE_SOURCES_DESCRIPTOR_INDEX] = pageSources->GetSRVInfo(0).slot.index;
                rootConstants[CLOD_RT_SETUP_BUILD_INFOS_DESCRIPTOR_INDEX] = buildInfos->GetUAVShaderVisibleInfo(0).slot.index;
                rootConstants[CLOD_RT_SETUP_PAGE_SOURCE_COUNT] = clodRt->GetGpuPageSourceCount();
                rootConstants[CLOD_RT_SETUP_BUILD_CLUSTER_CAPACITY] = stats.buildableClusters;
                commandList.PushConstants(
                    rhi::ShaderStage::Compute,
                    0,
                    MiscUintRootSignatureIndex,
                    0,
                    NumMiscUintRootConstants,
                    rootConstants);

                commandList.Dispatch((rootConstants[CLOD_RT_SETUP_PAGE_SOURCE_COUNT] + 63u) / 64u, 1u, 1u);

                rhi::BufferBarrier buildInfoBarrier{};
                buildInfoBarrier.buffer = buildInfos->GetAPIResource().GetHandle();
                buildInfoBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
                buildInfoBarrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
                buildInfoBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
                buildInfoBarrier.afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;

                rhi::BarrierBatch buildInfoBarrierBatch{};
                buildInfoBarrierBatch.buffers = { &buildInfoBarrier };
                commandList.Barriers(buildInfoBarrierBatch);

                clodRt->ExecuteClasBuild(commandList);

                auto clasData = clodRt->GetClasDataBuffer();
                auto clasAddresses = clodRt->GetClasAddressBuffer();
                if (clasData && clasAddresses && clodRt->HasGpuBlasBuildInputs()) {
                    rhi::BufferBarrier clasBarriers[2]{};
                    clasBarriers[0].buffer = clasData->GetAPIResource().GetHandle();
                    clasBarriers[0].beforeAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureWrite;
                    clasBarriers[0].afterAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureRead;
                    clasBarriers[0].beforeSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
                    clasBarriers[0].afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;

                    clasBarriers[1].buffer = clasAddresses->GetAPIResource().GetHandle();
                    clasBarriers[1].beforeAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureWrite;
                    clasBarriers[1].afterAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureRead;
                    clasBarriers[1].beforeSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
                    clasBarriers[1].afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;

                    rhi::BarrierBatch clasBarrierBatch{};
                    clasBarrierBatch.buffers = { clasBarriers, 2u };
                    commandList.Barriers(clasBarrierBatch);

                    clodRt->ExecuteBlasBuild(commandList);

                    auto blasData = clodRt->GetBlasDataBuffer();
                    auto blasAddresses = clodRt->GetBlasAddressBuffer();
                    auto tlasInstances = clodRt->GetTlasInstanceBuffer();
                    if (blasData && blasAddresses && tlasInstances && clodRt->HasGpuTlasBuildInputs()) {
                        rhi::BufferBarrier blasBarriers[2]{};
                        blasBarriers[0].buffer = blasData->GetAPIResource().GetHandle();
                        blasBarriers[0].beforeAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureWrite;
                        blasBarriers[0].afterAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureRead;
                        blasBarriers[0].beforeSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
                        blasBarriers[0].afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;

                        blasBarriers[1].buffer = blasAddresses->GetAPIResource().GetHandle();
                        blasBarriers[1].beforeAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureWrite;
                        blasBarriers[1].afterAccess = rhi::ResourceAccessType::ShaderResource;
                        blasBarriers[1].beforeSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
                        blasBarriers[1].afterSync = rhi::ResourceSyncState::ComputeShading;

                        rhi::BarrierBatch blasBarrierBatch{};
                        blasBarrierBatch.buffers = { blasBarriers, 2u };
                        commandList.Barriers(blasBarrierBatch);

                        tlasInstances->Materialize();

                        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
                        commandList.BindPipeline(m_tlasSetupPso.GetAPIPipelineState().GetHandle());
                        BindResourceDescriptorIndices(commandList, m_tlasSetupPso.GetResourceDescriptorSlots());

                        uint32_t tlasRootConstants[NumMiscUintRootConstants] = {};
                        tlasRootConstants[CLOD_RT_SETUP_BLAS_ADDRESSES_DESCRIPTOR_INDEX] = blasAddresses->GetSRVInfo(0).slot.index;
                        tlasRootConstants[CLOD_RT_SETUP_TLAS_INSTANCES_DESCRIPTOR_INDEX] = tlasInstances->GetUAVShaderVisibleInfo(0).slot.index;
                        commandList.PushConstants(
                            rhi::ShaderStage::Compute,
                            0,
                            MiscUintRootSignatureIndex,
                            0,
                            NumMiscUintRootConstants,
                            tlasRootConstants);
                        commandList.Dispatch(1u, 1u, 1u);

                        rhi::BufferBarrier tlasInstanceBarrier{};
                        tlasInstanceBarrier.buffer = tlasInstances->GetAPIResource().GetHandle();
                        tlasInstanceBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
                        tlasInstanceBarrier.afterAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureRead;
                        tlasInstanceBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
                        tlasInstanceBarrier.afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;

                        rhi::BarrierBatch tlasInstanceBarrierBatch{};
                        tlasInstanceBarrierBatch.buffers = { &tlasInstanceBarrier };
                        commandList.Barriers(tlasInstanceBarrierBatch);

                        clodRt->ExecuteTlasBuild(commandList);

                        clodRt->EnsureRayTracingPipeline(
                            DeviceManager::GetInstance().GetDevice(),
                            DeviceManager::GetInstance().GetRayTracingFeatures());
                        if (clodRt->HasRayTracingPipeline()) {
                            clodRt->ExecuteTraceRays(
                                DeviceManager::GetInstance().GetDevice(),
                                commandList,
                                *m_output,
                                m_output->GetWidth(),
                                m_output->GetHeight());
                            traceSubmitted = clodRt->GetStats().traceRaysSubmitted;
                        }
                    }
                }
            }
        }

        if (traceSubmitted) {
            return {};
        }

        rhi::UavClearInfo clearInfo{};
        clearInfo.cpuVisible = m_output->GetUAVNonShaderVisibleInfo(0).slot;
        clearInfo.shaderVisible = m_output->GetUAVShaderVisibleInfo(0).slot;
        clearInfo.resource = m_output->GetAPIResource();

        rhi::UavClearFloat clearValue{};
        clearValue.v[0] = 0.0f;
        clearValue.v[1] = 0.0f;
        clearValue.v[2] = 0.0f;
        clearValue.v[3] = 0.0f;
        commandList.ClearUavFloat(clearInfo, clearValue);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_setupPso;
    PipelineState m_tlasSetupPso;
    PixelBuffer* m_output = nullptr;
};
