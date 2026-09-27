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
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

struct RayTracedReflectionsFrameData {
    std::shared_ptr<br::render::CLodRayTracingSystem> service;
    std::shared_ptr<org::PixelBuffer> output;
    std::shared_ptr<org::Buffer> pageSources, buildInfos, clasData, clasAddresses;
    std::shared_ptr<org::Buffer> blasData, blasAddresses, tlasInstances;
    org::PreparedProgramBinding setup{}, tlasSetup{};
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    uint32_t pageSourceCount = 0, buildClusterCapacity = 0;
    uint32_t pageSourcesSRV = 0, buildInfosUAV = 0;
    uint32_t blasAddressesSRV = 0, tlasInstancesUAV = 0;
    uint32_t outputUAV = 0;
    org::PreparedDescriptorReference outputCpuUAV{}, outputShaderUAV{};
};

struct RayTracedReflectionsBindings {
    org::ResourceBindingToken output, pageSources, buildInfos, clasData, clasAddresses;
    org::ResourceBindingToken blasData, blasAddresses, tlasInstances;
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
        builder.WithShaderResource(Builtin::Color::HDRColorTarget,
            Builtin::PrimaryCamera::DepthTexture, Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo, Builtin::CameraBuffer,
            Builtin::Environment::CurrentPrefilteredCubemap);
        RayTracedReflectionsBindings bindings{};
        bindings.output = builder.BindUnorderedAccess(
            Builtin::PostProcessing::ScreenSpaceReflections);
        if (m_pageSources && m_buildInfos) {
            bindings.pageSources = builder.BindShaderResource(m_pageSources);
            bindings.buildInfos = builder.BindUnorderedAccess(m_buildInfos);
            if (m_clasData) bindings.clasData = builder.BindUnorderedAccess(m_clasData);
            if (m_clasAddresses) bindings.clasAddresses = builder.BindUnorderedAccess(m_clasAddresses);
            if (m_blasData) bindings.blasData = builder.BindUnorderedAccess(m_blasData);
            if (m_blasAddresses) bindings.blasAddresses = builder.BindUnorderedAccess(m_blasAddresses);
            if (m_tlasInstances) bindings.tlasInstances = builder.BindUnorderedAccess(m_tlasInstances);
            bindings.hasServiceResources = true;
        }
        builder.WithInternalTransition(
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
        frame.setup = preparation.CaptureProgramBinding(m_setupPso);
        frame.tlasSetup = preparation.CaptureProgramBinding(m_tlasSetupPso);
        frame.resourceHeap = context->textureDescriptorHeap.GetHandle();
        frame.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        frame.pageSourceCount = service->GetGpuPageSourceCount();
        frame.buildClusterCapacity = service->GetStats().buildableClusters;
        frame.pageSourcesSRV = preparation.ResolveView(bindings.pageSources,
            {org::BindlessViewKind::ShaderResource}).index;
        frame.buildInfosUAV = preparation.ResolveView(bindings.buildInfos,
            {org::BindlessViewKind::UnorderedAccess}).index;
        if (frame.blasAddresses) frame.blasAddressesSRV = preparation.ResolveView(
            bindings.blasAddresses, {org::BindlessViewKind::ShaderResource}).index;
        if (frame.tlasInstances) frame.tlasInstancesUAV = preparation.ResolveView(
            bindings.tlasInstances, {org::BindlessViewKind::UnorderedAccess}).index;
        frame.outputCpuUAV = preparation.CaptureView(bindings.output,
            {org::BindlessViewKind::NonShaderVisibleUnorderedAccess});
        frame.outputShaderUAV = preparation.CaptureView(bindings.output,
            {org::BindlessViewKind::UnorderedAccess});
        frame.outputUAV = preparation.ResolveView(bindings.output,
            {org::BindlessViewKind::UnorderedAccess}).index;
        preparation.Retain(frame.output);
        preparation.Retain(frame.pageSources); preparation.Retain(frame.buildInfos);
        preparation.Retain(frame.clasData); preparation.Retain(frame.clasAddresses);
        preparation.Retain(frame.blasData); preparation.Retain(frame.blasAddresses);
        preparation.Retain(frame.tlasInstances);
        return frame;
    }

