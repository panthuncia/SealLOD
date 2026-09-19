#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesDeepVisibilityRasterBindings {
    org::ResourceBindingToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::ResourceBindingToken tessConfigs, tessVertices, tessTriangles, indirectArgs, telemetry;
    org::ResourceBindingToken nodes, nodeCounter, overflowCounter;
    std::vector<org::ResourceBindingToken> visibilityBuffers, headPointerBuffers;
    uint32_t patchVisibilityIndexBase = 0u;
    uint32_t nodeCapacity = 1u;
};

class ReyesDeepVisibilityRasterizationPass final : public org::TypedRenderGraphPass<ReyesDeepVisibilityRasterizationPass,
    br::render::PreparedComputeIndirect, ReyesDeepVisibilityRasterBindings>, public org::IDynamicDeclaredResources {
public:
    ReyesDeepVisibilityRasterizationPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> rasterWorkBuffer,
        std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityNodesBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup,
        std::string_view resourceName,
        uint32_t patchVisibilityIndexBase);

    ReyesDeepVisibilityRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesDeepVisibilityRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesDeepVisibilityRasterBindings&, const br::render::PreparedComputeIndirect& data,
        org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    // Built by Update from the view snapshot; its descriptors are resolved and
    // the table published during preparation.
    org::PreparedTablePublisher m_viewRasterInfoPublisher;
    CLodViewRasterInfoTable m_viewRasterInfoTable;

    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_deepVisibilityNodeCapacity = 1u;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos; // Rows without descriptors (change detection).
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_deepVisibilityHeadPointerBuffers;
    bool m_declaredResourcesChanged = true;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
