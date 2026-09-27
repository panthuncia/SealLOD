#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/DeclaredTableLayout.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesPatchRasterBindings {
    org::DeclaredViewToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::DeclaredViewToken tessConfigs, tessVertices, tessTriangles;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken telemetry;
    org::DeclaredTableLayout<CLodViewRasterInfo> viewRasterInfoTable;
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
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Reyes Patch Raster View Raster Info"};
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    uint32_t m_maxDiceQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    uint32_t m_patchVisibilityIndexBase = 0u;
    struct ViewInput {
        uint32_t cameraIndex = 0;
        std::shared_ptr<org::PixelBuffer> visibility;
        bool operator==(const ViewInput&) const = default;
    };
    std::vector<ViewInput> m_viewInputs;
    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    bool m_declaredResourcesChanged = true;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
