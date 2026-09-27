#pragma once

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include <array>
#include <memory>
#include <vector>

namespace org { class PixelBuffer; }

struct PerViewLinearDepthCopyPreparedView {
    std::vector<uint32_t> constants;
    uint32_t groupsX = 0, groupsY = 0;
};
struct PerViewLinearDepthCopyPreparedData {
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    org::PreparedProgramReference program{};
    std::vector<PerViewLinearDepthCopyPreparedView> views;
};

struct PerViewLinearDepthCopyBindingView {
    org::ResourceBindingToken visibility, linearDepth;
    uint32_t width = 0, height = 0;
    bool primary = false;
    std::array<uint32_t, 2> projection{};
};

struct PerViewLinearDepthCopyBindings {
    std::vector<PerViewLinearDepthCopyBindingView> views;
    org::ResourceBindingToken projectedDepth, canonicalDeviceDepth;
    bool hasProjectedDepth = false, hasCanonicalDeviceDepth = false;
};

class PerViewLinearDepthCopyPass : public org::TypedRenderGraphPass<PerViewLinearDepthCopyPass,
    PerViewLinearDepthCopyPreparedData, PerViewLinearDepthCopyBindings>, public org::IDynamicDeclaredResources {
public:
    explicit PerViewLinearDepthCopyPass(bool writeProjectedDepth = true);

    PerViewLinearDepthCopyBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext&) override;
    bool DeclaredResourcesChanged() const override;
    PerViewLinearDepthCopyPreparedData Prepare(const PerViewLinearDepthCopyBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const PerViewLinearDepthCopyBindings&,
        const PerViewLinearDepthCopyPreparedData&, org::PassRecordContext&);

private:
    using PreparedView = PerViewLinearDepthCopyPreparedView;
    using PreparedData = PerViewLinearDepthCopyPreparedData;
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
