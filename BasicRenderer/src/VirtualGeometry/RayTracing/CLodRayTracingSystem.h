#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <source_location>
#include <vector>

#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Render/PipelineState.h"
#include <rhi.h>

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
namespace org::runtime { class IUploadService; }
namespace org::runtime { struct UploadTarget; }

namespace br::render {

class CLodRayTracingSystem {
public:
    explicit CLodRayTracingSystem(std::shared_ptr<org::runtime::IUploadService> uploads = {});
    void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploads) noexcept;
    std::mutex& FrameOperationMutex() noexcept { return m_frameOperationMutex; }
    struct Stats {
        uint32_t residentGroups = 0;
        uint32_t residentPages = 0;
        uint32_t pageSources = 0;
        uint32_t buildableGroups = 0;
        uint32_t meshlets = 0;
        uint32_t triangles = 0;
        uint32_t buildableClusters = 0;
        uint64_t snapshotGeneration = 0;
        bool hasPagePool = false;
        bool gpuResourcesReady = false;
        bool clasBuildSubmitted = false;
        bool blasBuildSubmitted = false;
        bool tlasBuildSubmitted = false;
        bool rayPipelineReady = false;
        bool traceRaysSubmitted = false;
    };

    struct PageSource {
        uint32_t groupGlobalIndex = 0;
        uint32_t groupLocalPageIndex = 0;
        uint32_t slabIndex = 0;
        uint32_t slabDescriptorIndex = 0;
        uint64_t slabByteOffset = 0;
        uint32_t firstMeshletInPage = 0;
        uint32_t meshletCount = 0;
    };

    struct alignas(8) GpuPageSource {
        uint64_t slabDeviceAddress = 0;
        uint64_t slabByteOffset = 0;
        uint32_t slabDescriptorIndex = 0;
        uint32_t firstMeshletInPage = 0;
        uint32_t meshletCount = 0;
        uint32_t outputClusterBase = 0;
    };

    void Refresh(const MeshManager& meshManager);
    void Reset();
    void UpdateGpuResources(rhi::Device device, const RayTracingFeatureInfo& rayTracingFeatures);
    void ExecuteClasBuild(rhi::CommandList commandList);
    void ExecuteBlasBuild(rhi::CommandList commandList);
    void ExecuteTlasBuild(rhi::CommandList commandList);
    void EnsureRayTracingPipeline(rhi::Device device, const RayTracingFeatureInfo& rayTracingFeatures);
    void ExecuteTraceRays(rhi::Device device, rhi::CommandList commandList,
        org::PixelBuffer& output, uint32_t outputUAVIndex, uint32_t width, uint32_t height);

    const Stats& GetStats() const { return m_stats; }
    const br::render::CLodRayTracingResidencySnapshot& GetSnapshot() const { return m_snapshot; }
    const std::vector<PageSource>& GetPageSources() const { return m_pageSources; }
    uint32_t GetGpuPageSourceCount() const { return static_cast<uint32_t>(m_gpuPageSources.size()); }
    bool HasBuildableGeometry() const { return m_stats.buildableGroups > 0; }
    bool HasGpuClasBuildInputs() const { return m_stats.gpuResourcesReady && m_stats.buildableClusters > 0; }
    bool HasGpuBlasBuildInputs() const { return HasGpuClasBuildInputs() && m_blasBuildInfoBuffer != nullptr; }
    bool HasGpuTlasBuildInputs() const { return HasGpuBlasBuildInputs() && static_cast<bool>(m_tlas) && m_tlasInstanceBuffer != nullptr; }
    bool HasRayTracingPipeline() const { return m_stats.rayPipelineReady; }

