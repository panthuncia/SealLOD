#pragma once

#include <utility>
#include <vector>

#include "RenderPasses/Base/CopyPass.h"
#include "Interfaces/IResourceResolver.h"

struct CLodDirectStorageCompletionWaitInputs {
	std::vector<ExternalTimelinePoint> waits;
	std::unique_ptr<IResourceResolver> targetSlabResolver;
};

class CLodDirectStorageCompletionWaitPass : public CopyPass {
public:
	explicit CLodDirectStorageCompletionWaitPass(CLodDirectStorageCompletionWaitInputs inputs)
		: m_inputs(std::move(inputs)) {}

	void Setup() override {}
	void Cleanup() override {}

private:
	void DeclareResourceUsages(CopyPassBuilder* builder) override {
		if (m_inputs.targetSlabResolver) {
			builder->WithCopyDest(*m_inputs.targetSlabResolver);
		}
		for (const auto& wait : m_inputs.waits) {
			builder->WithExternalWaitBeforeTransitions(wait.timeline, wait.value);
		}
		builder->PreferQueue(QueueKind::Graphics);
	}

	CLodDirectStorageCompletionWaitInputs m_inputs;
};
