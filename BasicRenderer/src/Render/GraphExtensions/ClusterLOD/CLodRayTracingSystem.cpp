#include "Render/GraphExtensions/ClusterLOD/CLodRayTracingSystem.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "Managers/Singletons/DescriptorHeapManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include <rhi_helpers.h>
#include <rhi_interop.h>
#include <rhi_interop_dx12.h>

namespace br::render {

namespace {

uint64_t GetNativeBufferDeviceAddress(rhi::Resource resource) noexcept
{
    if (ID3D12Resource* nativeResource = rhi::dx12::get_resource(resource)) {
        return nativeResource->GetGPUVirtualAddress();
    }

    rhi::VulkanResourceInfo vulkanInfo{};
    if (rhi::QueryNativeResource(resource, rhi::RHI_IID_VK_RESOURCE, &vulkanInfo, sizeof(vulkanInfo))) {
        return vulkanInfo.deviceAddress;
    }

    return 0u;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept
{
    if (alignment == 0u) {
        return value;
    }

    const uint64_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

void SetBufferNameAndUsage(const std::shared_ptr<Buffer>& buffer, const char* name, std::string_view usage)
{
    buffer->SetName(name);
    rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
}

} // namespace

void CLodRayTracingSystem::Refresh(const MeshManager& meshManager) {
    meshManager.GetCLodRayTracingResidencySnapshot(m_snapshot);
    m_pageSources.clear();

    Stats next{};
    next.snapshotGeneration = m_snapshot.pagePoolGeneration;
    next.hasPagePool = m_snapshot.pagePool != nullptr;
    next.residentGroups = static_cast<uint32_t>(m_snapshot.residentGroups.size());

    for (const auto& group : m_snapshot.residentGroups) {
        next.residentPages += static_cast<uint32_t>(group.pageAllocations.size());
        next.meshlets += group.chunk.meshletCount;

        const bool groupHasBuildablePages = !group.pageAllocations.empty();
        const bool groupUsesNativePositions = group.chunk.compressedPositionQuantExp == CLOD_POSITION_FORMAT_FLOAT3;
        if (groupHasBuildablePages && groupUsesNativePositions) {
            ++next.buildableGroups;
        }

        (void)group.segments;

        if (m_snapshot.pagePool && groupHasBuildablePages && groupUsesNativePositions) {
            std::vector<uint32_t> pageMeshletCounts(group.pageAllocations.size(), 0u);
            for (const ClusterLODGroupSegment& segment : group.segments) {
                const auto pageIt = std::find(group.meshPageIndices.begin(), group.meshPageIndices.end(), segment.pageIndex);
                if (pageIt == group.meshPageIndices.end()) {
                    continue;
                }
                const uint32_t groupLocalPageIndex = static_cast<uint32_t>(std::distance(group.meshPageIndices.begin(), pageIt));
                pageMeshletCounts[groupLocalPageIndex] = std::max(
                    pageMeshletCounts[groupLocalPageIndex],
                    segment.firstMeshletInPage + segment.meshletCount);
            }

            uint32_t localPageIndex = 0;
            for (const PagePool::PageAllocation& allocation : group.pageAllocations) {
                for (uint32_t pageOffset = 0; pageOffset < allocation.pageCount; ++pageOffset) {
                    const uint32_t pageMeshletCount = localPageIndex < pageMeshletCounts.size()
                        ? pageMeshletCounts[localPageIndex]
                        : 0u;
                    const uint32_t pageId = allocation.firstPageID + pageOffset;
                    PageSource source{};
                    source.groupGlobalIndex = group.groupGlobalIndex;
                    source.groupLocalPageIndex = localPageIndex++;
                    source.slabIndex = m_snapshot.pagePool->PageToSlabIndex(pageId);
                    source.slabDescriptorIndex = m_snapshot.pagePool->GetSlabDescriptorIndex(allocation);
                    source.slabByteOffset = m_snapshot.pagePool->PageToSlabByteOffset(pageId);
                    source.firstMeshletInPage = 0u;
                    source.meshletCount = pageMeshletCount;
                    next.buildableClusters += pageMeshletCount;
                    m_pageSources.push_back(source);
                }
            }
        }
    }

    next.pageSources = static_cast<uint32_t>(m_pageSources.size());
    m_stats = next;
}

void CLodRayTracingSystem::EnsureBuffers(
    uint32_t pageSourceCount,
    uint32_t clusterCount,
    uint64_t clasDataBytes,
    uint64_t clasScratchBytes,
    uint64_t blasDataBytes,
    uint64_t blasScratchBytes) {
    pageSourceCount = std::max(1u, pageSourceCount);
    clusterCount = std::max(1u, clusterCount);
    clasDataBytes = std::max<uint64_t>(4u, AlignUp(clasDataBytes, 4u));
    clasScratchBytes = std::max<uint64_t>(4u, AlignUp(clasScratchBytes, 4u));
    blasDataBytes = std::max<uint64_t>(4u, AlignUp(blasDataBytes, 4u));
    blasScratchBytes = std::max<uint64_t>(4u, AlignUp(blasScratchBytes, 4u));

    if (!m_pageSourceBuffer) {
        m_pageSourceBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            pageSourceCount,
            sizeof(GpuPageSource),
            false,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_pageSourceBuffer, "CLod RT page sources", "Cluster LOD ray tracing");
    }
    else {
        m_pageSourceBuffer->ResizeStructured(pageSourceCount);
    }

    if (!m_clasBuildInfoBuffer) {
        m_clasBuildInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            clusterCount,
            sizeof(rhi::PackedRayTracingClusterBuildTriangleInfo),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_clasBuildInfoBuffer, "CLod RT CLAS build infos", "Cluster LOD ray tracing");
    }
    else {
        m_clasBuildInfoBuffer->ResizeStructured(clusterCount);
    }

