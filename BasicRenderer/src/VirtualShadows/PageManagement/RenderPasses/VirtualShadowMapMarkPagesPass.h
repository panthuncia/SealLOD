#pragma once

#include <memory>
#include <optional>
#include <array>
#include <vector>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMarkFrameData {
    uint32_t screenWidth = 0, screenHeight = 0;
    uint32_t activeClipmapCount = 0;
    uint32_t receiverSubpageMode = 0;
};

struct VirtualShadowMapMarkPagesBindings {
    org::PipelineState markPipeline, clearPipeline, clearUint2Pipeline;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    org::DeclaredViewToken tileWork, tileCount;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken clipmapData;
    org::DeclaredViewToken mask, list, count;
    std::optional<org::DeclaredViewToken> receiverMask;
};

class VirtualShadowMapMarkPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapMarkPagesPass,
    VirtualShadowMarkFrameData, VirtualShadowMapMarkPagesBindings> {
public:
    VirtualShadowMapMarkPagesPass(
        std::shared_ptr<org::Buffer> tileWorkBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> markClipmapDataBuffer,
        std::shared_ptr<org::Buffer> markedBlocksMaskBuffer,
        std::shared_ptr<org::Buffer> markedBlocksListBuffer,
        std::shared_ptr<org::Buffer> markedBlocksCountBuffer,
        std::shared_ptr<org::Buffer> receiverSubpageMaskBuffer);

    VirtualShadowMapMarkPagesBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    VirtualShadowMarkFrameData Prepare(const VirtualShadowMapMarkPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapMarkPagesBindings&,
        const VirtualShadowMarkFrameData&, org::PassRecordContext&);

private:

    org::PipelineState m_pso;
    org::PipelineState m_clearPso;
    org::PipelineState m_clearUint2Pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
    std::shared_ptr<org::Buffer> m_tileWorkBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_markClipmapDataBuffer;
    std::shared_ptr<org::Buffer> m_markedBlocksMaskBuffer;
    std::shared_ptr<org::Buffer> m_markedBlocksListBuffer;
    std::shared_ptr<org::Buffer> m_markedBlocksCountBuffer;
    std::shared_ptr<org::Buffer> m_receiverSubpageMaskBuffer;
    uint32_t m_activeClipmapCount = 0u;
    uint32_t m_receiverSubpageMode = 0u;
};
