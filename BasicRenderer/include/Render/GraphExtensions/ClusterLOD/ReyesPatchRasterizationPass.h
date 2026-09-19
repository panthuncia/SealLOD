#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesPatchRasterBindings {
    org::ResourceBindingToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::ResourceBindingToken tessConfigs, tessVertices, tessTriangles, indirectArgs, telemetry;
    uint32_t phase = 0, patchIndexBase = 0;
    bool enabled = false;
};

class ReyesPatchRasterizationPass final : public org::TypedRenderGraphPass<ReyesPatchRasterizationPass,
    br::render::PreparedComputeIndirect, ReyesPatchRasterBindings>, public org::IDynamicDeclaredResources {
public:
    ReyesPatchRasterizationPass(
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
        std::shared_ptr<org::ResourceGroup> slabResourceGroup,
        uint32_t maxDiceQueueEntries,
        uint32_t phaseIndex,
        uint32_t patchVisibilityIndexBase);

    ReyesPatchRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesPatchRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesPatchRasterBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

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
    // The per-view table the shader reads; it embeds the visibility UAVs, so
    // it is published during preparation from the frame's bindings.
    CLodViewRasterInfoTable ViewRasterInfoTable(const org::PassPrepareContext&) const;
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Reyes Patch Raster View Raster Info"};
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    uint32_t m_maxDiceQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    uint32_t m_patchVisibilityIndexBase = 0u;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    bool m_declaredResourcesChanged = true;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