    if (!m_clasAddressBuffer) {
        m_clasAddressBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            clusterCount,
            sizeof(uint64_t),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_clasAddressBuffer, "CLod RT CLAS addresses", "Cluster LOD ray tracing");
    }
    else {
        m_clasAddressBuffer->ResizeStructured(clusterCount);
    }

    if (!m_clasSizeBuffer) {
        m_clasSizeBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            clusterCount,
            sizeof(uint32_t),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_clasSizeBuffer, "CLod RT CLAS sizes", "Cluster LOD ray tracing");
    }
    else {
        m_clasSizeBuffer->ResizeStructured(clusterCount);
    }

    if (!m_clasDataBuffer) {
        m_clasDataBuffer = CreateAliasedUnmaterializedRawBuffer(clasDataBytes, true, false, false);
        SetBufferNameAndUsage(m_clasDataBuffer, "CLod RT CLAS data", "Cluster LOD ray tracing acceleration structures");
    }
    else {
        m_clasDataBuffer->ResizeBytes(clasDataBytes);
    }

    if (!m_clasScratchBuffer) {
        m_clasScratchBuffer = CreateAliasedUnmaterializedRawBuffer(clasScratchBytes, true, false, false);
        SetBufferNameAndUsage(m_clasScratchBuffer, "CLod RT CLAS scratch", "Cluster LOD ray tracing scratch");
    }
    else {
        m_clasScratchBuffer->ResizeBytes(clasScratchBytes);
    }

    if (!m_blasBuildInfoBuffer) {
        m_blasBuildInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo),
            false,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_blasBuildInfoBuffer, "CLod RT aggregate BLAS build info", "Cluster LOD ray tracing");
    }
    else {
        m_blasBuildInfoBuffer->ResizeStructured(1u);
    }

    if (!m_blasAddressBuffer) {
        m_blasAddressBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(uint64_t),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_blasAddressBuffer, "CLod RT aggregate BLAS address", "Cluster LOD ray tracing");
    }
    else {
        m_blasAddressBuffer->ResizeStructured(1u);
    }

    if (!m_blasSizeBuffer) {
        m_blasSizeBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(uint32_t),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_blasSizeBuffer, "CLod RT aggregate BLAS size", "Cluster LOD ray tracing");
    }
    else {
        m_blasSizeBuffer->ResizeStructured(1u);
    }

    if (!m_blasDataBuffer) {
        m_blasDataBuffer = CreateAliasedUnmaterializedRawBuffer(blasDataBytes, true, false, false);
        SetBufferNameAndUsage(m_blasDataBuffer, "CLod RT aggregate BLAS data", "Cluster LOD ray tracing acceleration structures");
    }
    else {
        m_blasDataBuffer->ResizeBytes(blasDataBytes);
    }

    if (!m_blasScratchBuffer) {
        m_blasScratchBuffer = CreateAliasedUnmaterializedRawBuffer(blasScratchBytes, true, false, false);
        SetBufferNameAndUsage(m_blasScratchBuffer, "CLod RT aggregate BLAS scratch", "Cluster LOD ray tracing scratch");
    }
    else {
        m_blasScratchBuffer->ResizeBytes(blasScratchBytes);
    }
}

