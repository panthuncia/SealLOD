#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct DeepVisibilityResolveBindings {
    org::ResourceBindingToken headPointers, nodes, counter, overflow, visibleClusters, stats;
    org::ResourceBindingToken diceQueue, tessConfigs, tessVertices, tessTriangles;
    uint32_t patchIndexBase = 0, width = 0, height = 0, globalPsoFlags = 0;
    bool ready = false, shadows = false, punctualLights = false, gtao = false;
    bool hasDiceQueue = false, hasTessTables = false;
    bool useNormalMaps = false;
    float terrainNormalBlend = 0, terrainNormalMipBias = 0, objectNormalMapBlend = 0;
};

class DeepVisibilityResolvePass final : public org::TypedRenderGraphPass<DeepVisibilityResolvePass,
    br::render::PreparedComputeDispatch, DeepVisibilityResolveBindings>, public org::IDynamicDeclaredResources {
public:
    DeepVisibilityResolvePass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> reyesDiceQueueBuffer,
        std::shared_ptr<org::Buffer> reyesTessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> reyesTessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> reyesTessTableTrianglesBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityNodesBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityStatsBuffer,
        uint32_t patchVisibilityIndexBase);

    DeepVisibilityResolveBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    br::render::PreparedComputeDispatch Prepare(const DeepVisibilityResolveBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const DeepVisibilityResolveBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_reyesDiceQueueBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableTrianglesBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityStatsBuffer;
    uint32_t m_patchVisibilityIndexBase = 0u;
    std::shared_ptr<org::PixelBuffer> m_primaryHeadPointerTexture;

    org::PixelBuffer* m_pHDRTarget = nullptr;
    bool m_declaredResourcesChanged = true;

    std::function<bool()> m_getPunctualLightingEnabled;
    std::function<bool()> m_getShadowsEnabled;
    bool m_gtaoEnabled = true;
    uint32_t m_renderWidth = 0, m_renderHeight = 0, m_globalPsoFlags = 0;
};
