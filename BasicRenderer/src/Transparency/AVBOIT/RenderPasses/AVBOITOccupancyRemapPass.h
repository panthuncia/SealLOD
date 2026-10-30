#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITOccupancyRemapBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, lut, occupancy;
};
struct AVBOITOccupancyRemapFrame {
    uint32_t groupsX = 0, groupsY = 1;
};
class AVBOITOccupancyRemapPass final : public org::TypedRenderGraphPass<AVBOITOccupancyRemapPass, AVBOITOccupancyRemapFrame, AVBOITOccupancyRemapBindings> {
public:
    AVBOITOccupancyRemapPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::Buffer> depthWarpLUTBuffer);

    AVBOITOccupancyRemapBindings Declare(org::PassBuilder& builder);
    AVBOITOccupancyRemapFrame Prepare(const AVBOITOccupancyRemapBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITOccupancyRemapBindings&, const AVBOITOccupancyRemapFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::Buffer> m_depthWarpLUTBuffer;
    org::PipelineState m_pso;
};