void CLodRayTracingSystem::UpdateGpuResources(rhi::Device device, const RayTracingFeatureInfo& rayTracingFeatures) {
    m_stats.gpuResourcesReady = false;
    m_stats.clasBuildSubmitted = false;
    m_gpuPageSources.clear();
    m_clasBuildInputs = {};
    m_clasTriangleInputs = {};
    m_blasBuildInputs = {};
    m_blasInputs = {};
    m_tlasGeometry = {};
    m_tlasBuildInputs = {};
    m_stats.rayPipelineReady = false;
    m_stats.traceRaysSubmitted = false;
    m_clasDataBytes = 0;
    m_clasScratchBytes = 0;
    m_blasDataBytes = 0;
    m_blasScratchBytes = 0;
    m_tlasDataBytes = 0;
    m_tlasScratchBytes = 0;

    if (!device || !m_snapshot.pagePool || m_stats.buildableClusters == 0u || !rayTracingFeatures.clusterAccelerationStructure) {
        return;
    }

    m_clasTriangleInputs.vertexFormat = rhi::RayTracingClusterVertexFormat::Float32x3;
    m_clasTriangleInputs.maxGeometryIndexValue = 0u;
    m_clasTriangleInputs.maxClusterUniqueGeometryCount = 1u;
    m_clasTriangleInputs.maxClusterTriangleCount = rayTracingFeatures.maxClusterTriangles != 0u
        ? rayTracingFeatures.maxClusterTriangles
        : 256u;
    m_clasTriangleInputs.maxClusterVertexCount = rayTracingFeatures.maxClusterVertices != 0u
        ? rayTracingFeatures.maxClusterVertices
        : 256u;
    m_clasTriangleInputs.maxTotalTriangleCount = m_clasTriangleInputs.maxClusterTriangleCount * m_stats.buildableClusters;
    m_clasTriangleInputs.maxTotalVertexCount = m_clasTriangleInputs.maxClusterVertexCount * m_stats.buildableClusters;
    m_clasTriangleInputs.minPositionTruncateBitCount = 0u;

    m_clasBuildInputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
    m_clasBuildInputs.operationMode = rhi::RayTracingRtasOperationMode::ImplicitDestinations;
    m_clasBuildInputs.flags = rhi::RTASBuild_PreferFastTrace;
    m_clasBuildInputs.maxAccelerationStructureCount = m_stats.buildableClusters;
    m_clasBuildInputs.triangleClusters = &m_clasTriangleInputs;

    rhi::RayTracingRtasPrebuildInfo clasPrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(m_clasBuildInputs, clasPrebuild))
        || clasPrebuild.resultDataMaxSizeInBytes == 0u
        || clasPrebuild.scratchDataSizeInBytes == 0u) {
        return;
    }

    m_blasInputs.maxTotalClusterCount = m_stats.buildableClusters;
    m_blasInputs.maxClusterCountPerAccelerationStructure = m_stats.buildableClusters;

    m_blasBuildInputs.operationType = rhi::RayTracingRtasOperationType::BuildClustersBottomLevel;
    m_blasBuildInputs.operationMode = rhi::RayTracingRtasOperationMode::ImplicitDestinations;
    m_blasBuildInputs.flags = rhi::RTASBuild_PreferFastTrace;
    m_blasBuildInputs.maxAccelerationStructureCount = 1u;
    m_blasBuildInputs.clustersBottomLevel = &m_blasInputs;

    rhi::RayTracingRtasPrebuildInfo blasPrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(m_blasBuildInputs, blasPrebuild))
        || blasPrebuild.resultDataMaxSizeInBytes == 0u
        || blasPrebuild.scratchDataSizeInBytes == 0u) {
        return;
    }

    m_clasDataBytes = AlignUp(clasPrebuild.resultDataMaxSizeInBytes, std::max<uint64_t>(4u, rayTracingFeatures.clusterAlignment));
    m_clasScratchBytes = AlignUp(clasPrebuild.scratchDataSizeInBytes, std::max<uint64_t>(4u, rayTracingFeatures.clusterScratchAlignment));
    m_blasDataBytes = AlignUp(blasPrebuild.resultDataMaxSizeInBytes, std::max<uint64_t>(4u, rayTracingFeatures.clusterAlignment));
    m_blasScratchBytes = AlignUp(blasPrebuild.scratchDataSizeInBytes, std::max<uint64_t>(4u, rayTracingFeatures.clusterScratchAlignment));
    EnsureBuffers(
        static_cast<uint32_t>(m_pageSources.size()),
        m_stats.buildableClusters,
        m_clasDataBytes,
        m_clasScratchBytes,
        m_blasDataBytes,
        m_blasScratchBytes);

    uint32_t outputClusterBase = 0u;
    m_gpuPageSources.reserve(m_pageSources.size());
    for (const PageSource& source : m_pageSources) {
        if (source.meshletCount == 0u) {
            continue;
        }

        std::shared_ptr<Buffer> slab = m_snapshot.pagePool->GetSlab(source.slabIndex);
        if (!slab) {
            continue;
        }
        slab->Materialize();

        GpuPageSource gpuSource{};
        gpuSource.slabDeviceAddress = GetNativeBufferDeviceAddress(slab->GetAPIResource());
        gpuSource.slabByteOffset = source.slabByteOffset;
        gpuSource.slabDescriptorIndex = source.slabDescriptorIndex;
        gpuSource.firstMeshletInPage = source.firstMeshletInPage;
        gpuSource.meshletCount = source.meshletCount;
        gpuSource.outputClusterBase = outputClusterBase;
        outputClusterBase += source.meshletCount;

        if (gpuSource.slabDeviceAddress != 0u) {
            m_gpuPageSources.push_back(gpuSource);
        }
    }

    if (m_gpuPageSources.empty()) {
        return;
    }

    m_stats.buildableClusters = outputClusterBase;
    BUFFER_UPLOAD(
        m_gpuPageSources.data(),
        static_cast<uint32_t>(m_gpuPageSources.size() * sizeof(GpuPageSource)),
        rg::runtime::UploadTarget::FromShared(m_pageSourceBuffer),
        0);

    m_clasAddressBuffer->Materialize();
    rhi::PackedRayTracingClusterBuildBottomLevelInfo aggregateBlasInfo{};
    aggregateBlasInfo.clusterReferencesCount = m_stats.buildableClusters;
    aggregateBlasInfo.clusterReferencesStride = sizeof(uint64_t);
    aggregateBlasInfo.clusterReferences = GetNativeBufferDeviceAddress(m_clasAddressBuffer->GetAPIResource());
    if (aggregateBlasInfo.clusterReferences == 0u) {
        return;
    }

    if (!m_tlasInstanceBuffer) {
        m_tlasInstanceBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(rhi::PackedRayTracingInstanceDesc),
            true,
            false,
            false,
            false);
        SetBufferNameAndUsage(m_tlasInstanceBuffer, "CLod RT aggregate TLAS instance", "Cluster LOD ray tracing");
    }
    else {
        m_tlasInstanceBuffer->ResizeStructured(1u);
    }
    m_tlasInstanceBuffer->Materialize();

    m_tlasGeometry.type = rhi::RayTracingGeometryType::Instances;
    m_tlasGeometry.flags = rhi::RTGeometry_None;
    m_tlasGeometry.instances.instanceBuffer = {
        m_tlasInstanceBuffer->GetAPIResource().GetHandle(),
        0u,
        sizeof(rhi::PackedRayTracingInstanceDesc)
    };
    m_tlasGeometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
    m_tlasGeometry.instances.count = 1u;

    m_tlasBuildInputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    m_tlasBuildInputs.flags = rhi::RTASBuild_PreferFastTrace;
    m_tlasBuildInputs.geometries = { &m_tlasGeometry, 1u };

    rhi::AccelerationStructurePrebuildInfo tlasPrebuild{};
    if (!rhi::IsOk(device.GetAccelerationStructurePrebuildInfo(m_tlasBuildInputs, tlasPrebuild))
        || tlasPrebuild.resultDataMaxSizeInBytes == 0u
        || tlasPrebuild.scratchDataSizeInBytes == 0u) {
        return;
    }

    m_tlasDataBytes = AlignUp(tlasPrebuild.resultDataMaxSizeInBytes, 256u);
    m_tlasScratchBytes = AlignUp(tlasPrebuild.scratchDataSizeInBytes, 256u);
    if (!m_tlasScratchBuffer) {
        m_tlasScratchBuffer = CreateAliasedUnmaterializedRawBuffer(m_tlasScratchBytes, true, false, false);
        SetBufferNameAndUsage(m_tlasScratchBuffer, "CLod RT aggregate TLAS scratch", "Cluster LOD ray tracing scratch");
    }
    else {
        m_tlasScratchBuffer->ResizeBytes(m_tlasScratchBytes);
    }

    if (!m_tlasStorage || m_tlasStorageBytes != m_tlasDataBytes || !m_tlas) {
        m_tlasStorage.Reset();
        m_tlas.Reset();
        m_tlasStorageBytes = 0u;

        auto tlasStorageDesc = rhi::helpers::ResourceDesc::Buffer(
            m_tlasDataBytes,
            rhi::HeapType::DeviceLocal,
            rhi::ResourceFlags::RF_RaytracingAccelerationStructure,
            "CLod RT aggregate TLAS storage");
        if (!rhi::IsOk(device.CreateCommittedResource(tlasStorageDesc, m_tlasStorage)) || !m_tlasStorage) {
            return;
        }

        rhi::AccelerationStructureDesc tlasDesc{};
        tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
        tlasDesc.storage = m_tlasStorage->GetHandle();
        tlasDesc.storageOffset = 0u;
        tlasDesc.sizeBytes = m_tlasDataBytes;
        tlasDesc.flags = rhi::RTASBuild_PreferFastTrace;
        tlasDesc.debugName = "CLod RT aggregate TLAS";
        if (!rhi::IsOk(device.CreateAccelerationStructure(tlasDesc, m_tlas)) || !m_tlas) {
            m_tlasStorage.Reset();
            return;
        }
        m_tlasStorageBytes = m_tlasDataBytes;
    }

    BUFFER_UPLOAD(
        &aggregateBlasInfo,
        sizeof(aggregateBlasInfo),
        rg::runtime::UploadTarget::FromShared(m_blasBuildInfoBuffer),
        0);

    m_stats.gpuResourcesReady = true;
}

