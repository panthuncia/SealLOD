#pragma once

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
        std::shared_ptr<Buffer> voxelWorkRecordsBuffer,
        std::shared_ptr<Buffer> voxelWorkCounterBuffer,
        std::shared_ptr<Buffer> voxelIndirectArgsBuffer,
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
    PipelineState m_rasterPso;
    rhi::CommandSignaturePtr m_dispatchCommandSignature;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_voxelWorkRecordsBuffer;
    std::shared_ptr<Buffer> m_voxelWorkCounterBuffer;
    std::shared_ptr<Buffer> m_voxelIndirectArgsBuffer;
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
