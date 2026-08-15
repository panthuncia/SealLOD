#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/RenderPass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

class ReyesVirtualShadowHardwareRasterPass final : public RenderPass, public IDynamicDeclaredResources {
public:
    ReyesVirtualShadowHardwareRasterPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<Buffer> packedRasterWorkGroupsBuffer,
        std::shared_ptr<Buffer> compactedRasterWorkIndicesBuffer,
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> tessTableVerticesBuffer,
        std::shared_ptr<Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup);
    ~ReyesVirtualShadowHardwareRasterPass();

    void DeclareResourceUsages(RenderPassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_packedRasterWorkGroupsBuffer;
    std::shared_ptr<Buffer> m_compactedRasterWorkIndicesBuffer;
    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_diceQueueBuffer;
    std::shared_ptr<Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    rhi::CommandSignaturePtr m_rasterizationCommandSignature;
    uint32_t m_passWidth = 1u;
    uint32_t m_passHeight = 1u;
    bool m_declaredResourcesChanged = true;
};