void CLodRayTracingSystem::ExecuteClasBuild(rhi::CommandList commandList) {
    if (!commandList || !HasGpuClasBuildInputs()
        || !m_clasDataBuffer
        || !m_clasScratchBuffer
        || !m_clasBuildInfoBuffer
        || !m_clasAddressBuffer
        || !m_clasSizeBuffer) {
        return;
    }

    m_clasDataBuffer->Materialize();
    m_clasScratchBuffer->Materialize();
    m_clasBuildInfoBuffer->Materialize();
    m_clasAddressBuffer->Materialize();
    m_clasSizeBuffer->Materialize();

    rhi::RayTracingRtasOperationDesc desc{};
    desc.inputs = m_clasBuildInputs;
    desc.batched.dstImplicitData = { m_clasDataBuffer->GetAPIResource().GetHandle(), 0u };
    desc.batched.scratchData = { m_clasScratchBuffer->GetAPIResource().GetHandle(), 0u };
    desc.batched.srcInfosArray = {
        m_clasBuildInfoBuffer->GetAPIResource().GetHandle(),
        0u,
        static_cast<uint64_t>(m_stats.buildableClusters) * sizeof(rhi::PackedRayTracingClusterBuildTriangleInfo),
        sizeof(rhi::PackedRayTracingClusterBuildTriangleInfo)
    };
    desc.batched.dstAddressesArray = {
        m_clasAddressBuffer->GetAPIResource().GetHandle(),
        0u,
        static_cast<uint64_t>(m_stats.buildableClusters) * sizeof(uint64_t),
        sizeof(uint64_t)
    };
    desc.batched.dstSizesArray = {
        m_clasSizeBuffer->GetAPIResource().GetHandle(),
        0u,
        static_cast<uint64_t>(m_stats.buildableClusters) * sizeof(uint32_t),
        sizeof(uint32_t)
    };

    commandList.ExecuteIndirectRtasOperations(&desc, 1u);
    m_stats.clasBuildSubmitted = true;
}

