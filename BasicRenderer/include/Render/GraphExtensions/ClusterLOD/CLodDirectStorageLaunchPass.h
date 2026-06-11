#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "RenderPasses/Base/CopyPass.h"
#include "Interfaces/IResourceResolver.h"

struct CLodDirectStorageLaunchInputs {
	std::unique_ptr<IResourceResolver> targetSlabResolver;
	std::function<PassReturn()> launchCallback;
};

class CLodDirectStorageLaunchPass : public CopyPass {
public:
	explicit CLodDirectStorageLaunchPass(CLodDirectStorageLaunchInputs inputs)
		: m_inputs(std::move(inputs)) {}

	void Setup() override {}
	void Cleanup() override {}

	PassReturn Execute(PassExecutionContext&) override {
		return m_inputs.launchCallback ? m_inputs.launchCallback() : PassReturn{};
	}

private:
	void DeclareResourceUsages(CopyPassBuilder* builder) override {
		if (m_inputs.targetSlabResolver) {
			builder->WithCopyDest(*m_inputs.targetSlabResolver);
		}
		builder->PreferQueue(QueueKind::Graphics);
	}

	CLodDirectStorageLaunchInputs m_inputs;
};
