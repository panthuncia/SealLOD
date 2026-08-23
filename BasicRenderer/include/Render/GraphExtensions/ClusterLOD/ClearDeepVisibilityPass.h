#pragma once

#include <memory>
#include <vector>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/RenderPass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class ClearDeepVisibilityPass final : public RenderPass, public IDynamicDeclaredResources {
public:
    ClearDeepVisibilityPass(
        std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<Buffer> deepVisibilityStatsBuffer);

    void DeclareResourceUsages(RenderPassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityStatsBuffer;
    std::vector<std::shared_ptr<PixelBuffer>> m_headPointerTextures;
    bool m_declaredResourcesChanged = true;
};
