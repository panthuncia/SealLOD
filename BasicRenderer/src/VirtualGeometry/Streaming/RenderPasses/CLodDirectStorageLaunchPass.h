#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "Interfaces/IResourceResolver.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"

struct CLodDirectStorageLaunchInputs {
    std::unique_ptr<org::IResourceResolver> targetSlabResolver;
    std::function<std::shared_ptr<const org::PreparedLifecycleEffect>()> reserveLaunch;
};

class CLodDirectStorageLaunchPass : public org::TypedRenderGraphPass<CLodDirectStorageLaunchPass> {
public:
    explicit CLodDirectStorageLaunchPass(CLodDirectStorageLaunchInputs inputs)
        : m_inputs(std::move(inputs)) {}
    void Declare(org::PassBuilder& builder) {
        if (m_inputs.targetSlabResolver) builder.WithCopyDest(*m_inputs.targetSlabResolver);
        builder.PreferQueue(org::QueueKind::Graphics);
    }
    org::EmptyPassFrameData Prepare(const org::PassPrepareContext& preparation) {
        if (m_inputs.reserveLaunch) {
            if (auto reservation = m_inputs.reserveLaunch()) preparation.Reserve(std::move(reservation));
        }
        return {};
    }
    static void Record(const org::EmptyPassFrameData&, org::PassRecordContext&) {}
private:
    CLodDirectStorageLaunchInputs m_inputs;
};