void CLodRayTracingSystem::ExecuteBlasBuild(rhi::CommandList commandList) {
    if (!commandList || !HasGpuBlasBuildInputs()
        || !m_blasDataBuffer
        || !m_blasScratchBuffer
        || !m_blasBuildInfoBuffer
        || !m_blasAddressBuffer
        || !m_blasSizeBuffer) {
        return;
    }

    m_blasDataBuffer->Materialize();
    m_blasScratchBuffer->Materialize();
    m_blasBuildInfoBuffer->Materialize();
    m_blasAddressBuffer->Materialize();
    m_blasSizeBuffer->Materialize();

    rhi::RayTracingRtasOperationDesc desc{};
    desc.inputs = m_blasBuildInputs;
    desc.batched.dstImplicitData = { m_blasDataBuffer->GetAPIResource().GetHandle(), 0u };
    desc.batched.scratchData = { m_blasScratchBuffer->GetAPIResource().GetHandle(), 0u };
    desc.batched.srcInfosArray = {
        m_blasBuildInfoBuffer->GetAPIResource().GetHandle(),
        0u,
        sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo),
        sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo)
    };
    desc.batched.dstAddressesArray = {
        m_blasAddressBuffer->GetAPIResource().GetHandle(),
        0u,
        sizeof(uint64_t),
        sizeof(uint64_t)
    };
    desc.batched.dstSizesArray = {
        m_blasSizeBuffer->GetAPIResource().GetHandle(),
        0u,
        sizeof(uint32_t),
        sizeof(uint32_t)
    };

    commandList.ExecuteIndirectRtasOperations(&desc, 1u);
    m_stats.blasBuildSubmitted = true;
}

