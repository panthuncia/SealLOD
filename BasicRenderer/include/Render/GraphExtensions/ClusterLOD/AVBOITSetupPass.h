#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "ShaderBuffers.h"
#include "RenderPasses/PreparedResourceClears.h"
#include <array>
#include <vector>

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

using AVBOITSetupFrameData = br::render::PreparedResourceClears;

struct AVBOITSetupBindings {
    std::vector<org::ResourceBindingToken> clears;
    std::vector<org::ResourceBindingToken> targets;
};

class AVBOITSetupPass final : public org::TypedRenderGraphPass<AVBOITSetupPass,
    AVBOITSetupFrameData, AVBOITSetupBindings> {
public:
    AVBOITSetupPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> fitStateBuffer,
        std::shared_ptr<org::Buffer> depthWarpLUTBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> coverageTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> integratedTransmittanceTexture,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture,
        std::shared_ptr<org::PixelBuffer> accumulationTexture,
        std::shared_ptr<org::PixelBuffer> normalizationTexture,
        std::shared_ptr<org::PixelBuffer> shadingExtinctionTexture);

    AVBOITSetupBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    AVBOITSetupFrameData Prepare(const AVBOITSetupBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITSetupBindings&, const AVBOITSetupFrameData&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_fitStateBuffer;
    std::shared_ptr<org::Buffer> m_depthWarpLUTBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_coverageTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_integratedTransmittanceTexture;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    std::shared_ptr<org::PixelBuffer> m_accumulationTexture;
    std::shared_ptr<org::PixelBuffer> m_normalizationTexture;
    std::shared_ptr<org::PixelBuffer> m_shadingExtinctionTexture;
    CLodAVBOITConfig m_config{};
    bool m_fitStateInitialized = false;
};
