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
    std::shared_ptr<org::PixelBuffer> source; // The view's visibility buffer: Prepare finds the view's size by it.
    bool primary = false;
};

struct PerViewLinearDepthCopyBindings {
    org::PipelineState pipeline;
    std::vector<PerViewLinearDepthCopyBindingView> views;
    org::DeclaredViewToken projectedDepth, canonicalDeviceDepth;
    bool hasProjectedDepth = false, hasCanonicalDeviceDepth = false;
};

// Per declared view, this frame's size and projection: they change without the declared resources changing (a
// resize keeps the buffer, a new projection keeps everything). A declared view that's gone has width 0.
struct PerViewLinearDepthCopyFrame {
    struct View { uint32_t width = 0, height = 0; std::array<uint32_t, 2> projection{}; };
    std::vector<View> views;
};

class PerViewLinearDepthCopyPass : public org::TypedRenderGraphPass<PerViewLinearDepthCopyPass,
    PerViewLinearDepthCopyFrame, PerViewLinearDepthCopyBindings>, public org::IDynamicDeclaredResources {
public:
    explicit PerViewLinearDepthCopyPass(bool writeProjectedDepth = true);

    PerViewLinearDepthCopyBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext&) override;
    bool DeclaredResourcesChanged() const override;
    PerViewLinearDepthCopyFrame Prepare(const PerViewLinearDepthCopyBindings& bindings,
        const org::PassPrepareContext& preparation) const;
    static void Record(const PerViewLinearDepthCopyBindings& bindings, const PerViewLinearDepthCopyFrame& frame,
        org::PassRecordContext& context);

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