void CLodRayTracingSystem::ExecuteTlasBuild(rhi::CommandList commandList) {
    if (!commandList || !HasGpuTlasBuildInputs()
        || !m_tlasScratchBuffer
        || !m_tlasInstanceBuffer) {
        return;
    }

    m_tlasScratchBuffer->Materialize();
    m_tlasInstanceBuffer->Materialize();

    m_tlasGeometry.instances.instanceBuffer = {
        m_tlasInstanceBuffer->GetAPIResource().GetHandle(),
        0u,
        sizeof(rhi::PackedRayTracingInstanceDesc)
    };
    m_tlasBuildInputs.geometries = { &m_tlasGeometry, 1u };

    rhi::AccelerationStructureBuildDesc desc{};
    desc.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
    desc.destination = m_tlas->GetHandle();
    desc.inputs = m_tlasBuildInputs;
    desc.scratch = { m_tlasScratchBuffer->GetAPIResource().GetHandle(), 0u, m_tlasScratchBytes };

    commandList.BuildAccelerationStructures(&desc, 1u);
    m_stats.tlasBuildSubmitted = true;
}

void CLodRayTracingSystem::EnsureRayTracingPipeline(rhi::Device device, const RayTracingFeatureInfo& rayTracingFeatures) {
    m_stats.rayPipelineReady = false;
    if (!device
        || !HasGpuTlasBuildInputs()
        || !rayTracingFeatures.pipeline
        || rayTracingFeatures.shaderGroupHandleSize == 0u
        || rayTracingFeatures.shaderTableStrideAlignment == 0u
        || rayTracingFeatures.shaderGroupBaseAlignment == 0u) {
        return;
    }

    if (!m_hasTlasSrvSlot) {
        const auto& heap = DescriptorHeapManager::GetInstance().GetCBVSRVUAVHeap();
        if (!heap) {
            return;
        }
        m_tlasSrvSlot.heap = heap->GetHeap().GetHandle();
        m_tlasSrvSlot.index = heap->AllocateDescriptor();
        m_hasTlasSrvSlot = true;
    }

    rhi::SrvDesc tlasSrv{};
    tlasSrv.dimension = rhi::SrvDim::AccelerationStruct;
    tlasSrv.accel.accelerationStructure = m_tlas->GetHandle();
    tlasSrv.accel.sizeBytes = m_tlasDataBytes;
    if (!rhi::IsOk(device.CreateShaderResourceView(m_tlasSrvSlot, {}, tlasSrv))) {
        return;
    }

    if (!m_rayTracingPso.GetAPIPipelineState()) {
        ShaderLibraryInfo shaderInfo(L"Shaders/ClusterLOD/rayTracedReflections.rt.hlsl", L"lib_6_8");
        ShaderLibraryBundle library = PSOManager::GetInstance().CompileShaderLibrary(shaderInfo, {});

        rhi::SubobjShader shaders[3] = {
            { rhi::ShaderStage::RayGen, rhi::DXIL(library.libraryBlob.Get()), "CLodRtReflectionsRayGen" },
            { rhi::ShaderStage::Miss, rhi::DXIL(library.libraryBlob.Get()), "CLodRtReflectionsMiss" },
            { rhi::ShaderStage::ClosestHit, rhi::DXIL(library.libraryBlob.Get()), "CLodRtReflectionsClosestHit" },
        };
        rhi::RayTracingShaderGroupDesc groups[3] = {
            {
                .type = rhi::RayTracingShaderGroupType::General,
                .name = "CLodRtReflectionsRayGen",
                .generalShader = "CLodRtReflectionsRayGen",
            },
            {
                .type = rhi::RayTracingShaderGroupType::General,
                .name = "CLodRtReflectionsMiss",
                .generalShader = "CLodRtReflectionsMiss",
            },
            {
                .type = rhi::RayTracingShaderGroupType::TrianglesHitGroup,
                .name = "CLodRtReflectionsHitGroup",
                .closestHitShader = "CLodRtReflectionsClosestHit",
            },
        };

        rhi::SubobjRayTracingPipeline rtPipeline{};
        rtPipeline.globalLayout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        rtPipeline.shaders = { shaders, 3u };
        rtPipeline.shaderGroups = { groups, 3u };
        rtPipeline.shaderConfig.maxPayloadSizeInBytes = 32u;
        rtPipeline.shaderConfig.maxAttributeSizeInBytes = 8u;
        rtPipeline.pipelineConfig.maxTraceRecursionDepth = 1u;
        rtPipeline.flags = rhi::RTPipeline_AllowClusteredGeometry;

        rhi::PipelineStreamItem pipelineItems[] = {
            rhi::Make(rtPipeline),
        };

        rhi::PipelinePtr pso;
        if (!rhi::IsOk(device.CreatePipeline(pipelineItems, 1u, pso)) || !pso) {
            return;
        }

        m_rayTracingPso = PipelineState(std::move(pso), library.resourceIDsHash, library.resourceDescriptorSlots);
    }

    m_shaderGroupHandleSize = rayTracingFeatures.shaderGroupHandleSize;
    m_shaderRecordStride = rhi::AlignRayTracingShaderRecordSize(m_shaderGroupHandleSize, rayTracingFeatures.shaderTableStrideAlignment);
    const uint64_t tableAlignment = rayTracingFeatures.shaderGroupBaseAlignment;
    m_rayGenTableOffset = 0u;
    m_missTableOffset = AlignUp(m_rayGenTableOffset + m_shaderRecordStride, tableAlignment);
    m_hitGroupTableOffset = AlignUp(m_missTableOffset + m_shaderRecordStride, tableAlignment);
    m_shaderTableBytes = AlignUp(m_hitGroupTableOffset + m_shaderRecordStride, tableAlignment);

    if (!m_shaderTableBuffer) {
        m_shaderTableBuffer = CreateAliasedUnmaterializedRawBuffer(m_shaderTableBytes, false, false, false);
        SetBufferNameAndUsage(m_shaderTableBuffer, "CLod RT reflections shader table", "Cluster LOD ray tracing");
    }
    else {
        m_shaderTableBuffer->ResizeBytes(m_shaderTableBytes);
    }

    std::vector<uint8_t> groupHandles(static_cast<size_t>(m_shaderGroupHandleSize * 3u));
    if (!rhi::IsOk(device.GetRayTracingShaderGroupHandles(
        m_rayTracingPso.GetAPIPipelineState().GetHandle(),
        0u,
        3u,
        groupHandles.data(),
        static_cast<uint32_t>(groupHandles.size())))) {
        return;
    }

    std::vector<uint8_t> shaderTable(static_cast<size_t>(m_shaderTableBytes), 0u);
    for (uint32_t groupIndex = 0; groupIndex < 3u; ++groupIndex) {
        const uint64_t dstOffset = groupIndex == 0u
            ? m_rayGenTableOffset
            : (groupIndex == 1u ? m_missTableOffset : m_hitGroupTableOffset);
        if (!rhi::WriteRayTracingShaderRecord(
            shaderTable.data() + dstOffset,
            m_shaderRecordStride,
            groupHandles.data() + static_cast<size_t>(groupIndex) * m_shaderGroupHandleSize,
            static_cast<uint32_t>(m_shaderGroupHandleSize))) {
            return;
        }
    }

    BUFFER_UPLOAD(
        shaderTable.data(),
        static_cast<uint32_t>(shaderTable.size()),
        rg::runtime::UploadTarget::FromShared(m_shaderTableBuffer),
        0);

    m_stats.rayPipelineReady = true;
}

