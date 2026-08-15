#pragma once

#include <memory>
#include <vector>

#include "RenderPasses/Base/CopyPass.h"
#include "Interfaces/IResourceResolver.h"
#include "Managers/UploadInstance.h"

struct CLodAsyncUploadInputs {
    UploadInstance* uploadInstance = nullptr;
    // Optional resolver for additional copy-dest resources (e.g. page-pool slab group).
    std::unique_ptr<IResourceResolver> poolResolver;
};

inline org::Hash64 HashValue(const CLodAsyncUploadInputs& i) {
    return static_cast<org::Hash64>(reinterpret_cast<uintptr_t>(i.uploadInstance));
}

inline bool operator==(const CLodAsyncUploadInputs& a, const CLodAsyncUploadInputs& b) {
    return a.uploadInstance == b.uploadInstance;
}

// CopyPass that drains the CLod UploadInstance on a dedicated async copy queue.
class CLodAsyncUploadPass final : public CopyPass, public IHasImmediateModeCommands {
public:
    explicit CLodAsyncUploadPass(CLodAsyncUploadInputs inputs) {
        SetInputs(std::move(inputs));
    }

    void DeclareResourceUsages(CopyPassBuilder* builder) override {
        const auto& inputs = Inputs<CLodAsyncUploadInputs>();
        if (inputs.uploadInstance) {
            std::vector<std::shared_ptr<Resource>> dests;
            inputs.uploadInstance->CollectPendingDestinations(dests);
            for (auto& dst : dests) {
                builder->WithCopyDest(dst);
            }
        }
        if (inputs.poolResolver) {
            builder->WithCopyDest(*inputs.poolResolver);
        }
        builder->PreferQueue(QueueKind::Copy);
    }

    void Setup() override {}

    void RecordImmediateCommands(ImmediateExecutionContext& context) override {
        const auto& inputs = Inputs<CLodAsyncUploadInputs>();
        if (inputs.uploadInstance) {
            inputs.uploadInstance->ProcessUploads(
                static_cast<uint8_t>(context.frameIndex), context.list);
        }
    }

    PassReturn Execute(PassExecutionContext&) override { return {}; }

    void Cleanup() override {}
};
