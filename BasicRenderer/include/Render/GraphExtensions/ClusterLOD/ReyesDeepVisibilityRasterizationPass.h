#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

class ReyesDeepVisibilityRasterizationPass final : public ComputePass, public IDynamicDeclaredResources {
public:
    ReyesDeepVisibilityRasterizationPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> diceQueueCounterBuffer,
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> tessTableVerticesBuffer,
        std::shared_ptr<Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        std::shared_ptr<Buffer> deepVisibilityNodesBuffer,
        std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup,
        std::string_view resourceName,
        uint32_t patchVisibilityIndexBase);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_diceQueueBuffer;
    std::shared_ptr<Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;

    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_deepVisibilityNodeCapacity = 1u;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    std::vector<std::shared_ptr<PixelBuffer>> m_deepVisibilityHeadPointerBuffers;
    bool m_declaredResourcesChanged = true;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};
