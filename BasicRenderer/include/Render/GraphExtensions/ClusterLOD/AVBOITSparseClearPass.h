#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class AVBOITSparseClearPass final : public ComputePass {
public:
    AVBOITSparseClearPass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<PixelBuffer> occupancyTexture,
        std::shared_ptr<PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<PixelBuffer> zeroTransmittanceSliceTexture);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<PixelBuffer> m_occupancyTexture;
    std::shared_ptr<PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_zeroTransmittanceSliceTexture;
    PipelineState m_pso;
};