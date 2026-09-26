#pragma once

#include <memory>

#include "Pipeline/PipelineState/PSOManager.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include <array>
#include <vector>

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITEarlyDepthFrameData {
    bool enabled = false;
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    org::PreparedProgramReference program;
    org::PreparedDescriptorReference depth;
    org::PreparedResourceReference arguments, count;
    rhi::CommandSignatureHandle signature{};
    rhi::ClearValue clear{};
    uint32_t width = 0, height = 0, maximumCount = 0;
    std::vector<unsigned int> descriptorIndices;
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
};

struct AVBOITEarlyDepthBindings {
    org::ResourceBindingToken config, arguments, count, depth;
};

class AVBOITEarlyDepthPass final : public org::TypedRenderGraphPass<AVBOITEarlyDepthPass,
    AVBOITEarlyDepthFrameData, AVBOITEarlyDepthBindings> {
public:
    AVBOITEarlyDepthPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> tileCommandsBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer,
        std::shared_ptr<org::PixelBuffer> earlyDepthTexture);

    AVBOITEarlyDepthBindings Declare(org::PassBuilder& builder);
    AVBOITEarlyDepthFrameData Prepare(const AVBOITEarlyDepthBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITEarlyDepthBindings&,
        const AVBOITEarlyDepthFrameData&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_tileCommandsBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
    std::shared_ptr<org::PixelBuffer> m_earlyDepthTexture;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
