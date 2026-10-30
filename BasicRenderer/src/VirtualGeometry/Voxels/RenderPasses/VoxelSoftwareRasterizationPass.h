#pragma once

#include <array>
#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct VoxelRasterBindings {
    org::PipelineState buildArgs;
    // Rigid then skinned, without and with telemetry.
    std::array<org::PipelineState, 2> raster, telemetryRaster;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    org::DeclaredViewToken visible, transforms, telemetry;
    std::array<org::DeclaredViewToken, 2> workRecords, workCounters, indirectArgs;
    org::DeclaredViewToken pageTable, clipmapInfo, physicalPages, dynamicPages;
    org::DeclaredTableLayout<CLodViewRasterInfo> viewRasterInfoLayout;
    uint32_t voxelWorkCapacity = 0;
    bool hasTelemetry = false, virtualShadow = false;
};
struct VoxelRasterFrame {
    uint32_t viewRasterInfoTable = 0;
    uint32_t pageTableResolution = 0, virtualResolution = 0;
    bool telemetry = false;
};

class VoxelSoftwareRasterizationPass
    : public org::TypedRenderGraphPass<VoxelSoftwareRasterizationPass, VoxelRasterFrame, VoxelRasterBindings>,
      public org::IDynamicDeclaredResources {
public:
    VoxelSoftwareRasterizationPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> rigidVoxelWorkRecordsBuffer,
        std::shared_ptr<org::Buffer> rigidVoxelWorkCounterBuffer,
        std::shared_ptr<org::Buffer> skinnedVoxelWorkRecordsBuffer,
        std::shared_ptr<org::Buffer> skinnedVoxelWorkCounterBuffer,
        std::shared_ptr<org::Buffer> rigidVoxelIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> skinnedVoxelIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup,
        uint32_t voxelWorkCapacity);
    ~VoxelSoftwareRasterizationPass() override;

    VoxelRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    VoxelRasterFrame Prepare(const VoxelRasterBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const VoxelRasterBindings& bindings, const VoxelRasterFrame& frame,
        org::PassRecordContext& context);

private:
    org::PipelineState m_buildArgsPso;
    org::PipelineState m_rigidRasterPso;
    org::PipelineState m_skinnedRasterPso;
    org::PipelineState m_rigidTelemetryRasterPso;
    org::PipelineState m_skinnedTelemetryRasterPso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_dispatchCommandSignature;
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    std::array<std::shared_ptr<org::Buffer>, 2> m_voxelWorkRecordsBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_voxelWorkCounterBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_voxelIndirectArgsBuffers;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    // The per-view table the shader reads; it embeds the visibility UAVs, so
    // it is published during preparation from the frame's bindings.
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Voxel Raster View Raster Info"};
    CLodDeclaredViewRasterTable m_viewRasterInfoTable;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    uint32_t m_voxelWorkCapacity = 0u;
    bool m_declaredResourcesChanged = true;
};
