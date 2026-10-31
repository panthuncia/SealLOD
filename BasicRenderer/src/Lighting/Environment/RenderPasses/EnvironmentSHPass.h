#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/Runtime/IDescriptorService.h"
#include "Utilities/Utilities.h"
#include "../../../../generated/BuiltinResources.h"

#include <algorithm>
#include <vector>

struct EnvironmentSHBindings {
	org::PipelineState pipeline;
	uint32_t samplerIndex = 0;
	// The jobs this declaration declares, and each one's source cubemap view, in the same order. Executions take work
	// only from them (PreparesOnlyDeclaredWork).
	std::shared_ptr<const br::render::EnvironmentSHWorkQueue::Snapshot> declared;
	std::vector<org::DeclaredViewToken> sources;
};
struct EnvironmentSHFrame {
	struct Job { uint32_t cubemapResolution = 0, environmentIndex = 0, declared = 0; };
	std::vector<Job> jobs;
};

class EnvironmentSHPass : public org::TypedRenderGraphPass<EnvironmentSHPass, EnvironmentSHFrame, EnvironmentSHBindings>, public org::IDynamicDeclaredResources {
public:
	// Runs in the environment job graph (EnvironmentJobGraph), which has no resource providers: it takes its work
	// queue and the environment info buffer directly.
	EnvironmentSHPass(br::render::EnvironmentSHWorkQueue work, std::shared_ptr<org::Resource> environmentInfo)
		: m_work(std::move(work)), m_environmentInfo(std::move(environmentInfo)) {}

	void Initialize() {
		rhi::SamplerDesc shSamplerDesc = {};
		shSamplerDesc.minFilter = rhi::Filter::Linear;
		shSamplerDesc.magFilter = rhi::Filter::Linear;
		shSamplerDesc.mipFilter = rhi::MipFilter::Nearest;
		shSamplerDesc.addressU = rhi::AddressMode::Clamp;
		shSamplerDesc.addressV = rhi::AddressMode::Clamp;
		shSamplerDesc.addressW = rhi::AddressMode::Clamp;
		shSamplerDesc.mipLodBias = 0.0f;
		shSamplerDesc.maxAnisotropy = 1;
		shSamplerDesc.borderPreset = rhi::BorderPreset::TransparentBlack;
		shSamplerDesc.minLod = 0.0f;
		shSamplerDesc.maxLod = (std::numeric_limits<float>::max)();

		m_samplerIndex = DescriptorService().CreateIndexedSampler(shSamplerDesc);

		CreatePSO();
	}

	~EnvironmentSHPass() {
	}

	EnvironmentSHBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
		EnvironmentSHBindings bindings{m_PSO, m_samplerIndex};
		br::render::EnvironmentSHWorkQueue::Snapshot declared;
		for (const auto& j : m_pending) {
			if (!j->work.srcCubemap) continue;
			bindings.sources.push_back(builder.ShaderResource(j->work.srcCubemap).View());
			declared.push_back(j);
		}
		// The shader finds the buffer by its builtin name.
		builder.NameUse(Builtin::Environment::InfoBuffer, builder.UnorderedAccess(m_environmentInfo));

		m_declaredResourcesChanged = false;
		bindings.declared = std::make_shared<const br::render::EnvironmentSHWorkQueue::Snapshot>(std::move(declared));
		return bindings;
	}



    void Update(const org::UpdateExecutionContext&) override {
        auto pending = m_work.Pending();
        if (pending != m_pending) {
            m_pending = std::move(pending);
            m_declaredResourcesChanged = true;
        }
    }

    // Takes only jobs the bindings declare: while a new declaration builds, executions still get the old one. Taking a
    // job leaves the queue's pending work smaller, so the next Update redeclares without it.
    EnvironmentSHFrame Prepare(const EnvironmentSHBindings& bindings, const org::PassPrepareContext& preparation) {
        EnvironmentSHFrame frame;
        if (!bindings.declared) return frame;
        for (const auto& entry : m_work.ReserveAvailable(*bindings.declared, preparation))
            frame.jobs.push_back({entry->work.cubemapResolution, entry->work.environmentIndex, static_cast<uint32_t>(
                std::find(bindings.declared->begin(), bindings.declared->end(), entry) - bindings.declared->begin())});
        return frame;
    }

    static void Record(const EnvironmentSHBindings& bindings, const EnvironmentSHFrame& frame, org::PassRecordContext& context) {
        if (frame.jobs.empty()) return;
        context.BindComputePipeline(bindings.pipeline);
        for (const auto& job : frame.jobs) {
            br::render::MiscConstants constants{};
            constants[UintRootConstant0] = job.cubemapResolution;
            constants[UintRootConstant1] = bindings.samplerIndex;
            constants[UintRootConstant2] = job.environmentIndex;
            constants[UintRootConstant3] = context.Index(bindings.sources[job.declared]);
            br::render::PushMiscConstants(context, constants);
            const uint32_t groups = (job.cubemapResolution + 15) / 16;
            context.Commands().Dispatch(groups, groups, 6);
        }
    }

	bool DeclaredResourcesChanged() const override {
		return m_declaredResourcesChanged;
	}
	bool PreparesOnlyDeclaredWork() const noexcept override { return true; }



private:
    br::render::EnvironmentSHWorkQueue m_work;
    std::shared_ptr<org::Resource> m_environmentInfo;
    br::render::EnvironmentSHWorkQueue::Snapshot m_pending;
	bool m_declaredResourcesChanged = true;

	void CreatePSO() {
		m_PSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
			L"shaders/SphericalHarmonics.hlsl",
			L"CSMain",
			{},
			"Environment Spherical Harmonics CS");
	}

	unsigned int m_samplerIndex = 0;
	org::PipelineState m_PSO;
};
