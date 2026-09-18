#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

struct ReyesPatchRasterBindings {
    org::ResourceBindingToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::ResourceBindingToken tessConfigs, tessVertices, tessTriangles, viewRasterInfo, indirectArgs, telemetry;
    uint32_t phase = 0, patchIndexBase = 0;
    bool enabled = false;
};

class ReyesPatchRasterizationPass final : public org::TypedRenderGraphPass<ReyesPatchRasterizationPass,
    br::render::PreparedComputeIndirect, ReyesPatchRasterBindings>, public IDynamicDeclaredResources {
public:
    ReyesPatchRasterizationPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> diceQueueCounterBuffer,
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> tessTableVerticesBuffer,
        std::shared_ptr<Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<Buffer> viewRasterInfoBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup,
        uint32_t maxDiceQueueEntries,
        uint32_t phaseIndex,
        uint32_t patchVisibilityIndexBase);

    ReyesPatchRasterBindings Declare(org::PassBuilder& builder);
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesPatchRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesPatchRasterBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

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
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    uint32_t m_maxDiceQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    uint32_t m_patchVisibilityIndexBase = 0u;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    bool m_declaredResourcesChanged = true;
    PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
