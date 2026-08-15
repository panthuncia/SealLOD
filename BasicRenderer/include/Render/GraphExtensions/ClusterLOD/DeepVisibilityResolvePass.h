#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class DeepVisibilityResolvePass final : public ComputePass, public IDynamicDeclaredResources {
public:
    DeepVisibilityResolvePass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> reyesDiceQueueBuffer,
        std::shared_ptr<Buffer> reyesTessTableConfigsBuffer,
        std::shared_ptr<Buffer> reyesTessTableVerticesBuffer,
        std::shared_ptr<Buffer> reyesTessTableTrianglesBuffer,
        std::shared_ptr<Buffer> deepVisibilityNodesBuffer,
        std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<Buffer> deepVisibilityStatsBuffer,
        uint32_t patchVisibilityIndexBase);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableTrianglesBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityStatsBuffer;
    uint32_t m_patchVisibilityIndexBase = 0u;
    std::shared_ptr<PixelBuffer> m_primaryHeadPointerTexture;

    PixelBuffer* m_pHDRTarget = nullptr;
    bool m_declaredResourcesChanged = true;

    std::function<bool()> m_getPunctualLightingEnabled;
    std::function<bool()> m_getShadowsEnabled;
    bool m_gtaoEnabled = true;
};
