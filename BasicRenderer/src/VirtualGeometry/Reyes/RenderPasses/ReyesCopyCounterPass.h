#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"

namespace org { class Buffer; }

struct ReyesCounterCopyBindings {
    org::ResourceBindingToken source;
    org::ResourceBindingToken destination;
};

class ReyesCopyCounterPass final
    : public org::TypedRenderGraphPass<ReyesCopyCounterPass,
        org::EmptyPassFrameData, ReyesCounterCopyBindings> {
public:
    ReyesCopyCounterPass(std::shared_ptr<org::Buffer> sourceCounterBuffer, std::shared_ptr<org::Buffer> destCounterBuffer)
        : m_sourceCounterBuffer(std::move(sourceCounterBuffer))
        , m_destCounterBuffer(std::move(destCounterBuffer))
    {
    }

    ReyesCounterCopyBindings Declare(org::PassBuilder& builder)
    {
        builder.PreferQueue(org::QueueKind::Copy);
        return {builder.BindCopySource(m_sourceCounterBuffer),
            builder.BindCopyDestination(m_destCounterBuffer)};
    }

    static void Record(const ReyesCounterCopyBindings& data,
        org::PassRecordContext& recording)
    {
        recording.Commands().CopyBufferRegion(
            recording.Resolve(data.destination).GetHandle(), 0,
            recording.Resolve(data.source).GetHandle(), 0, sizeof(uint32_t));
    }

private:
    std::shared_ptr<org::Buffer> m_sourceCounterBuffer;
    std::shared_ptr<org::Buffer> m_destCounterBuffer;
};
