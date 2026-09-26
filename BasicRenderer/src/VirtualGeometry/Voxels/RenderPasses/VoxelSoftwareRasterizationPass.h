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
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct VoxelRasterFrameData {
    struct Step {
        org::PreparedProgramBinding buildProgram{}, rasterProgram{};
        std::array<uint32_t, NumMiscUintRootConstants> constants{};
        org::PreparedResourceReference arguments{};
    };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::CommandSignatureHandle commandSignature{};
    std::array<Step, 2> steps;
};

struct VoxelRasterBindings {
    org::ResourceBindingToken visible, transforms, telemetry;
    std::array<org::ResourceBindingToken, 2> workRecords, workCounters, indirectArgs;
    org::ResourceBindingToken pageTable, clipmapInfo, physicalPages, dynamicPages;
    bool hasTelemetry = false, virtualShadow = false;
};

class VoxelSoftwareRasterizationPass
    : public org::TypedRenderGraphPass<VoxelSoftwareRasterizationPass, VoxelRasterFrameData, VoxelRasterBindings>,
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
    VoxelRasterFrameData Prepare(const VoxelRasterBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const VoxelRasterBindings&, const VoxelRasterFrameData&, org::PassRecordContext&);

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
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    uint32_t m_voxelWorkCapacity = 0u;
    bool m_declaredResourcesChanged = true;
};
