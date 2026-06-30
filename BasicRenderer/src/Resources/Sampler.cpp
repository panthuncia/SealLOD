#include "Resources/Sampler.h"
#include "Render/Runtime/DescriptorServiceAccess.h"

std::shared_ptr<Sampler> Sampler::m_defaultSampler = nullptr;
std::shared_ptr<Sampler> Sampler::m_defaultShadowSampler = nullptr;
std::unordered_map<rhi::SamplerDesc, std::shared_ptr<Sampler>, rhi::SamplerDescHash, rhi::SamplerDescEq> Sampler::m_samplerCache;

Sampler::Sampler(rhi::SamplerDesc samplerDesc, bool createDescriptor)
	: m_index(0), m_hasDescriptorIndex(false), m_samplerDesc(samplerDesc) {
	if (createDescriptor) {
		m_index = rg::runtime::CreateIndexedSamplerFromActiveDescriptorService(m_samplerDesc);
		m_hasDescriptorIndex = true;
	}
}

std::shared_ptr<Sampler> Sampler::CreateSampler(rhi::SamplerDesc samplerDesc) {
	auto it = m_samplerCache.find(samplerDesc);
	if (it != m_samplerCache.end()) {
		return it->second;
	}
	return std::shared_ptr<Sampler>(new Sampler(samplerDesc, true));
}

std::shared_ptr<Sampler> Sampler::CreateCpuOnlySampler(rhi::SamplerDesc samplerDesc) {
	return std::shared_ptr<Sampler>(new Sampler(samplerDesc, false));
}

bool Sampler::CanCreateDescriptorSamplers() {
	return rg::runtime::GetActiveDescriptorService() != nullptr;
}

UINT Sampler::GetDescriptorIndex() const {
	if (m_hasDescriptorIndex.load(std::memory_order_acquire)) {
		return m_index;
	}

	std::lock_guard<std::mutex> lock(m_descriptorMutex);
	if (!m_hasDescriptorIndex.load(std::memory_order_relaxed)) {
		m_index = rg::runtime::CreateIndexedSamplerFromActiveDescriptorService(m_samplerDesc);
		m_hasDescriptorIndex.store(true, std::memory_order_release);
	}
	return m_index;
}

std::shared_ptr<Sampler> Sampler::GetDefaultSampler() {
	if (m_defaultSampler == nullptr) {
		rhi::SamplerDesc samplerDesc = {};
		samplerDesc.minFilter = rhi::Filter::Linear;
		samplerDesc.magFilter = rhi::Filter::Linear;
		samplerDesc.mipFilter = rhi::MipFilter::Linear;
		samplerDesc.addressU = rhi::AddressMode::Wrap;
		samplerDesc.addressV = rhi::AddressMode::Wrap;
		samplerDesc.addressW = rhi::AddressMode::Wrap;
		samplerDesc.mipLodBias = 0.0f;
		samplerDesc.minLod = 0.0f;
		samplerDesc.maxLod = (std::numeric_limits<float>::max)();
		samplerDesc.maxAnisotropy = 1;
		samplerDesc.compareEnable = false;
		samplerDesc.compareOp = rhi::CompareOp::Always;
		samplerDesc.reduction = rhi::ReductionMode::Standard;
		samplerDesc.borderPreset = rhi::BorderPreset::TransparentBlack;

		m_defaultSampler = Sampler::CreateSampler(samplerDesc);
	}
	return m_defaultSampler;
}

std::shared_ptr<Sampler> Sampler::GetDefaultShadowSampler() {
	if (m_defaultShadowSampler == nullptr) {
		rhi::SamplerDesc samplerDesc = {};
		samplerDesc.minFilter = rhi::Filter::Linear;
		samplerDesc.magFilter = rhi::Filter::Linear;
		samplerDesc.mipFilter = rhi::MipFilter::Linear;
		samplerDesc.addressU = rhi::AddressMode::Border;
		samplerDesc.addressV = rhi::AddressMode::Border;
		samplerDesc.addressW = rhi::AddressMode::Border;
		samplerDesc.mipLodBias = 0.0f;
		samplerDesc.minLod = 0.0f;
		samplerDesc.maxLod = (std::numeric_limits<float>::max)();
		samplerDesc.maxAnisotropy = 1;
		samplerDesc.compareEnable = true;
		samplerDesc.compareOp = rhi::CompareOp::LessEqual;
		samplerDesc.reduction = rhi::ReductionMode::Comparison;
		samplerDesc.borderPreset = rhi::BorderPreset::OpaqueWhite;

		m_defaultShadowSampler = Sampler::CreateSampler(samplerDesc);
	}
	return m_defaultShadowSampler;
}
