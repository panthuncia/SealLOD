#pragma once

#include <memory>
#include <optional>
#include <array>
#include <vector>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

    struct VirtualShadowMarkFrameData {
        rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
        org::PreparedProgramReference clearProgram, clearUint2Program, markProgram;
        rhi::CommandSignatureHandle commandSignature{};
        org::PreparedResourceReference indirectArguments;
        std::vector<unsigned int> clearIndices, clearUint2Indices, markIndices;
        std::array<unsigned int, NumMiscUintRootConstants> clearMask{}, clearReceiver{}, clearCount{}, mark{};
        std::array<org::PreparedResourceReference, 3> barrierResources{};
        uint32_t barrierCount = 2;
        uint32_t receiverGroups = 0;
        bool receiverUint2 = false;
    };

struct VirtualShadowMapMarkPagesBindings {
    org::ResourceBindingToken tileWork, tileCount, indirectArgs, clipmapData;
    org::ResourceBindingToken mask, list, count;
    std::optional<org::ResourceBindingToken> receiverMask;
    uint32_t activeClipmapCount = 0;
    uint32_t receiverSubpageMode = 0;
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
