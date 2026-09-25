#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/EnvironmentManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/Runtime/IDescriptorService.h"
#include "Utilities/Utilities.h"

#include <vector>

class EnvironmentSHPass : public org::TypedRenderGraphPass<EnvironmentSHPass, br::render::PreparedComputeDispatchSequence>, public org::IDynamicDeclaredResources {
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

	void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
		for (const auto& j : m_pending) {
			if (!j->work.srcCubemap) continue;
			builder.WithShaderResource(j->work.srcCubemap);
		}

		builder.WithUnorderedAccess(Builtin::Environment::InfoBuffer);
		builder.WithConstantBuffer(Builtin::PerFrameBuffer);

		m_declaredResourcesChanged = false;
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

    br::render::PreparedComputeDispatchSequence Prepare(const org::PassPrepareContext& preparation) {
        br::render::PreparedComputeDispatchSequence data;
        if (m_pending.empty()) return data;
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_PSO);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        for (const auto& entry : m_pending) {
            const auto& job = entry->work;
            br::render::PreparedComputeDispatchSequence::Step step;
            step.constants[UintRootConstant0] = job.cubemapResolution;
            step.constants[UintRootConstant1] = m_samplerIndex;
            step.constants[UintRootConstant2] = job.environmentIndex;
            step.groupsX = step.groupsY = (job.cubemapResolution + 15) / 16;
            step.groupsZ = 6;
            data.steps.push_back(step);
        }
        m_work.Reserve(m_pending, preparation);
        m_pending.clear();
        m_declaredResourcesChanged = true;
        return data;
    }

    static void Record(const br::render::PreparedComputeDispatchSequence& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatchSequence(data, recording);
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
