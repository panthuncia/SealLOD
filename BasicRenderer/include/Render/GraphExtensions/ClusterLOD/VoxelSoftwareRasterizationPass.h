#pragma once

#include <array>
#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/PixelBuffer.h"

class Buffer;
class ResourceGroup;

class VoxelSoftwareRasterizationPass : public ComputePass, public IDynamicDeclaredResources {
public:
    VoxelSoftwareRasterizationPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> rigidVoxelWorkRecordsBuffer,
        std::shared_ptr<Buffer> rigidVoxelWorkCounterBuffer,
        std::shared_ptr<Buffer> skinnedVoxelWorkRecordsBuffer,
        std::shared_ptr<Buffer> skinnedVoxelWorkCounterBuffer,
        std::shared_ptr<Buffer> rigidVoxelIndirectArgsBuffer,
        std::shared_ptr<Buffer> skinnedVoxelIndirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        std::shared_ptr<Buffer> viewRasterInfoBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup,
        uint32_t voxelWorkCapacity);
    ~VoxelSoftwareRasterizationPass() override;

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_buildArgsPso;
    PipelineState m_rigidRasterPso;
    PipelineState m_skinnedRasterPso;
    PipelineState m_rigidTelemetryRasterPso;
    PipelineState m_skinnedTelemetryRasterPso;
    rhi::CommandSignaturePtr m_dispatchCommandSignature;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelWorkRecordsBuffers;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelWorkCounterBuffers;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelIndirectArgsBuffers;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    uint32_t m_voxelWorkCapacity = 0u;
    bool m_declaredResourcesChanged = true;
};
