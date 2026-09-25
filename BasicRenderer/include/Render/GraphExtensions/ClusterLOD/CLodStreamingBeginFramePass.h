#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class UploadInstance; }
struct UpdateContext;

struct CLodStreamingBeginFrameBindings {
    org::ResourceBindingToken loadCounter, loadRequestKeys, usedGroupsCounter, sourceMismatchCounter;
    bool hasSourceMismatchCounter = false;
};

class CLodStreamingBeginFramePass : public org::TypedRenderGraphPass<CLodStreamingBeginFramePass,
    br::render::PreparedComputeDispatchSequence, CLodStreamingBeginFrameBindings> {
public:
    CLodStreamingBeginFramePass(
        std::function<org::UploadInstance*()> getUploadInstance,
        std::shared_ptr<org::Buffer> loadCounter,
        std::shared_ptr<org::Buffer> loadRequestKeys,
        std::shared_ptr<org::Buffer> usedGroupsCounter,
        std::shared_ptr<org::Buffer> sourceGroupMismatchCounter,
        std::shared_ptr<org::Buffer> runtimeState,
        std::function<bool(std::vector<uint32_t>&, uint32_t&, org::UploadInstance*)> queueNonResidentBitsUpload,
        std::function<uint32_t(const UpdateContext&)> getActiveGroupScanCount,
        std::function<void()> scheduleStreamingReadbacks,
        std::function<void()> processStreamingRequests);

    CLodStreamingBeginFrameBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatchSequence Prepare(const CLodStreamingBeginFrameBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const CLodStreamingBeginFrameBindings&,
        const br::render::PreparedComputeDispatchSequence&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    std::shared_ptr<org::Buffer> m_loadCounter;
    std::shared_ptr<org::Buffer> m_loadRequestKeys;
    std::shared_ptr<org::Buffer> m_usedGroupsCounter;
    std::shared_ptr<org::Buffer> m_sourceGroupMismatchCounter;
    std::shared_ptr<org::Buffer> m_runtimeState;
    std::function<bool(std::vector<uint32_t>&, uint32_t&, org::UploadInstance*)> m_queueNonResidentBitsUpload;
    std::function<uint32_t(const UpdateContext&)> m_getActiveGroupScanCount;
    std::function<void()> m_scheduleStreamingReadbacks;
    std::function<void()> m_processStreamingRequests;
    std::function<org::UploadInstance*()> m_getUploadInstance;
    std::vector<uint32_t> m_nonResidentBitsUploadScratch;
    org::PipelineState m_clearUintPipeline;
};
