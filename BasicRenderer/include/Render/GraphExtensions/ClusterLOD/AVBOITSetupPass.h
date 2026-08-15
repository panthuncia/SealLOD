#pragma once

#include <memory>

#include "RenderPasses/Base/RenderPass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class AVBOITSetupPass final : public RenderPass {
public:
    AVBOITSetupPass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<Buffer> fitStateBuffer,
        std::shared_ptr<Buffer> depthWarpLUTBuffer,
        std::shared_ptr<PixelBuffer> occupancyTexture,
        std::shared_ptr<PixelBuffer> coverageTexture,
        std::shared_ptr<PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<PixelBuffer> integratedTransmittanceTexture,
        std::shared_ptr<PixelBuffer> zeroTransmittanceSliceTexture,
        std::shared_ptr<PixelBuffer> accumulationTexture,
        std::shared_ptr<PixelBuffer> normalizationTexture,
        std::shared_ptr<PixelBuffer> shadingExtinctionTexture);

    void DeclareResourceUsages(RenderPassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<Buffer> m_fitStateBuffer;
    std::shared_ptr<Buffer> m_depthWarpLUTBuffer;
    std::shared_ptr<PixelBuffer> m_occupancyTexture;
    std::shared_ptr<PixelBuffer> m_coverageTexture;
    std::shared_ptr<PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_integratedTransmittanceTexture;
    std::shared_ptr<PixelBuffer> m_zeroTransmittanceSliceTexture;
    std::shared_ptr<PixelBuffer> m_accumulationTexture;
    std::shared_ptr<PixelBuffer> m_normalizationTexture;
    std::shared_ptr<PixelBuffer> m_shadingExtinctionTexture;
    bool m_fitStateInitialized = false;
};