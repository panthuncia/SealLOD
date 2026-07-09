#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class UploadInstance;

class CLodStreamingBeginFramePass : public ComputePass {
public:
    CLodStreamingBeginFramePass(
        std::function<UploadInstance*()> getUploadInstance,
        std::shared_ptr<Buffer> loadCounter,
        std::shared_ptr<Buffer> loadRequestKeys,
        std::shared_ptr<Buffer> usedGroupsCounter,
        std::shared_ptr<Buffer> sourceGroupMismatchCounter,
        std::shared_ptr<Buffer> nonResidentBits,
        std::shared_ptr<Buffer> activeGroupsBits,
        std::shared_ptr<Buffer> runtimeState,
        std::function<bool(std::vector<uint32_t>&, uint32_t&, UploadInstance*)> queueNonResidentBitsUpload,
        std::function<bool(std::vector<uint32_t>&, uint32_t&)> getActiveGroupsBitsUpload,
        std::function<void()> scheduleStreamingReadbacks,
        std::function<void()> processStreamingRequests);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_loadCounter;
    std::shared_ptr<Buffer> m_loadRequestKeys;
    std::shared_ptr<Buffer> m_usedGroupsCounter;
    std::shared_ptr<Buffer> m_sourceGroupMismatchCounter;
    std::shared_ptr<Buffer> m_nonResidentBits;
    std::shared_ptr<Buffer> m_activeGroupsBits;
    std::shared_ptr<Buffer> m_runtimeState;
    std::function<bool(std::vector<uint32_t>&, uint32_t&, UploadInstance*)> m_queueNonResidentBitsUpload;
    std::function<bool(std::vector<uint32_t>&, uint32_t&)> m_getActiveGroupsBitsUpload;
    std::function<void()> m_scheduleStreamingReadbacks;
    std::function<void()> m_processStreamingRequests;
    std::function<UploadInstance*()> m_getUploadInstance;
    std::vector<uint32_t> m_activeGroupsBitsUploadScratch;
    std::vector<uint32_t> m_nonResidentBitsUploadScratch;
    PipelineState m_clearUintPipeline;
};
