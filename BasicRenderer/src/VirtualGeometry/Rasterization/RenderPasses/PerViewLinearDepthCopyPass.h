#pragma once

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include <array>
#include <memory>
#include <vector>

namespace org { class PixelBuffer; }

struct PerViewLinearDepthCopyBindingView {
    org::DeclaredViewToken visibility, linearDepth;
    uint32_t width = 0, height = 0;
    bool primary = false;
    std::array<uint32_t, 2> projection{};
};

struct PerViewLinearDepthCopyBindings {
    org::PipelineState pipeline;
    std::vector<PerViewLinearDepthCopyBindingView> views;
    org::DeclaredViewToken projectedDepth, canonicalDeviceDepth;
    bool hasProjectedDepth = false, hasCanonicalDeviceDepth = false;
};

class PerViewLinearDepthCopyPass : public org::TypedRenderGraphPass<PerViewLinearDepthCopyPass,
    org::EmptyPassFrameData, PerViewLinearDepthCopyBindings>, public org::IDynamicDeclaredResources {
public:
    explicit PerViewLinearDepthCopyPass(bool writeProjectedDepth = true);

    PerViewLinearDepthCopyBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext&) override;
    bool DeclaredResourcesChanged() const override;
    static void Record(const PerViewLinearDepthCopyBindings& bindings, org::PassRecordContext& context);

private:
    org::PipelineState m_pso;
    struct ViewSnapshot {
        std::shared_ptr<org::PixelBuffer> visibility, linearDepth;
        uint32_t width = 0, height = 0;
        bool primary = false;
        std::array<uint32_t, 2> projection{};
        bool operator==(const ViewSnapshot&) const = default;
    };
    std::vector<ViewSnapshot> m_views;
    bool m_declaredResourcesChanged = true;
    bool m_writeProjectedDepth = true;
};