    std::shared_ptr<org::Buffer> GetPageSourceBuffer() const { return m_pageSourceBuffer; }
    std::shared_ptr<org::Buffer> GetClasBuildInfoBuffer() const { return m_clasBuildInfoBuffer; }
    std::shared_ptr<org::Buffer> GetClasAddressBuffer() const { return m_clasAddressBuffer; }
    std::shared_ptr<org::Buffer> GetClasSizeBuffer() const { return m_clasSizeBuffer; }
    std::shared_ptr<org::Buffer> GetClasDataBuffer() const { return m_clasDataBuffer; }
    std::shared_ptr<org::Buffer> GetClasScratchBuffer() const { return m_clasScratchBuffer; }
    std::shared_ptr<org::Buffer> GetBlasBuildInfoBuffer() const { return m_blasBuildInfoBuffer; }
    std::shared_ptr<org::Buffer> GetBlasAddressBuffer() const { return m_blasAddressBuffer; }
    std::shared_ptr<org::Buffer> GetBlasSizeBuffer() const { return m_blasSizeBuffer; }
    std::shared_ptr<org::Buffer> GetBlasDataBuffer() const { return m_blasDataBuffer; }
    std::shared_ptr<org::Buffer> GetBlasScratchBuffer() const { return m_blasScratchBuffer; }
    std::shared_ptr<org::Buffer> GetTlasInstanceBuffer() const { return m_tlasInstanceBuffer; }
    std::shared_ptr<org::Buffer> GetTlasScratchBuffer() const { return m_tlasScratchBuffer; }
    rhi::AccelerationStructure GetTlas() const { return m_tlas ? m_tlas.Get() : rhi::AccelerationStructure{}; }

private:
    org::runtime::IUploadService& UploadService() const;
    void UploadBufferData(const void* data, size_t size, org::runtime::UploadTarget target,
        size_t offset, std::source_location source = std::source_location::current()) const;
    std::mutex m_frameOperationMutex;
    std::weak_ptr<org::runtime::IUploadService> m_uploadService;
    void EnsureBuffers(
        uint32_t pageSourceCount,
        uint32_t clusterCount,
        uint64_t clasDataBytes,
        uint64_t clasScratchBytes,
        uint64_t blasDataBytes,
        uint64_t blasScratchBytes);

    br::render::CLodRayTracingResidencySnapshot m_snapshot;
    std::vector<PageSource> m_pageSources;
    std::vector<GpuPageSource> m_gpuPageSources;
    Stats m_stats{};
    rhi::RayTracingRtasOperationInputs m_clasBuildInputs{};
    rhi::RayTracingClusterTriangleInputsDesc m_clasTriangleInputs{};
    rhi::RayTracingRtasOperationInputs m_blasBuildInputs{};
    rhi::RayTracingClusterBottomLevelInputsDesc m_blasInputs{};
    rhi::RayTracingGeometryDesc m_tlasGeometry{};
    rhi::AccelerationStructureBuildInputs m_tlasBuildInputs{};
    uint64_t m_clasDataBytes = 0;
    uint64_t m_clasScratchBytes = 0;
    uint64_t m_blasDataBytes = 0;
    uint64_t m_blasScratchBytes = 0;
    uint64_t m_tlasDataBytes = 0;
    uint64_t m_tlasScratchBytes = 0;
    uint64_t m_tlasStorageBytes = 0;

    std::shared_ptr<org::Buffer> m_pageSourceBuffer;
    std::shared_ptr<org::Buffer> m_clasBuildInfoBuffer;
    std::shared_ptr<org::Buffer> m_clasAddressBuffer;
    std::shared_ptr<org::Buffer> m_clasSizeBuffer;
    std::shared_ptr<org::Buffer> m_clasDataBuffer;
    std::shared_ptr<org::Buffer> m_clasScratchBuffer;
    std::shared_ptr<org::Buffer> m_blasBuildInfoBuffer;
    std::shared_ptr<org::Buffer> m_blasAddressBuffer;
    std::shared_ptr<org::Buffer> m_blasSizeBuffer;
    std::shared_ptr<org::Buffer> m_blasDataBuffer;
    std::shared_ptr<org::Buffer> m_blasScratchBuffer;
    std::shared_ptr<org::Buffer> m_tlasInstanceBuffer;
    std::shared_ptr<org::Buffer> m_tlasScratchBuffer;
    std::shared_ptr<org::Buffer> m_shaderTableBuffer;
    rhi::ResourcePtr m_tlasStorage;
    rhi::AccelerationStructurePtr m_tlas;
    org::PipelineState m_rayTracingPso;
    rhi::DescriptorSlot m_tlasSrvSlot{};
    bool m_hasTlasSrvSlot = false;
    uint64_t m_shaderTableBytes = 0;
    uint64_t m_rayGenTableOffset = 0;
    uint64_t m_missTableOffset = 0;
    uint64_t m_hitGroupTableOffset = 0;
    uint64_t m_shaderRecordStride = 0;
    uint64_t m_shaderGroupHandleSize = 0;
};

} // namespace br::render
