#pragma once

#include <memory>
#include <vector>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct ClearDeepVisibilityBindings {
    struct Clear { org::DeclaredViewToken gpu, cpu; };
    std::vector<Clear> headPointers;
};

class ClearDeepVisibilityPass final : public org::TypedRenderGraphPass<ClearDeepVisibilityPass,
    org::EmptyPassFrameData, ClearDeepVisibilityBindings>, public org::IDynamicDeclaredResources {
public:
    ClearDeepVisibilityPass(
        std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityStatsBuffer);

    ClearDeepVisibilityBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    static void Record(const ClearDeepVisibilityBindings& bindings, org::PassRecordContext& context);

private:
    std::shared_ptr<org::Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityStatsBuffer;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_headPointerTextures;
    bool m_declaredResourcesChanged = true;
};
