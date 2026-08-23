#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class VirtualShadowMapBuildMarkTilesPass final : public ComputePass {
public:
    VirtualShadowMapBuildMarkTilesPass(
        std::shared_ptr<Buffer> tileWorkBuffer,
        std::shared_ptr<Buffer> tileCountBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_tileWorkBuffer;
    std::shared_ptr<Buffer> m_tileCountBuffer;
};