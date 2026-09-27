#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/DeclaredTableLayout.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesDeepVisibilityRasterBindings {
    org::DeclaredViewToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::DeclaredViewToken tessConfigs, tessVertices, tessTriangles;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken telemetry;
    org::DeclaredViewToken nodes, nodeCounter, overflowCounter;
    org::DeclaredTableLayout<CLodViewRasterInfo> viewRasterInfoTable;
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
    // Rows are selected in Update; Declare binds their frozen descriptor views.
    org::PreparedTablePublisher m_viewRasterInfoPublisher;

    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_deepVisibilityNodeCapacity = 1u;

    struct ViewInput {
        uint32_t cameraIndex = 0;
        std::shared_ptr<org::PixelBuffer> visibility, headPointers;
        bool operator==(const ViewInput&) const = default;
    };
    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    std::vector<ViewInput> m_viewInputs;
    bool m_declaredResourcesChanged = true;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