void CLodRayTracingSystem::ExecuteTraceRays(rhi::Device device, rhi::CommandList commandList, PixelBuffer& output, uint32_t width, uint32_t height) {
    if (!device || !commandList || !HasRayTracingPipeline() || !m_shaderTableBuffer || width == 0u || height == 0u) {
        return;
    }

    m_shaderTableBuffer->Materialize();
    output.Materialize();

    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_rayTracingPso.GetAPIPipelineState().GetHandle());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_RT_TLAS_DESCRIPTOR_INDEX] = m_tlasSrvSlot.index;
    rootConstants[CLOD_RT_REFLECTION_OUTPUT_DESCRIPTOR_INDEX] = output.GetUAVShaderVisibleInfo(0).slot.index;
    commandList.PushConstants(
        rhi::ShaderStage::AllRayTracing,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    rhi::RayTracingDispatchDesc desc{};
    desc.rayGenerationShaderTable = {
        m_shaderTableBuffer->GetAPIResource().GetHandle(),
        m_rayGenTableOffset,
        m_shaderRecordStride,
        m_shaderRecordStride
    };
    desc.missShaderTable = {
        m_shaderTableBuffer->GetAPIResource().GetHandle(),
        m_missTableOffset,
        m_shaderRecordStride,
        m_shaderRecordStride
    };
    desc.hitGroupTable = {
        m_shaderTableBuffer->GetAPIResource().GetHandle(),
        m_hitGroupTableOffset,
        m_shaderRecordStride,
        m_shaderRecordStride
    };
    desc.width = width;
    desc.height = height;
    desc.depth = 1u;

    commandList.TraceRays(desc);
    m_stats.traceRaysSubmitted = true;
}

void CLodRayTracingSystem::Reset() {
    m_snapshot.residentGroups.clear();
    m_snapshot.pagePool = nullptr;
    m_snapshot.pagePoolGeneration = 0;
    m_pageSources.clear();
    m_gpuPageSources.clear();
    m_tlas.Reset();
    m_tlasStorage.Reset();
    m_tlasStorageBytes = 0u;
    m_stats = {};
}

} // namespace br::render