    static void Record(const RayTracedReflectionsBindings&,
        const RayTracedReflectionsFrameData& frame,
        org::PassRecordContext& recording) {
        if (!frame.output) return;
        std::unique_lock<std::mutex> serviceLock;
        if (frame.service) serviceLock = std::unique_lock(frame.service->FrameOperationMutex());
        auto& commands = recording.Commands();
        commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
        bool traced = false;
        auto bind = [&](const org::PreparedProgramBinding& program) {
            commands.BindLayout(recording.ResolveLayout(program.program));
            commands.BindPipeline(recording.Resolve(program.program));
            if (!program.descriptorIndices.empty()) commands.PushConstants(
                rhi::ShaderStage::Compute, 0,
                org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                static_cast<uint32_t>(program.descriptorIndices.size()),
                program.descriptorIndices.data());
        };
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
            bind(frame.setup);
            uint32_t constants[NumMiscUintRootConstants]{};
            constants[CLOD_RT_SETUP_PAGE_SOURCES_DESCRIPTOR_INDEX] =
                frame.pageSourcesSRV;
            constants[CLOD_RT_SETUP_BUILD_INFOS_DESCRIPTOR_INDEX] =
                frame.buildInfosUAV;
            constants[CLOD_RT_SETUP_PAGE_SOURCE_COUNT] = frame.pageSourceCount;
            constants[CLOD_RT_SETUP_BUILD_CLUSTER_CAPACITY] = frame.buildClusterCapacity;
            commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex,
                0, NumMiscUintRootConstants, constants);
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
                    bind(frame.tlasSetup);
                    uint32_t tlas[NumMiscUintRootConstants]{};
                    tlas[CLOD_RT_SETUP_BLAS_ADDRESSES_DESCRIPTOR_INDEX] =
                        frame.blasAddressesSRV;
                    tlas[CLOD_RT_SETUP_TLAS_INSTANCES_DESCRIPTOR_INDEX] =
                        frame.tlasInstancesUAV;
                    commands.PushConstants(rhi::ShaderStage::Compute, 0,
                        MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, tlas);
                    commands.Dispatch(1u, 1u, 1u);
                    barrier(frame.tlasInstances, rhi::ResourceAccessType::UnorderedAccess,
                        rhi::ResourceAccessType::RaytracingAccelerationStructureRead,
                        rhi::ResourceSyncState::ComputeShading,
                        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
                    frame.service->ExecuteTlasBuild(commands);
                    if (frame.service->HasRayTracingPipeline()) {
                        frame.service->ExecuteTraceRays(DeviceManager::GetInstance().GetDevice(),
                            commands, *frame.output, frame.outputUAV,
                            frame.output->GetWidth(), frame.output->GetHeight());
                        traced = frame.service->GetStats().traceRaysSubmitted;
                    }
                }
            }
        }
        if (!traced) {
            rhi::UavClearInfo clear{};
            clear.cpuVisible = recording.Resolve(frame.outputCpuUAV);
            clear.shaderVisible = recording.Resolve(frame.outputShaderUAV);
            clear.resource = frame.output->GetAPIResource();
            commands.ClearUavFloat(clear, {});
        }
    }

private:
    org::PipelineState m_setupPso, m_tlasSetupPso;
    std::shared_ptr<org::PixelBuffer> m_output;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_service;
    std::shared_ptr<org::Buffer> m_pageSources, m_buildInfos, m_clasData, m_clasAddresses;
    std::shared_ptr<org::Buffer> m_blasData, m_blasAddresses, m_tlasInstances;
    bool m_declaredResourcesChanged = true;
};
