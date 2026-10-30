#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITOccupancyHistogramBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, occupancy, sliceMask, histogram;
};
struct AVBOITOccupancyHistogramFrame {
    uint32_t groupsX = 0, groupsY = 1;
};

class AVBOITOccupancyHistogramPass final : public org::TypedRenderGraphPass<AVBOITOccupancyHistogramPass,
    AVBOITOccupancyHistogramFrame, AVBOITOccupancyHistogramBindings> {
public:
    AVBOITOccupancyHistogramPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::Buffer> occupancyHistogramBuffer);

    AVBOITOccupancyHistogramBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    AVBOITOccupancyHistogramFrame Prepare(const AVBOITOccupancyHistogramBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITOccupancyHistogramBindings&, const AVBOITOccupancyHistogramFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::Buffer> m_occupancyHistogramBuffer;
    org::PipelineState m_pso;
};
