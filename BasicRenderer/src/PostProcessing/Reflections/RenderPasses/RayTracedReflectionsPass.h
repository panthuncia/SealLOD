#pragma once

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

struct RayTracedReflectionsFrameData {
    std::shared_ptr<br::render::CLodRayTracingSystem> service;
    // The ray-tracing service records through the device it was built on.
    rhi::Device device;
    std::shared_ptr<org::PixelBuffer> output;
    std::shared_ptr<org::Buffer> pageSources, buildInfos, clasData, clasAddresses;
    std::shared_ptr<org::Buffer> blasData, blasAddresses, tlasInstances;
    uint32_t pageSourceCount = 0, buildClusterCapacity = 0;
};

struct RayTracedReflectionsBindings {
    org::PipelineState setupPipeline, tlasSetupPipeline;
    org::DeclaredViewToken outputShader, outputCpu, pageSources, buildInfos;
    org::DeclaredViewToken blasAddressesSrv, tlasInstances;
    bool hasServiceResources = false;
};

class RayTracedReflectionsPass
    : public org::TypedRenderGraphPass<RayTracedReflectionsPass,
          RayTracedReflectionsFrameData, RayTracedReflectionsBindings>,
      public org::IDynamicDeclaredResources {
public:
    RayTracedReflectionsPass() {
        auto& manager = PSOManager::GetInstance();
        m_setupPso = manager.MakeComputePipeline(manager.GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/rayTracingSetup.hlsl", L"CLodRayTracingSetupCSMain", {},
            "CLod.RayTracing.Setup.PSO");
        m_tlasSetupPso = manager.MakeComputePipeline(manager.GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/rayTracingTlasSetup.hlsl", L"CLodRayTracingTlasSetupCSMain", {},
            "CLod.RayTracing.TLASSetup.PSO");
    }

    RayTracedReflectionsBindings Declare(org::PassBuilder& builder) {
        builder.ShaderResource(Builtin::Color::HDRColorTarget,
            Builtin::PrimaryCamera::DepthTexture, Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo, Builtin::CameraBuffer,
            Builtin::Environment::CurrentPrefilteredCubemap);
        RayTracedReflectionsBindings bindings{};
        bindings.setupPipeline = m_setupPso;
        bindings.tlasSetupPipeline = m_tlasSetupPso;
        const auto output = builder.UnorderedAccessClear(Builtin::PostProcessing::ScreenSpaceReflections);
        bindings.outputShader = output.View(0);
        bindings.outputCpu = output.View(1);
        if (m_pageSources && m_buildInfos) {
            bindings.pageSources = builder.ShaderResource(m_pageSources).View();
            bindings.buildInfos = builder.UnorderedAccess(m_buildInfos).View();
            if (m_clasData) builder.UnorderedAccess(m_clasData);
            if (m_clasAddresses) builder.UnorderedAccess(m_clasAddresses);
            if (m_blasData) builder.UnorderedAccess(m_blasData);
            if (m_blasAddresses) {
                builder.UnorderedAccess(m_blasAddresses);
                bindings.blasAddressesSrv = builder.ShaderResource(m_blasAddresses).View();
            }
            if (m_tlasInstances) bindings.tlasInstances = builder.UnorderedAccess(m_tlasInstances).View();
            bindings.hasServiceResources = true;
        }
        builder.InternalTransition(
            org::ResourceIdentifierAndRange(Builtin::PostProcessing::ScreenSpaceReflections, {}),
            org::ResourceState{.access = rhi::ResourceAccessType::Common,
                .layout = rhi::ResourceLayout::Common,
                .sync = rhi::ResourceSyncState::All});
        return bindings;
    }

    void Initialize() {
        m_output = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(
            Builtin::PostProcessing::ScreenSpaceReflections);
    }

    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* context = execution.hostData->Get<UpdateContext>();
        auto service = context ? context->clodRayTracingSystem : nullptr;
        std::shared_ptr<org::Buffer> pageSources, buildInfos, clasData, clasAddresses;
        std::shared_ptr<org::Buffer> blasData, blasAddresses, tlasInstances;
        if (service) {
            std::scoped_lock lock(service->FrameOperationMutex());
            pageSources = service->GetPageSourceBuffer();
            buildInfos = service->GetClasBuildInfoBuffer();
            clasData = service->GetClasDataBuffer();
            clasAddresses = service->GetClasAddressBuffer();
            blasData = service->GetBlasDataBuffer();
            blasAddresses = service->GetBlasAddressBuffer();
            tlasInstances = service->GetTlasInstanceBuffer();
        }
        m_declaredResourcesChanged = service != m_service || pageSources != m_pageSources ||
            buildInfos != m_buildInfos || clasData != m_clasData || clasAddresses != m_clasAddresses ||
            blasData != m_blasData || blasAddresses != m_blasAddresses || tlasInstances != m_tlasInstances;
        m_service = std::move(service); m_pageSources = std::move(pageSources);
        m_buildInfos = std::move(buildInfos); m_clasData = std::move(clasData);
        m_clasAddresses = std::move(clasAddresses); m_blasData = std::move(blasData);
        m_blasAddresses = std::move(blasAddresses); m_tlasInstances = std::move(tlasInstances);
    }

    bool DeclaredResourcesChanged() const override { return m_declaredResourcesChanged; }

    RayTracedReflectionsFrameData Prepare(const RayTracedReflectionsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        RayTracedReflectionsFrameData frame{};
        const auto* context = preparation.preparationData
            ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        if (!context || !m_output || !m_service || !bindings.hasServiceResources) return frame;
        auto service = m_service;
        std::scoped_lock serviceLock(service->FrameOperationMutex());
        if (!service->HasGpuClasBuildInputs()) return frame;
        service->EnsureRayTracingPipeline(DeviceManager::GetInstance().GetDevice(),
            DeviceManager::GetInstance().GetRayTracingFeatures());
        frame.service = service;
        frame.output = m_output;
        frame.pageSources = m_pageSources; frame.buildInfos = m_buildInfos;
        frame.clasData = m_clasData; frame.clasAddresses = m_clasAddresses;
        frame.blasData = m_blasData; frame.blasAddresses = m_blasAddresses;
        frame.tlasInstances = m_tlasInstances;
        frame.device = DeviceManager::GetInstance().GetDevice();
        frame.pageSourceCount = service->GetGpuPageSourceCount();
        frame.buildClusterCapacity = service->GetStats().buildableClusters;
        preparation.Retain(frame.output);
        preparation.Retain(frame.pageSources); preparation.Retain(frame.buildInfos);
        preparation.Retain(frame.clasData); preparation.Retain(frame.clasAddresses);
        preparation.Retain(frame.blasData); preparation.Retain(frame.blasAddresses);
        preparation.Retain(frame.tlasInstances);
        return frame;
    }

    static void Record(const RayTracedReflectionsBindings& bindings,
        const RayTracedReflectionsFrameData& frame,
        org::PassRecordContext& recording) {
        if (!frame.output) return;
        std::unique_lock<std::mutex> serviceLock;
        if (frame.service) serviceLock = std::unique_lock(frame.service->FrameOperationMutex());
        auto& commands = recording.Commands();
        bool traced = false;
        auto barrier = [&](const std::shared_ptr<org::Buffer>& buffer,
            rhi::ResourceAccessType before, rhi::ResourceAccessType after,
            rhi::ResourceSyncState beforeSync, rhi::ResourceSyncState afterSync) {
            if (!buffer) return;
            rhi::BufferBarrier value{};
            value.buffer = buffer->GetAPIResource().GetHandle();
            value.beforeAccess = before; value.afterAccess = after;
            value.beforeSync = beforeSync; value.afterSync = afterSync;
            rhi::BarrierBatch batch{}; batch.buffers = {&value}; commands.Barriers(batch);
        };
        if (frame.service && frame.pageSources && frame.buildInfos) {
            recording.BindComputePipeline(bindings.setupPipeline);
            br::render::MiscConstants constants{};
            constants[CLOD_RT_SETUP_PAGE_SOURCES_DESCRIPTOR_INDEX] =
                recording.Index(bindings.pageSources);
            constants[CLOD_RT_SETUP_BUILD_INFOS_DESCRIPTOR_INDEX] =
                recording.Index(bindings.buildInfos);
            constants[CLOD_RT_SETUP_PAGE_SOURCE_COUNT] = frame.pageSourceCount;
            constants[CLOD_RT_SETUP_BUILD_CLUSTER_CAPACITY] = frame.buildClusterCapacity;
            br::render::PushMiscConstants(recording, constants);
            commands.Dispatch((frame.pageSourceCount + 63u) / 64u, 1u, 1u);
            barrier(frame.buildInfos, rhi::ResourceAccessType::UnorderedAccess,
                rhi::ResourceAccessType::ShaderResource, rhi::ResourceSyncState::ComputeShading,
                rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
            frame.service->ExecuteClasBuild(commands);
            if (frame.clasData && frame.clasAddresses && frame.service->HasGpuBlasBuildInputs()) {
                barrier(frame.clasData, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
                    rhi::ResourceAccessType::RaytracingAccelerationStructureRead,
                    rhi::ResourceSyncState::BuildRaytracingAccelerationStructure,
                    rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
                barrier(frame.clasAddresses, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
                    rhi::ResourceAccessType::RaytracingAccelerationStructureRead,
                    rhi::ResourceSyncState::BuildRaytracingAccelerationStructure,
                    rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
                frame.service->ExecuteBlasBuild(commands);
                if (frame.blasData && frame.blasAddresses && frame.tlasInstances &&
                    frame.service->HasGpuTlasBuildInputs()) {
                    barrier(frame.blasData, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
                        rhi::ResourceAccessType::RaytracingAccelerationStructureRead,
                        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure,
                        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
                    barrier(frame.blasAddresses, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
                        rhi::ResourceAccessType::ShaderResource,
                        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure,
                        rhi::ResourceSyncState::ComputeShading);
                    recording.BindComputePipeline(bindings.tlasSetupPipeline);
                    br::render::MiscConstants tlas{};
                    tlas[CLOD_RT_SETUP_BLAS_ADDRESSES_DESCRIPTOR_INDEX] =
                        recording.Index(bindings.blasAddressesSrv);
                    tlas[CLOD_RT_SETUP_TLAS_INSTANCES_DESCRIPTOR_INDEX] =
                        recording.Index(bindings.tlasInstances);
                    br::render::PushMiscConstants(recording, tlas);
                    commands.Dispatch(1u, 1u, 1u);
                    barrier(frame.tlasInstances, rhi::ResourceAccessType::UnorderedAccess,
                        rhi::ResourceAccessType::RaytracingAccelerationStructureRead,
                        rhi::ResourceSyncState::ComputeShading,
                        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
                    frame.service->ExecuteTlasBuild(commands);
                    if (frame.service->HasRayTracingPipeline()) {
                        frame.service->ExecuteTraceRays(frame.device,
                            commands, *frame.output, recording.Index(bindings.outputShader),
                            frame.output->GetWidth(), frame.output->GetHeight());
                        traced = frame.service->GetStats().traceRaysSubmitted;
                    }
                }
            }
        }
        if (!traced) br::render::ClearUavFloat(recording, bindings.outputShader, bindings.outputCpu, 0.0f);
    }

private:
    org::PipelineState m_setupPso, m_tlasSetupPso;
    std::shared_ptr<org::PixelBuffer> m_output;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_service;
    std::shared_ptr<org::Buffer> m_pageSources, m_buildInfos, m_clasData, m_clasAddresses;
    std::shared_ptr<org::Buffer> m_blasData, m_blasAddresses, m_tlasInstances;
    bool m_declaredResourcesChanged = true;
};
