#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
namespace org { class ResourceGroup; }

struct ReyesShadowHardwareFrameData {
    struct Bucket {
        org::PreparedProgramReference program;
        std::vector<unsigned int> descriptorIndices;
        uint64_t argumentsOffset = 0;
    };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::CommandSignatureHandle signature{};
    org::PreparedResourceReference arguments;
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    std::vector<Bucket> buckets;
    uint32_t width = 1, height = 1;
};

struct ReyesShadowHardwareBindings {
    org::ResourceBindingToken visible, histogram, indirectArgs, packedWork, compactedIndices, work, diceQueue;
    org::ResourceBindingToken tessConfigs, tessVertices, tessTriangles, pageTable, physicalPages, dynamicPages, clipmapInfo, telemetry, viewInfo;
    uint32_t width = 1, height = 1, pageTableResolution = 0, virtualResolution = 0;
    std::vector<uint32_t> bucketFlags;
};

class ReyesVirtualShadowHardwareRasterPass final : public org::TypedRenderGraphPass<ReyesVirtualShadowHardwareRasterPass,
    ReyesShadowHardwareFrameData, ReyesShadowHardwareBindings>, public org::IDynamicDeclaredResources {
public:
    ReyesVirtualShadowHardwareRasterPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> packedRasterWorkGroupsBuffer,
        std::shared_ptr<org::Buffer> compactedRasterWorkIndicesBuffer,
        std::shared_ptr<org::Buffer> rasterWorkBuffer,
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup);
    ~ReyesVirtualShadowHardwareRasterPass();

    ReyesShadowHardwareBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    ReyesShadowHardwareFrameData Prepare(const ReyesShadowHardwareBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesShadowHardwareBindings&, const ReyesShadowHardwareFrameData& data, org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_packedRasterWorkGroupsBuffer;
    std::shared_ptr<org::Buffer> m_compactedRasterWorkIndicesBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::Buffer> m_viewRasterInfoBuffer;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    std::shared_ptr<rhi::CommandSignaturePtr> m_rasterizationCommandSignature;
    uint32_t m_passWidth = 1u;
    uint32_t m_passHeight = 1u;
    bool m_declaredResourcesChanged = true;
    CLodVirtualShadowResolutionConfig m_shadowConfig{};
    std::vector<uint32_t> m_bucketFlags;
};
