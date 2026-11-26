#pragma once

#include <functional>
#include "Render/PassBuilders.h"
#include <memory>
#include <utility>
#include <vector>

#include "Interfaces/IResourceResolver.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"

struct CLodDirectStorageLaunchInputs {
    std::unique_ptr<org::IResourceResolver> targetSlabResolver;
    std::function<std::shared_ptr<const org::PreparedLifecycleEffect>()> reserveLaunch;
    // Whether reserveLaunch would reserve a launch now. A reservation is a lifecycle effect, which a kept recording cannot
    // make: while one is wanted the pass's revision moves, so Shading prepares again and reserves it (M6 step 5).
    std::function<bool()> launchWanted;
};

class CLodDirectStorageLaunchPass : public org::TypedRenderGraphPass<CLodDirectStorageLaunchPass> {
public:
    explicit CLodDirectStorageLaunchPass(CLodDirectStorageLaunchInputs inputs)
        : m_inputs(std::move(inputs)) {}
    void Declare(org::PassBuilder& builder) {
        if (m_inputs.targetSlabResolver) builder.CopyDestination(*m_inputs.targetSlabResolver);
        builder.PreferQueue(org::QueueKind::Graphics);
    }
    org::EmptyPassFrameData Prepare(const org::PassPrepareContext& preparation) {
        if (m_inputs.reserveLaunch) {
            if (auto reservation = m_inputs.reserveLaunch()) preparation.Reserve(std::move(reservation));
        }
        return {};
    }
    void Revision(std::vector<uint64_t>& out) const { out.push_back(m_inputs.launchWanted && m_inputs.launchWanted() ? 1u : 0u); }
    static void Record(const org::EmptyPassFrameData&, org::PassRecordContext&) {}
private:
    CLodDirectStorageLaunchInputs m_inputs;
};
