#pragma once

#include <array>
#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

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
      public IDynamicDeclaredResources {
public:
    VoxelSoftwareRasterizationPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> rigidVoxelWorkRecordsBuffer,
        std::shared_ptr<Buffer> rigidVoxelWorkCounterBuffer,
        std::shared_ptr<Buffer> skinnedVoxelWorkRecordsBuffer,
        std::shared_ptr<Buffer> skinnedVoxelWorkCounterBuffer,
        std::shared_ptr<Buffer> rigidVoxelIndirectArgsBuffer,
        std::shared_ptr<Buffer> skinnedVoxelIndirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup,
        uint32_t voxelWorkCapacity);
    ~VoxelSoftwareRasterizationPass() override;

    VoxelRasterBindings Declare(org::PassBuilder& builder);
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    VoxelRasterFrameData Prepare(const VoxelRasterBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const VoxelRasterBindings&, const VoxelRasterFrameData&, org::PassRecordContext&);

private:
    PipelineState m_buildArgsPso;
    PipelineState m_rigidRasterPso;
    PipelineState m_skinnedRasterPso;
    PipelineState m_rigidTelemetryRasterPso;
    PipelineState m_skinnedTelemetryRasterPso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_dispatchCommandSignature;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClusterTransformIndicesBuffer;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelWorkRecordsBuffers;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelWorkCounterBuffers;
    std::array<std::shared_ptr<Buffer>, 2> m_voxelIndirectArgsBuffers;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    // The per-view table the shader reads; it embeds the visibility UAVs, so
    // it is published during preparation from the frame's bindings.
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Voxel Raster View Raster Info"};
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    uint32_t m_voxelWorkCapacity = 0u;
    bool m_declaredResourcesChanged = true;
};
