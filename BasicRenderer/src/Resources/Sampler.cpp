#include "Resources/Sampler.h"
#include "Render/Runtime/IDescriptorService.h"

std::shared_ptr<org::Sampler> org::Sampler::m_defaultSampler = nullptr;
std::shared_ptr<org::Sampler> org::Sampler::m_defaultShadowSampler = nullptr;
std::unordered_map<rhi::SamplerDesc, std::shared_ptr<org::Sampler>, rhi::SamplerDescHash, rhi::SamplerDescEq> org::Sampler::m_samplerCache;

org::Sampler::Sampler(rhi::SamplerDesc samplerDesc)
	: m_index(0), m_hasDescriptorIndex(false), m_samplerDesc(samplerDesc) {}

std::shared_ptr<org::Sampler> org::Sampler::CreateSampler(rhi::SamplerDesc samplerDesc) {
	auto it = m_samplerCache.find(samplerDesc);
	if (it != m_samplerCache.end()) {
		return it->second;
	}
	auto sampler = std::shared_ptr<org::Sampler>(new org::Sampler(samplerDesc));
	m_samplerCache.emplace(samplerDesc, sampler);
	return sampler;
}

std::shared_ptr<org::Sampler> org::Sampler::CreateCpuOnlySampler(rhi::SamplerDesc samplerDesc) {
	return std::shared_ptr<org::Sampler>(new org::Sampler(samplerDesc));
}

UINT org::Sampler::GetDescriptorIndex(org::runtime::IDescriptorService& descriptorService) const {
	if (m_hasDescriptorIndex.load(std::memory_order_acquire) &&
		m_descriptorOwner.load(std::memory_order_acquire) == &descriptorService) {
		return m_index;
	}

	std::lock_guard<std::mutex> lock(m_descriptorMutex);
	if (!m_hasDescriptorIndex.load(std::memory_order_relaxed) ||
		m_descriptorOwner.load(std::memory_order_relaxed) != &descriptorService) {
		m_index = descriptorService.CreateIndexedSampler(m_samplerDesc);
		m_descriptorOwner.store(&descriptorService, std::memory_order_release);
		m_hasDescriptorIndex.store(true, std::memory_order_release);
	}
	return m_index;
}

std::shared_ptr<org::Sampler> org::Sampler::GetDefaultSampler() {
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

		// Headless import/preprocess tools still need material texture metadata,
		// but have no active GPU descriptor service. Keep the sampler description
		// CPU-only; GetDescriptorIndex(service) materializes it lazily if the asset is
		// subsequently used by a renderer with an active descriptor service.
		m_defaultSampler = org::Sampler::CreateSampler(samplerDesc);
	}
	return m_defaultSampler;
}

std::shared_ptr<org::Sampler> org::Sampler::GetDefaultShadowSampler() {
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

		m_defaultShadowSampler = org::Sampler::CreateSampler(samplerDesc);
	}
	return m_defaultShadowSampler;
}
