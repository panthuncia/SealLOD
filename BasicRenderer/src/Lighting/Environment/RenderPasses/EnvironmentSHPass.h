#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/Runtime/IDescriptorService.h"
#include "Utilities/Utilities.h"

#include <vector>

struct EnvironmentSHBindings {
	org::PipelineState pipeline;
	uint32_t samplerIndex = 0;
};
struct EnvironmentSHFrame {
	struct Job { uint32_t cubemapResolution = 0, environmentIndex = 0; };
	std::vector<Job> jobs;
};

class EnvironmentSHPass : public org::TypedRenderGraphPass<EnvironmentSHPass, EnvironmentSHFrame, EnvironmentSHBindings>, public org::IDynamicDeclaredResources {
public:
	EnvironmentSHPass() = default;

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
		for (const auto& j : m_pending) {
			if (!j->work.srcCubemap) continue;
			builder.ShaderResource(j->work.srcCubemap);
		}

		builder.UnorderedAccess(Builtin::Environment::InfoBuffer);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);

		m_declaredResourcesChanged = false;
		return {m_PSO, m_samplerIndex};
	}



    void Update(const org::UpdateExecutionContext& context) override {
        const auto* input = context.hostData->Get<UpdateContext>();
        m_work = input->environmentWork.sphericalHarmonics;
        auto pending = m_work.Pending();
        if (pending != m_pending) {
            m_pending = std::move(pending);
            m_declaredResourcesChanged = true;
        }
    }

    EnvironmentSHFrame Prepare(const EnvironmentSHBindings&, const org::PassPrepareContext& preparation) {
        EnvironmentSHFrame frame;
        if (m_pending.empty()) return frame;
        for (const auto& entry : m_pending)
            frame.jobs.push_back({entry->work.cubemapResolution, entry->work.environmentIndex});
        m_work.Reserve(m_pending, preparation);
        m_pending.clear();
        m_declaredResourcesChanged = true;
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
            br::render::PushMiscConstants(context, constants);
            const uint32_t groups = (job.cubemapResolution + 15) / 16;
            context.Commands().Dispatch(groups, groups, 6);
        }
    }

	bool DeclaredResourcesChanged() const override {
		return m_declaredResourcesChanged;
	}



private:
    br::render::EnvironmentSHWorkQueue m_work;
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
