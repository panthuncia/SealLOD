#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class VirtualShadowMapGatherStatsPass final : public ComputePass {
public:
    VirtualShadowMapGatherStatsPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> allocationCountBuffer,
        std::shared_ptr<Buffer> allocationIndirectArgsBuffer,
        std::shared_ptr<Buffer> pageListHeaderBuffer,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        bool capturePreAllocateState);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_allocationCountBuffer;
    std::shared_ptr<Buffer> m_allocationIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_pageListHeaderBuffer;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    bool m_capturePreAllocateState = false;
};