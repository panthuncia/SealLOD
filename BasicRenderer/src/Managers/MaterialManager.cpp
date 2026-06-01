#include "Managers/MaterialManager.h"
#include "../generated/BuiltinResources.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RasterBucketFlags.h"
#include "Render/Runtime/IReadbackService.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	constexpr uint64_t kTextureStreamingIdleFramesBeforeCoarsen = 180u;
	constexpr std::string_view kTextureStreamingFeedbackReadbackAnchorPass = "MenuRenderPass";

	uint64_t ComputeTextureResidentBytes(const TextureDescription& desc) {
		uint64_t totalBytes = 0;
		for (const ImageDimensions& dims : desc.imageDimensions) {
			totalBytes += dims.slicePitch;
		}
		return totalBytes;
	}

	TextureStreamingGPUInfo BuildTextureStreamingGPUInfo(const TextureAsset& texture) {
		const TextureStreamingState& state = texture.GetStreamingState();
		TextureStreamingGPUInfo info = {};
		if (state.eligible) {
			info.flags |= kTextureStreamingFlagEligible;
		}
		if (state.enabled) {
			info.flags |= kTextureStreamingFlagEnabled;
		}
		info.totalMipCount = state.residency.totalMipCount;
		info.residentTopMip = state.residency.residentTopMip;
		info.residentMipCount = state.residency.residentMipCount;
		info.fullWidth = texture.GetFullMip0Width();
		info.fullHeight = texture.GetFullMip0Height();
		info.requestedTopMip = state.requestedTopMip;
		info.pendingTopMip = state.pendingTopMip;
		info.bindingRevisionLo = static_cast<uint32_t>(state.bindingRevision & 0xffffffffull);
		info.bindingRevisionHi = static_cast<uint32_t>(state.bindingRevision >> 32u);
		return info;
	}

	PerMaterialOpenPBRCB BuildOpenPBRMaterialData(const Material& material) {
		const OpenPBRMaterialParameters& materialParameters = material.GetOpenPBRMaterial();
		const OpenPBRTextureBindings& textures = material.GetOpenPBRTextures();
		constexpr uint32_t kInvalidDescriptor = std::numeric_limits<uint32_t>::max();
		constexpr uint32_t kInvalidStreamingTextureID = 0u;
		PerMaterialOpenPBRCB result = {};
		result.baseWeight = materialParameters.baseWeight;
		result.baseColor = materialParameters.baseColor;
		result.baseDiffuseRoughness = materialParameters.baseDiffuseRoughness;
		result.baseMetalness = materialParameters.baseMetalness;
		result.subsurfaceWeight = materialParameters.subsurfaceWeight;
		result.subsurfaceRadius = materialParameters.subsurfaceRadius;
		result.subsurfaceColor = materialParameters.subsurfaceColor;
		result.subsurfaceScatterAnisotropy = materialParameters.subsurfaceScatterAnisotropy;
		result.subsurfaceRadiusScale = materialParameters.subsurfaceRadiusScale;
		result.specularWeight = materialParameters.specularWeight;
		result.specularColor = materialParameters.specularColor;
		result.specularRoughness = materialParameters.specularRoughness;
		result.specularRoughnessAnisotropy = materialParameters.specularRoughnessAnisotropy;
		result.specularIor = materialParameters.specularIor;
		result.specularAnisotropyRotationCosSin = materialParameters.specularAnisotropyRotationCosSin;
		result.coatWeight = materialParameters.coatWeight;
		result.coatColor = materialParameters.coatColor;
		result.coatRoughness = materialParameters.coatRoughness;
		result.coatRoughnessAnisotropy = materialParameters.coatRoughnessAnisotropy;
		result.coatIor = materialParameters.coatIor;
		result.coatDarkening = materialParameters.coatDarkening;
		result.coatAnisotropyRotationCosSin = materialParameters.coatAnisotropyRotationCosSin;
		result.fuzzWeight = materialParameters.fuzzWeight;
		result.fuzzColor = materialParameters.fuzzColor;
		result.fuzzRoughness = materialParameters.fuzzRoughness;
		result.transmissionWeight = materialParameters.transmissionWeight;
		result.transmissionColor = materialParameters.transmissionColor;
		result.transmissionDepth = materialParameters.transmissionDepth;
		result.transmissionScatter = materialParameters.transmissionScatter;
		result.transmissionScatterAnisotropy = materialParameters.transmissionScatterAnisotropy;
		result.transmissionDispersionScale = materialParameters.transmissionDispersionScale;
		result.transmissionDispersionAbbeNumber = materialParameters.transmissionDispersionAbbeNumber;
		result.thinFilmWeight = materialParameters.thinFilmWeight;
		result.thinFilmThickness = materialParameters.thinFilmThickness;
		result.thinFilmIor = materialParameters.thinFilmIor;
		result.emissionLuminance = materialParameters.emissionLuminance;
		result.emissionColor = materialParameters.emissionColor;
		result.geometryOpacity = materialParameters.geometryOpacity;
		result.geometryThinWalled = materialParameters.geometryThinWalled ? 1u : 0u;

		auto initializeColorTextureMetadata = [&](const TextureAndConstant& binding,
			uint32_t& textureIndex,
			uint32_t& samplerIndex,
			DirectX::XMUINT4& channels,
			uint32_t& uvSetIndex,
			uint32_t& streamingTextureID) {
			textureIndex = kInvalidDescriptor;
			samplerIndex = kInvalidDescriptor;
			uvSetIndex = binding.uvSetIndex;
			streamingTextureID = kInvalidStreamingTextureID;
			channels = DirectX::XMUINT4(0u, 1u, 2u, 3u);

			if (binding.texture == nullptr) {
				return;
			}

			textureIndex = binding.texture->Image().GetSRVInfo(0).slot.index;
			samplerIndex = binding.texture->SamplerDescriptorIndex();
			streamingTextureID = IsMaterialTextureStreamingEnabledSetting() ? binding.texture->GetStreamingTextureID() : kInvalidStreamingTextureID;
			if (binding.channels.size() > 0u) channels.x = binding.channels[0];
			if (binding.channels.size() > 1u) channels.y = binding.channels[1];
			if (binding.channels.size() > 2u) channels.z = binding.channels[2];
			if (binding.channels.size() > 3u) channels.w = binding.channels[3];
		};

		auto initializeScalarTextureMetadata = [&](const TextureAndConstant& binding,
			uint32_t& textureIndex,
			uint32_t& samplerIndex,
			uint32_t& channel,
			uint32_t& uvSetIndex,
			uint32_t& streamingTextureID) {
			textureIndex = kInvalidDescriptor;
			samplerIndex = kInvalidDescriptor;
			channel = 0u;
			uvSetIndex = binding.uvSetIndex;
			streamingTextureID = kInvalidStreamingTextureID;

			if (binding.texture == nullptr) {
				return;
			}

			textureIndex = binding.texture->Image().GetSRVInfo(0).slot.index;
			samplerIndex = binding.texture->SamplerDescriptorIndex();
			streamingTextureID = IsMaterialTextureStreamingEnabledSetting() ? binding.texture->GetStreamingTextureID() : kInvalidStreamingTextureID;
			if (!binding.channels.empty()) {
				channel = binding.channels[0];
			}
		};

		initializeColorTextureMetadata(textures.coatColor,
			result.coatColorTextureIndex,
			result.coatColorSamplerIndex,
			result.coatColorChannels,
			result.coatColorUvSetIndex,
			result.coatColorStreamingTextureID);
		initializeScalarTextureMetadata(textures.coatWeight,
			result.coatWeightTextureIndex,
			result.coatWeightSamplerIndex,
			result.coatWeightChannel,
			result.coatWeightUvSetIndex,
			result.coatWeightStreamingTextureID);
		initializeScalarTextureMetadata(textures.coatRoughness,
			result.coatRoughnessTextureIndex,
			result.coatRoughnessSamplerIndex,
			result.coatRoughnessChannel,
			result.coatRoughnessUvSetIndex,
			result.coatRoughnessStreamingTextureID);
		initializeColorTextureMetadata(textures.fuzzColor,
			result.fuzzColorTextureIndex,
			result.fuzzColorSamplerIndex,
			result.fuzzColorChannels,
			result.fuzzColorUvSetIndex,
			result.fuzzColorStreamingTextureID);
		initializeScalarTextureMetadata(textures.fuzzWeight,
			result.fuzzWeightTextureIndex,
			result.fuzzWeightSamplerIndex,
			result.fuzzWeightChannel,
			result.fuzzWeightUvSetIndex,
			result.fuzzWeightStreamingTextureID);
		initializeScalarTextureMetadata(textures.fuzzRoughness,
			result.fuzzRoughnessTextureIndex,
			result.fuzzRoughnessSamplerIndex,
			result.fuzzRoughnessChannel,
			result.fuzzRoughnessUvSetIndex,
			result.fuzzRoughnessStreamingTextureID);
		return result;
	}

	PerMaterialEvalCB BuildMaterialEvalData(const Material& material) {
		const PerMaterialCB& base = material.GetData();
		PerMaterialEvalCB result = {};
		result.materialFlags = base.materialFlags;
		result.baseColorTextureIndex = base.baseColorTextureIndex;
		result.baseColorSamplerIndex = base.baseColorSamplerIndex;
		result.normalTextureIndex = base.normalTextureIndex;
		result.normalSamplerIndex = base.normalSamplerIndex;
		result.metallicTextureIndex = base.metallicTextureIndex;
		result.metallicSamplerIndex = base.metallicSamplerIndex;
		result.roughnessTextureIndex = base.roughnessTextureIndex;
		result.roughnessSamplerIndex = base.roughnessSamplerIndex;
		result.emissiveTextureIndex = base.emissiveTextureIndex;
		result.emissiveSamplerIndex = base.emissiveSamplerIndex;
		result.aoMapIndex = base.aoMapIndex;
		result.aoSamplerIndex = base.aoSamplerIndex;
		result.heightMapIndex = base.heightMapIndex;
		result.heightSamplerIndex = base.heightSamplerIndex;
		result.opacityTextureIndex = base.opacityTextureIndex;
		result.opacitySamplerIndex = base.opacitySamplerIndex;
		result.metallicFactor = base.metallicFactor;
		result.roughnessFactor = base.roughnessFactor;
		result.heightMapScale = base.heightMapScale;
		result.alphaCutoff = base.alphaCutoff;
		result.geometricDisplacementMin = base.geometricDisplacementMin;
		result.geometricDisplacementMax = base.geometricDisplacementMax;
		result.geometricDisplacementEnabled = base.geometricDisplacementEnabled;
		result.baseColorFactor = base.baseColorFactor;
		result.emissiveFactor = base.emissiveFactor;
		result.baseColorChannels = base.baseColorChannels;
		result.normalChannels = base.normalChannels;
		result.aoChannel = base.aoChannel;
		result.heightChannel = base.heightChannel;
		result.metallicChannel = base.metallicChannel;
		result.roughnessChannel = base.roughnessChannel;
		result.emissiveChannels = base.emissiveChannels;
		result.openPBRMaterialDataIndex = base.openPBRMaterialDataIndex;
		result.baseColorUvSetIndex = base.baseColorUvSetIndex;
		result.normalUvSetIndex = base.normalUvSetIndex;
		result.metallicUvSetIndex = base.metallicUvSetIndex;
		result.roughnessUvSetIndex = base.roughnessUvSetIndex;
		result.emissiveUvSetIndex = base.emissiveUvSetIndex;
		result.aoUvSetIndex = base.aoUvSetIndex;
		result.heightUvSetIndex = base.heightUvSetIndex;
		result.opacityUvSetIndex = base.opacityUvSetIndex;
		result.baseColorStreamingTextureID = base.baseColorStreamingTextureID;
		result.normalStreamingTextureID = base.normalStreamingTextureID;
		result.metallicStreamingTextureID = base.metallicStreamingTextureID;
		result.roughnessStreamingTextureID = base.roughnessStreamingTextureID;
		result.emissiveStreamingTextureID = base.emissiveStreamingTextureID;
		result.aoStreamingTextureID = base.aoStreamingTextureID;
		result.heightStreamingTextureID = base.heightStreamingTextureID;
		result.opacityStreamingTextureID = base.opacityStreamingTextureID;
		return result;
	}

	std::vector<std::shared_ptr<Resource>> CollectMaterialTextureResources(const Material& material) {
		std::vector<std::shared_ptr<Resource>> textures;
		std::unordered_set<uint64_t> seenResourceIds;

		material.ForEachReferencedTexture([&](const std::shared_ptr<TextureAsset>& texture) {
			std::shared_ptr<Resource> image = texture ? texture->ImagePtr() : nullptr;
			if (!image) {
				return;
			}

			if (seenResourceIds.insert(image->GetGlobalResourceID()).second) {
				textures.push_back(std::move(image));
			}
		});

		return textures;
	}

	std::vector<std::shared_ptr<TextureAsset>> CollectMaterialTextureAssets(const Material& material) {
		std::vector<std::shared_ptr<TextureAsset>> textures;
		std::unordered_set<uint32_t> seenStreamingIds;
		material.ForEachReferencedTexture([&](const std::shared_ptr<TextureAsset>& texture) {
			if (!texture) {
				return;
			}

			const uint32_t streamingTextureID = texture->GetStreamingTextureID();
			if (streamingTextureID != 0u && !seenStreamingIds.insert(streamingTextureID).second) {
				return;
			}

			textures.push_back(texture);
		});
		return textures;
	}

	template <typename T>
	bool BytewiseEqual(const T& lhs, const T& rhs) {
		return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
	}
}

// TODO: Use LazyDynamicStructuredBuffer and active indices buffer like draw calls? Would reduce number of no-op indirect arguments
MaterialManager::MaterialManager() {
	auto& rm = ResourceManager::GetInstance();
	m_activeMaterialTextureGroup = std::make_shared<ResourceGroup>("ActiveMaterialTextures");

	// Primary material data buffer. Normally streamed scenes should reserve enough
	// slots to avoid reallocating GPU backing resources while frames are executing.
	// The forced-resize path is intentionally left on while validating upload and
	// descriptor lifetime safety under worst-case cell-streaming pressure.
	m_materialBufferCapacity = kForceMaterialBufferResizeEveryMaterial
		? m_compileFlagsSlotsUsed
		: kInitialMaterialBufferCapacity;
	m_perMaterialDataBuffer = DynamicStructuredBuffer<PerMaterialCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialDataBuffer", true);
	m_perMaterialEvalDataBuffer = DynamicStructuredBuffer<PerMaterialEvalCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialEvalDataBuffer", true);
	m_perMaterialOpenPBRDataBuffer = DynamicStructuredBuffer<PerMaterialOpenPBRCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialOpenPBRDataBuffer", true);
	m_textureStreamingMetadataBuffer = DynamicStructuredBuffer<TextureStreamingGPUInfo>::CreateShared(m_textureStreamingMetadataCapacity, "Builtin::Material::TextureStreamingMetadataBuffer", true);
	m_textureStreamingFeedbackBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_textureStreamingMetadataCapacity, "Builtin::Material::TextureStreamingFeedbackBuffer", true);
	rg::memory::SetResourceUsageHint(*m_perMaterialDataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_perMaterialEvalDataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_perMaterialOpenPBRDataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_textureStreamingMetadataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_textureStreamingFeedbackBuffer, "Material buffers");
	m_textureStreamingMetadataBuffer->UpdateAt(0u, TextureStreamingGPUInfo{});
	m_textureStreamingFeedbackBuffer->UpdateAt(0u, kTextureStreamingFeedbackUnused);

	// Visibility buffer resources
    m_materialPixelCountBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsSlotsUsed, "VisUtil::MaterialPixelCountBuffer", true);
    m_materialOffsetBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsSlotsUsed, "VisUtil::MaterialOffsetBuffer", true);
	m_materialWriteCursorBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsSlotsUsed, "VisUtil::MaterialWriteCursorBuffer", true);
	rg::memory::SetResourceUsageHint(*m_materialPixelCountBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_materialOffsetBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_materialWriteCursorBuffer, "Material evaluation buffers");

	// Per-block arrays for hierarchical scan
	const uint32_t numBlocks = (m_compileFlagsSlotsUsed + kScanBlockSize - 1u) / kScanBlockSize;
	m_blockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::BlockSumsBuffer", true);
	m_scannedBlockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::ScannedBlockSumsBuffer", true);
	rg::memory::SetResourceUsageHint(*m_blockSumsBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_scannedBlockSumsBuffer, "Material evaluation buffers");

	// Indirect command buffer for material evaluation
	m_materialEvaluationCommandBuffer = DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>::CreateShared(m_compileFlagsSlotsUsed, "IndirectCommandBuffers::MaterialEvaluationCommandBuffer", true);
	rg::memory::SetResourceUsageHint(*m_materialEvaluationCommandBuffer, "Indirect command buffers");

	m_resources["Builtin::VisUtil::MaterialPixelCountBuffer"] = m_materialPixelCountBuffer;
	m_resources["Builtin::VisUtil::MaterialOffsetBuffer"] = m_materialOffsetBuffer;
	m_resources["Builtin::VisUtil::MaterialWriteCursorBuffer"] = m_materialWriteCursorBuffer;
	m_resources["Builtin::VisUtil::BlockSumsBuffer"] = m_blockSumsBuffer;
	m_resources["Builtin::VisUtil::ScannedBlockSumsBuffer"] = m_scannedBlockSumsBuffer;
	m_resources["Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer"] = m_materialEvaluationCommandBuffer;
	m_resources[Builtin::PerMaterialDataBuffer] = m_perMaterialDataBuffer;
	m_resources["Builtin::PerMaterialEvalDataBuffer"] = m_perMaterialEvalDataBuffer;
	m_resources[Builtin::PerMaterialOpenPBRDataBuffer] = m_perMaterialOpenPBRDataBuffer;
	m_resources[Builtin::Material::TextureStreamingMetadataBuffer] = m_textureStreamingMetadataBuffer;
	m_resources[Builtin::Material::TextureStreamingFeedbackBuffer] = m_textureStreamingFeedbackBuffer;
	m_resolvers[Builtin::Material::TextureGroup] = std::make_shared<ResourceGroupResolver>(m_activeMaterialTextureGroup);

	// Reserve built-in material bins up front so render-graph material evaluation buffers are
	// fully sized before passes/materialization/upload steps touch them.
	GetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel);
}

void MaterialManager::BeginTextureStreamingFeedbackFrame(uint64_t frameIndex) {
	if (!IsMaterialTextureStreamingEnabledSetting()) {
		return;
	}

	std::vector<std::pair<uint32_t, uint32_t>> pendingFeedback;
	{
		std::lock_guard lock(m_textureStreamingFeedbackMutex);
		pendingFeedback.swap(m_pendingTextureStreamingFeedback);
	}

	std::vector<uint32_t> expiredTextureIDs;
	for (const auto& [streamingTextureID, requestedTopMip] : pendingFeedback) {
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			expiredTextureIDs.push_back(streamingTextureID);
			continue;
		}

		if (!texture->IsMipStreamingEnabled()) {
			continue;
		}

		const uint64_t previousRevision = texture->GetStreamingStateRevision();
		const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(requestedTopMip, frameIndex);
		if (texture->GetStreamingStateRevision() != previousRevision) {
			MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "feedback");
		}
	}

	for (uint32_t streamingTextureID : expiredTextureIDs) {
		m_streamingTexturesByID.erase(streamingTextureID);
	}

	for (auto it = m_streamingTexturesByID.begin(); it != m_streamingTexturesByID.end();) {
		auto texture = it->second.lock();
		if (!texture) {
			it = m_streamingTexturesByID.erase(it);
			continue;
		}

		if (!texture->IsMipStreamingEnabled()) {
			++it;
			continue;
		}

		const TextureStreamingState& state = texture->GetStreamingState();
		if (state.lastSeenFrame == 0u || frameIndex <= state.lastSeenFrame + kTextureStreamingIdleFramesBeforeCoarsen) {
			++it;
			continue;
		}

		const uint32_t coarsenedTopMip = (std::min)(
			state.residency.totalMipCount - 1u,
			(std::max)(state.requestedTopMip, state.residency.residentTopMip + 1u));
		if (coarsenedTopMip != state.requestedTopMip) {
			const uint64_t previousRevision = texture->GetStreamingStateRevision();
			const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(coarsenedTopMip, frameIndex, true);
			if (texture->GetStreamingStateRevision() != previousRevision) {
				MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "idle_coarsen");
			}
		}

		++it;
	}

	for (const uint32_t streamingTextureID : m_activeTextureStreamingFeedbackIDs) {
		if (streamingTextureID < m_textureStreamingMetadataCapacity) {
			m_textureStreamingFeedbackBuffer->UpdateAt(streamingTextureID, kTextureStreamingFeedbackUnused);
		}
	}
}
void MaterialManager::RequestTextureStreamingFeedbackReadback(rg::runtime::IReadbackService* readbackService) {
	if (!IsMaterialTextureStreamingEnabledSetting() ||
		!readbackService ||
		!m_textureStreamingFeedbackBuffer ||
		m_activeTextureStreamingFeedbackIDs.empty()) {
		return;
	}

	std::vector<uint32_t> activeStreamingTextureIDs = m_activeTextureStreamingFeedbackIDs;
	readbackService->RequestReadbackCapture(
		std::string(kTextureStreamingFeedbackReadbackAnchorPass),
		m_textureStreamingFeedbackBuffer.get(),
		RangeSpec{},
		[this, activeStreamingTextureIDs = std::move(activeStreamingTextureIDs)](ReadbackCaptureResult&& result) {
			if (result.desc.kind != ReadbackResourceKind::Buffer || result.data.empty()) {
				return;
			}

			TaskSchedulerManager::GetInstance().RunBackgroundTask(
				"MaterialManager::DecodeTextureStreamingFeedback",
				[this,
				 activeStreamingTextureIDs = std::move(activeStreamingTextureIDs),
				 resultData = std::move(result.data)]() mutable {
					std::vector<std::pair<uint32_t, uint32_t>> decodedFeedback;
					decodedFeedback.reserve(activeStreamingTextureIDs.size());
					const size_t wordCount = resultData.size() / sizeof(uint32_t);
					for (uint32_t streamingTextureID : activeStreamingTextureIDs) {
						if (streamingTextureID >= wordCount) {
							continue;
						}

						uint32_t requestedTopMip = kTextureStreamingFeedbackUnused;
						std::memcpy(
							&requestedTopMip,
							resultData.data() + static_cast<size_t>(streamingTextureID) * sizeof(uint32_t),
							sizeof(uint32_t));
						if (requestedTopMip == kTextureStreamingFeedbackUnused) {
							continue;
						}

						decodedFeedback.emplace_back(streamingTextureID, requestedTopMip);
					}

					if (decodedFeedback.empty()) {
						return;
					}

					std::lock_guard lock(m_textureStreamingFeedbackMutex);
					m_pendingTextureStreamingFeedback.insert(
						m_pendingTextureStreamingFeedback.end(),
						decodedFeedback.begin(),
						decodedFeedback.end());
				});
		},
		QueueKind::Copy);
}

MaterialTextureStreamingStats MaterialManager::GetMaterialTextureStreamingStats() const {
	MaterialTextureStreamingStats stats{};
	std::unordered_set<uint64_t> seenImageResourceIDs;

	for (const auto& [_, textures] : m_trackedMaterialTextures) {
		for (const auto& textureResource : textures) {
			if (!textureResource) {
				continue;
			}

			auto image = std::dynamic_pointer_cast<PixelBuffer>(textureResource);
			if (!image) {
				continue;
			}

			const uint64_t imageResourceID = image->GetGlobalResourceID();
			if (!seenImageResourceIDs.insert(imageResourceID).second) {
				continue;
			}

			stats.uniqueMaterialTextureCount++;
			stats.totalResidentBytes += ComputeTextureResidentBytes(image->GetDescription());

			uint32_t residentTopMip = 0u;
			auto textureIt = m_materialTextureAssetsByImageResourceID.find(imageResourceID);
			if (textureIt != m_materialTextureAssetsByImageResourceID.end()) {
				auto texture = textureIt->second.lock();
				if (!texture) {
					continue;
				}

				const TextureStreamingState& streamingState = texture->GetStreamingState();
				residentTopMip = streamingState.residency.residentTopMip;
				if (streamingState.eligible) {
					stats.uniqueStreamableTextureCount++;
					stats.streamableResidentBytes += ComputeTextureResidentBytes(image->GetDescription());
					if (streamingState.enabled) {
						stats.uniqueStreamingEnabledTextureCount++;
					}
					if (streamingState.residency.residentTopMip == 0u) {
						stats.streamableFullResolutionResidentTextureCount++;
					}
					if (streamingState.requestedTopMip != streamingState.residency.residentTopMip ||
						streamingState.pendingTopMip != streamingState.residency.residentTopMip) {
						stats.pendingReloadTextureCount++;
					}
				}
			}

			if (residentTopMip == 0u) {
				stats.fullResolutionResidentTextureCount++;
			}

			if (stats.residentTopMipHistogram.size() <= residentTopMip) {
				stats.residentTopMipHistogram.resize(static_cast<size_t>(residentTopMip) + 1u, 0u);
			}
			stats.residentTopMipHistogram[residentTopMip]++;
		}
	}

	return stats;
}

void MaterialManager::MarkMaterialDirty(Material& material) {
	const uint32_t materialID = material.GetMaterialID();
	if (m_dirtyMaterialIDSet.insert(materialID).second) {
		m_dirtyMaterialIDs.push_back(materialID);
	}
}

void MaterialManager::MarkTextureStreamingMetadataDirty(const std::shared_ptr<TextureAsset>& texture, bool needsUploadAdvance, const char* reason) {
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	m_streamingTexturesByID[streamingTextureID] = texture;
	if (needsUploadAdvance && m_texturesNeedingUploadAdvanceSet.insert(streamingTextureID).second) {
		m_texturesNeedingUploadAdvance.push_back(streamingTextureID);
	}
	if (IsMaterialTextureStreamingEnabledSetting() && m_dirtyTextureStreamingIDSet.insert(streamingTextureID).second) {
		m_dirtyTextureStreamingIDs.push_back(streamingTextureID);
	}

	RecordTextureDirtyReason(reason);
}

void MaterialManager::RecordTextureDirtyReason(const char* reason) {
	if (std::strcmp(reason, "feedback") == 0) {
		++m_textureDirtyReasonFeedback;
	} else if (std::strcmp(reason, "idle_coarsen") == 0) {
		++m_textureDirtyReasonIdleCoarsen;
	} else if (std::strcmp(reason, "track_material") == 0) {
		++m_textureDirtyReasonTrackMaterial;
	} else if (std::strcmp(reason, "upload_state_revision") == 0) {
		++m_textureDirtyReasonUploadStateRevision;
	} else if (std::strcmp(reason, "upload_pending") == 0) {
		++m_textureDirtyReasonUploadPending;
	} else {
		++m_textureDirtyReasonOther;
	}
}

void MaterialManager::ProcessPendingMaterialUpdates(uint64_t frameIndex, TextureFactory& textureFactory) {
	const auto updateStart = std::chrono::steady_clock::now();
	const auto feedbackStart = std::chrono::steady_clock::now();
	BeginTextureStreamingFeedbackFrame(frameIndex);
	const auto feedbackEnd = std::chrono::steady_clock::now();

	std::vector<uint32_t> texturesToAdvance;
	texturesToAdvance.swap(m_texturesNeedingUploadAdvance);
	m_texturesNeedingUploadAdvanceSet.clear();
	const auto uploadStart = std::chrono::steady_clock::now();
	std::size_t uploadAdvanceVisited = 0;
	std::size_t uploadAdvanceAlive = 0;
	std::size_t uploadAdvanceStillPending = 0;
	std::size_t uploadAdvanceBindingChanged = 0;
	std::size_t uploadAdvanceStateChanged = 0;
	std::size_t pendingNoUsableImage = 0;
	std::size_t pendingPlaceholder = 0;
	std::size_t pendingStreamingReload = 0;
	std::size_t pendingProcessingHandle = 0;
	std::size_t pendingReloadHandle = 0;
	std::size_t pendingDirectStorageHandle = 0;
	struct PendingTextureSample {
		uint32_t id = 0;
		uint32_t requestedTopMip = 0;
		uint32_t pendingTopMip = 0;
		uint32_t residentTopMip = 0;
		uint32_t directStorageTargetTopMip = 0;
		uint32_t residentMipCount = 0;
		uint32_t totalMipCount = 0;
		uint64_t stateRevision = 0;
		uint64_t bindingRevision = 0;
		const char* processingState = "None";
		const char* reloadState = "None";
		const char* directStorageState = "None";
		const char* loadPath = "unknown";
		const char* uploadPath = "unknown";
		bool cacheArtifact = false;
		std::string label;
		std::string name;
		std::string sourceIdentity;
		std::string filePath;
		std::string initialData;
	};
	std::vector<PendingTextureSample> pendingSamples;
	pendingSamples.reserve(8);
	for (const uint32_t streamingTextureID : texturesToAdvance) {
		++uploadAdvanceVisited;
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			m_streamingTexturesByID.erase(it);
			continue;
		}

		++uploadAdvanceAlive;
		const uint64_t previousBindingRevision = texture->GetBindingRevision();
		const uint64_t previousStreamingRevision = texture->GetStreamingStateRevision();
		EnsureTextureUploadAdvanced(texture, textureFactory);
		if (texture->GetBindingRevision() != previousBindingRevision) {
			++uploadAdvanceBindingChanged;
		}
		if (texture->GetStreamingStateRevision() != previousStreamingRevision) {
			++uploadAdvanceStateChanged;
		}
		if (texture->HasPendingUploadWork()) {
			++uploadAdvanceStillPending;
			const auto pending = texture->GetPendingDebugInfo();
			if (!pending.hasUsableImage) {
				++pendingNoUsableImage;
			}
			if (pending.hasPlaceholder) {
				++pendingPlaceholder;
			}
			if (pending.needsStreamingReload) {
				++pendingStreamingReload;
			}
			if (pending.hasProcessingHandle) {
				++pendingProcessingHandle;
			}
			if (pending.hasReloadHandle) {
				++pendingReloadHandle;
			}
			if (pending.hasDirectStorageHandle) {
				++pendingDirectStorageHandle;
			}
			if (pendingSamples.size() < 8) {
				pendingSamples.push_back(PendingTextureSample{
					.id = pending.streamingTextureID,
					.requestedTopMip = pending.requestedTopMip,
					.pendingTopMip = pending.pendingTopMip,
					.residentTopMip = pending.residentTopMip,
					.directStorageTargetTopMip = pending.directStorageTargetTopMip,
					.residentMipCount = pending.residentMipCount,
					.totalMipCount = pending.totalMipCount,
					.stateRevision = pending.stateRevision,
					.bindingRevision = pending.bindingRevision,
					.processingState = pending.processingState,
					.reloadState = pending.reloadState,
					.directStorageState = pending.directStorageState,
					.loadPath = pending.loadPath,
					.uploadPath = pending.uploadPath,
					.cacheArtifact = pending.isProcessingCacheArtifact,
					.label = pending.label,
					.name = pending.debugName,
					.sourceIdentity = pending.sourceIdentity,
					.filePath = pending.filePath,
					.initialData = pending.initialData
				});
			}
		}
	}
	const auto uploadEnd = std::chrono::steady_clock::now();

	std::vector<uint32_t> dirtyMaterialIDs;
	dirtyMaterialIDs.swap(m_dirtyMaterialIDs);
	m_dirtyMaterialIDSet.clear();
	const auto dirtyMaterialStart = std::chrono::steady_clock::now();
	std::size_t dirtyMaterialsVisited = 0;
	std::size_t dirtyMaterialsFlushed = 0;
	for (const uint32_t materialID : dirtyMaterialIDs) {
		++dirtyMaterialsVisited;
		auto materialIt = m_activeMaterialsByID.find(materialID);
		if (materialIt == m_activeMaterialsByID.end() || materialIt->second == nullptr) {
			continue;
		}

		FlushDirtyMaterial(*materialIt->second, &textureFactory);
		++dirtyMaterialsFlushed;
	}
	const auto dirtyMaterialEnd = std::chrono::steady_clock::now();

	std::vector<uint32_t> dirtyTextureIDs;
	dirtyTextureIDs.swap(m_dirtyTextureStreamingIDs);
	m_dirtyTextureStreamingIDSet.clear();
	const auto dirtyTextureStart = std::chrono::steady_clock::now();
	std::size_t dirtyTextureMetadataVisited = 0;
	std::size_t dirtyTextureMetadataAlive = 0;
	std::size_t dirtyTextureMetadataUpdated = 0;
	for (const uint32_t streamingTextureID : dirtyTextureIDs) {
		++dirtyTextureMetadataVisited;
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			m_streamingTexturesByID.erase(it);
			m_textureStreamingMetadataRevisions.erase(streamingTextureID);
			continue;
		}

		++dirtyTextureMetadataAlive;
		const auto previousUploadedRevision = m_textureStreamingMetadataRevisions.find(streamingTextureID);
		const uint64_t previousRevision = previousUploadedRevision != m_textureStreamingMetadataRevisions.end()
			? previousUploadedRevision->second
			: std::numeric_limits<uint64_t>::max();
		FlushDirtyTextureMetadata(texture);
		if (m_textureStreamingMetadataRevisions[streamingTextureID] != previousRevision) {
			++dirtyTextureMetadataUpdated;
		}
	}
	const auto dirtyTextureEnd = std::chrono::steady_clock::now();

	const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - updateStart).count();
	const auto feedbackUs = std::chrono::duration_cast<std::chrono::microseconds>(feedbackEnd - feedbackStart).count();
	const auto uploadUs = std::chrono::duration_cast<std::chrono::microseconds>(uploadEnd - uploadStart).count();
	const auto dirtyMaterialUs = std::chrono::duration_cast<std::chrono::microseconds>(dirtyMaterialEnd - dirtyMaterialStart).count();
	const auto dirtyTextureUs = std::chrono::duration_cast<std::chrono::microseconds>(dirtyTextureEnd - dirtyTextureStart).count();
	const auto now = std::chrono::steady_clock::now();
	const bool hadWork =
		uploadAdvanceVisited != 0 ||
		dirtyMaterialsVisited != 0 ||
		dirtyTextureMetadataVisited != 0 ||
		elapsedUs >= 2000;
	if (hadWork && now - m_lastMaterialUpdateStatsLog >= std::chrono::seconds(1)) {
		m_lastMaterialUpdateStatsLog = now;
		spdlog::info(
			"MaterialManager::ProcessPendingMaterialUpdates stats: elapsed_us={} feedback_us={} upload_us={} dirtyMaterial_us={} dirtyTexture_us={} uploadAdvance visited={} alive={} stillPending={} bindingChanged={} stateChanged={} pending(noImage={} placeholder={} streamingReload={} processing={} reload={} directStorage={}) dirtyMaterials visited={} flushed={} dirtyTextureMetadata visited={} alive={} updated={} activeMaterials={} activeStreamingTextures={} dirtyReasons(feedback={} idle={} track={} uploadState={} uploadPending={} other={})",
			elapsedUs,
			feedbackUs,
			uploadUs,
			dirtyMaterialUs,
			dirtyTextureUs,
			uploadAdvanceVisited,
			uploadAdvanceAlive,
			uploadAdvanceStillPending,
			uploadAdvanceBindingChanged,
			uploadAdvanceStateChanged,
			pendingNoUsableImage,
			pendingPlaceholder,
			pendingStreamingReload,
			pendingProcessingHandle,
			pendingReloadHandle,
			pendingDirectStorageHandle,
			dirtyMaterialsVisited,
			dirtyMaterialsFlushed,
			dirtyTextureMetadataVisited,
			dirtyTextureMetadataAlive,
			dirtyTextureMetadataUpdated,
			m_activeMaterialsByID.size(),
			m_streamingTexturesByID.size(),
			m_textureDirtyReasonFeedback,
			m_textureDirtyReasonIdleCoarsen,
			m_textureDirtyReasonTrackMaterial,
			m_textureDirtyReasonUploadStateRevision,
			m_textureDirtyReasonUploadPending,
			m_textureDirtyReasonOther);
		for (const auto& sample : pendingSamples) {
			spdlog::info(
				"MaterialManager::ProcessPendingMaterialUpdates pendingTexture: id={} label='{}' name='{}' source='{}' file='{}' initial='{}' cacheArtifact={} loadPath={} uploadPath={} requestedTopMip={} pendingTopMip={} residentTopMip={} directStorageTargetTopMip={} residentMipCount={} totalMipCount={} stateRevision={} bindingRevision={} processing={} reload={} directStorage={}",
				sample.id,
				sample.label,
				sample.name,
				sample.sourceIdentity,
				sample.filePath,
				sample.initialData,
				sample.cacheArtifact,
				sample.loadPath,
				sample.uploadPath,
				sample.requestedTopMip,
				sample.pendingTopMip,
				sample.residentTopMip,
				sample.directStorageTargetTopMip,
				sample.residentMipCount,
				sample.totalMipCount,
				sample.stateRevision,
				sample.bindingRevision,
				sample.processingState,
				sample.reloadState,
				sample.directStorageState);
		}
	}
}

unsigned int MaterialManager::IncrementMaterialUsageCount(Material& material, TextureFactory* textureFactory, unsigned int count) {
	//std::lock_guard<std::mutex> lock(m_materialSlotMappingMutex);
	if (count == 0u) {
		return GetMaterialSlot(material.GetMaterialID());
	}

	auto& flags = material.Technique().compileFlags;
	unsigned int flagsSlot = GetCompileFlagsSlot(flags);
	m_compileFlagsUsageCounts[flagsSlot] += count;

	uint32_t materialID = material.GetMaterialID();
	auto existingSlotIt = m_materialIDSlotMapping.find(materialID);
	const bool alreadyResident =
		existingSlotIt != m_materialIDSlotMapping.end()
		&& existingSlotIt->second < m_materialUsageCounts.size()
		&& m_materialUsageCounts[existingSlotIt->second] > 0u;

	if (!alreadyResident && textureFactory) {
		material.EnsureTexturesUploaded(*textureFactory);
	}
	material.SetCompileFlagsID(flagsSlot);
	unsigned int materialSlot = alreadyResident
		? existingSlotIt->second
		: GetMaterialSlot(materialID, textureFactory ? std::optional<PerMaterialCB>{ material.GetData() } : std::nullopt);
	material.SetOpenPBRMaterialDataIndex(materialSlot);
	m_activeMaterialsByID[materialID] = &material;

	m_materialUsageCounts[materialSlot] += count;
	if (m_materialUsageCounts[materialSlot] == 1u) {
		if (textureFactory) {
			FlushDirtyMaterial(material, textureFactory);
		} else {
			UpdateMaterialTextureUsage(material, 1);
			TrackMaterialTextureAssets(material, 1);
			MarkMaterialDirty(material);
		}
	}
	return materialSlot;
}

void MaterialManager::UpdateMaterialDataBuffer(Material& material) {
	FlushDirtyMaterial(material);
}

void MaterialManager::FlushDirtyMaterial(Material& material, TextureFactory* textureFactory) {
	const unsigned int materialSlot = GetMaterialSlot(material.GetMaterialID());
	material.SetOpenPBRMaterialDataIndex(materialSlot);
	const bool refreshedTextures = textureFactory != nullptr;
	if (textureFactory) {
		material.EnsureTexturesUploaded(*textureFactory);
		TrackMaterialTextureAssets(material, -1);
		TrackMaterialTextureAssets(material, 1);
	}

	const PerMaterialCB materialData = material.GetData();
	const PerMaterialEvalCB evalData = BuildMaterialEvalData(material);
	const PerMaterialOpenPBRCB openPBRData = BuildOpenPBRMaterialData(material);
	if (materialSlot >= m_materialUploadSignatures.size()) {
		m_materialUploadSignatures.resize(static_cast<size_t>(materialSlot) + 1u);
	}

	auto& signature = m_materialUploadSignatures[materialSlot];
	const bool dataChanged =
		!signature.valid ||
		!BytewiseEqual(signature.materialData, materialData) ||
		!BytewiseEqual(signature.evalData, evalData) ||
		!BytewiseEqual(signature.openPBRData, openPBRData);
	if (dataChanged) {
		m_perMaterialDataBuffer->UpdateAt(materialSlot, materialData);
		m_perMaterialEvalDataBuffer->UpdateAt(materialSlot, evalData);
		m_perMaterialOpenPBRDataBuffer->UpdateAt(materialSlot, openPBRData);
		signature.materialData = materialData;
		signature.evalData = evalData;
		signature.openPBRData = openPBRData;
		signature.valid = true;
	}

	if (dataChanged || refreshedTextures) {
		RefreshMaterialTextureUsage(material);
	}
}

void MaterialManager::EnsureTextureUploadAdvanced(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory) {
	if (!texture) {
		return;
	}

	const uint64_t previousBindingRevision = texture->GetBindingRevision();
	const uint64_t previousStreamingRevision = texture->GetStreamingStateRevision();
	texture->SetGenerateMipmaps(true);
	texture->EnsureUploaded(textureFactory);

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (texture->GetStreamingStateRevision() != previousStreamingRevision) {
		MarkTextureStreamingMetadataDirty(texture, false, "upload_state_revision");
	}
	if (texture->HasPendingUploadWork()) {
		if (m_texturesNeedingUploadAdvanceSet.insert(streamingTextureID).second) {
			m_texturesNeedingUploadAdvance.push_back(streamingTextureID);
		}
		RecordTextureDirtyReason("upload_pending");
	}
	if (texture->GetBindingRevision() != previousBindingRevision) {
		auto materialIdsIt = m_streamingTextureMaterialIDs.find(streamingTextureID);
		if (materialIdsIt != m_streamingTextureMaterialIDs.end()) {
			for (const uint32_t materialID : materialIdsIt->second) {
				if (auto materialIt = m_activeMaterialsByID.find(materialID);
					materialIt != m_activeMaterialsByID.end() && materialIt->second) {
					MarkMaterialDirty(*materialIt->second);
				}
			}
		}
	}
}

void MaterialManager::FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture) {
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	const uint64_t revision = texture->GetStreamingStateRevision();
	auto revisionIt = m_textureStreamingMetadataRevisions.find(streamingTextureID);
	if (revisionIt != m_textureStreamingMetadataRevisions.end() && revisionIt->second == revision) {
		return;
	}

	UpdateTextureStreamingMetadata(texture);
	m_textureStreamingMetadataRevisions[streamingTextureID] = revision;
}

void MaterialManager::UpdateTextureStreamingMetadata(const Material& material) {
	std::unordered_set<uint32_t> updatedStreamingTextureIDs;
	material.ForEachReferencedTexture([&](const std::shared_ptr<TextureAsset>& texture) {
		if (!texture) {
			return;
		}

		const uint32_t streamingTextureID = texture->GetStreamingTextureID();
		if (streamingTextureID == 0u || !updatedStreamingTextureIDs.insert(streamingTextureID).second) {
			return;
		}

		UpdateTextureStreamingMetadata(texture);
	});
}

void MaterialManager::UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture) {
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	if (streamingTextureID >= m_textureStreamingMetadataCapacity) {
		uint32_t newCapacity = m_textureStreamingMetadataCapacity;
		while (streamingTextureID >= newCapacity) {
			newCapacity *= 2u;
		}
		m_textureStreamingMetadataBuffer->Resize(newCapacity);
		m_textureStreamingFeedbackBuffer->Resize(newCapacity);
		m_textureStreamingMetadataCapacity = newCapacity;
	}

	m_textureStreamingMetadataBuffer->UpdateAt(streamingTextureID, BuildTextureStreamingGPUInfo(*texture));
	m_textureStreamingFeedbackBuffer->UpdateAt(streamingTextureID, kTextureStreamingFeedbackUnused);
	if (auto image = texture->ImagePtr()) {
		m_materialTextureAssetsByImageResourceID[image->GetGlobalResourceID()] = texture;
	}
	m_streamingTexturesByID[streamingTextureID] = texture;
	if (m_activeTextureStreamingFeedbackIDSet.insert(streamingTextureID).second) {
		m_activeTextureStreamingFeedbackIDs.push_back(streamingTextureID);
	}
}

void MaterialManager::DecrementMaterialUsageCount(const Material& material) {
	//std::lock_guard<std::mutex> lock(m_materialSlotMappingMutex);
	auto& flags = material.Technique().compileFlags;
	unsigned int flagsSlot = GetCompileFlagsSlot(flags);
	m_compileFlagsUsageCounts[flagsSlot]--;
	if (m_compileFlagsUsageCounts[flagsSlot] == 0) {
		m_freeCompileFlagsSlots.push_back(flagsSlot);
		m_compileFlagsSlotMapping.erase(flags);
		m_activeCompileFlagsSlots.erase(std::remove(m_activeCompileFlagsSlots.begin(), m_activeCompileFlagsSlots.end(), flagsSlot), m_activeCompileFlagsSlots.end());
		m_activeCompileFlags.erase(std::remove(m_activeCompileFlags.begin(), m_activeCompileFlags.end(), flags), m_activeCompileFlags.end());
	}

	const uint32_t materialID = material.GetMaterialID();
	const unsigned int materialSlot = GetMaterialSlot(materialID);
	m_materialUsageCounts[materialSlot]--;
	if (m_materialUsageCounts[materialSlot] == 0) {
		UpdateMaterialTextureUsage(material, -1);
		TrackMaterialTextureAssets(material, -1);
		if (materialSlot < m_materialUploadSignatures.size()) {
			m_materialUploadSignatures[materialSlot].valid = false;
		}
		m_freeMaterialSlots.push_back(materialSlot);
		m_materialIDSlotMapping.erase(materialID);
		m_activeMaterialsByID.erase(materialID);
		m_dirtyMaterialIDSet.erase(materialID);
		std::erase(m_dirtyMaterialIDs, materialID);
	}
}

void MaterialManager::UpdateMaterialTextureUsage(const Material& material, int delta) {
	const uint32_t materialId = material.GetMaterialID();
	if (delta > 0) {
		auto textures = CollectMaterialTextureResources(material);
		m_trackedMaterialTextures[materialId] = textures;
		UpdateTrackedMaterialTextureRefs(textures, delta);
		return;
	}

	auto trackedIt = m_trackedMaterialTextures.find(materialId);
	if (trackedIt == m_trackedMaterialTextures.end()) {
		return;
	}

	UpdateTrackedMaterialTextureRefs(trackedIt->second, delta);
	m_trackedMaterialTextures.erase(trackedIt);
}

void MaterialManager::TrackMaterialTextureAssets(const Material& material, int delta) {
	const uint32_t materialID = material.GetMaterialID();
	if (delta > 0) {
		std::vector<uint32_t> streamingTextureIDs;
		for (const auto& texture : CollectMaterialTextureAssets(material)) {
			if (!texture) {
				continue;
			}

			const uint32_t streamingTextureID = texture->GetStreamingTextureID();
			if (streamingTextureID == 0u) {
				continue;
			}

			streamingTextureIDs.push_back(streamingTextureID);
			m_streamingTexturesByID[streamingTextureID] = texture;
			m_streamingTextureMaterialIDs[streamingTextureID].insert(materialID);
			MarkTextureStreamingMetadataDirty(texture, true, "track_material");
		}
		m_materialStreamingTextureIDs[materialID] = std::move(streamingTextureIDs);
		return;
	}

	auto trackedIt = m_materialStreamingTextureIDs.find(materialID);
	if (trackedIt == m_materialStreamingTextureIDs.end()) {
		return;
	}

	for (const uint32_t streamingTextureID : trackedIt->second) {
		auto materialIdsIt = m_streamingTextureMaterialIDs.find(streamingTextureID);
		if (materialIdsIt != m_streamingTextureMaterialIDs.end()) {
			materialIdsIt->second.erase(materialID);
			if (materialIdsIt->second.empty()) {
				m_streamingTextureMaterialIDs.erase(materialIdsIt);
				m_streamingTexturesByID.erase(streamingTextureID);
				m_textureStreamingMetadataRevisions.erase(streamingTextureID);
				m_dirtyTextureStreamingIDSet.erase(streamingTextureID);
				m_texturesNeedingUploadAdvanceSet.erase(streamingTextureID);
				m_activeTextureStreamingFeedbackIDSet.erase(streamingTextureID);
				std::erase(m_dirtyTextureStreamingIDs, streamingTextureID);
				std::erase(m_texturesNeedingUploadAdvance, streamingTextureID);
				std::erase(m_activeTextureStreamingFeedbackIDs, streamingTextureID);
			}
		}
	}

	m_materialStreamingTextureIDs.erase(trackedIt);
}

void MaterialManager::RefreshMaterialTextureUsage(const Material& material) {
	auto slotIt = m_materialIDSlotMapping.find(material.GetMaterialID());
	if (slotIt == m_materialIDSlotMapping.end() || slotIt->second >= m_materialUsageCounts.size()) {
		return;
	}

	if (m_materialUsageCounts[slotIt->second] == 0u) {
		return;
	}

	auto currentTextures = CollectMaterialTextureResources(material);
	auto& trackedTextures = m_trackedMaterialTextures[material.GetMaterialID()];

	std::unordered_set<uint64_t> currentIds;
	currentIds.reserve(currentTextures.size());
	for (const auto& texture : currentTextures) {
		if (texture) {
			currentIds.insert(texture->GetGlobalResourceID());
		}
	}

	std::unordered_set<uint64_t> trackedIds;
	trackedIds.reserve(trackedTextures.size());
	for (const auto& texture : trackedTextures) {
		if (texture) {
			trackedIds.insert(texture->GetGlobalResourceID());
		}
	}

	std::vector<std::shared_ptr<Resource>> removedTextures;
	for (const auto& texture : trackedTextures) {
		if (texture && !currentIds.contains(texture->GetGlobalResourceID())) {
			removedTextures.push_back(texture);
		}
	}

	std::vector<std::shared_ptr<Resource>> addedTextures;
	for (const auto& texture : currentTextures) {
		if (texture && !trackedIds.contains(texture->GetGlobalResourceID())) {
			addedTextures.push_back(texture);
		}
	}

	UpdateTrackedMaterialTextureRefs(removedTextures, -1);
	UpdateTrackedMaterialTextureRefs(addedTextures, 1);
	trackedTextures = std::move(currentTextures);
}

void MaterialManager::UpdateTrackedMaterialTextureRefs(const std::vector<std::shared_ptr<Resource>>& textures, int delta) {
	if (delta == 0) {
		return;
	}

	for (const auto& texture : textures) {
		if (!texture) {
			continue;
		}

		const uint64_t resourceId = texture->GetGlobalResourceID();
		if (delta > 0) {
			auto& usageCount = m_materialTextureUsageCounts[resourceId];
			usageCount += static_cast<uint32_t>(delta);
			m_activeMaterialTextureGroup->AddResource(texture);
			continue;
		}

		auto usageIt = m_materialTextureUsageCounts.find(resourceId);
		if (usageIt == m_materialTextureUsageCounts.end()) {
			continue;
		}

		const uint32_t releaseCount = static_cast<uint32_t>(-delta);
		if (usageIt->second <= releaseCount) {
			m_materialTextureUsageCounts.erase(usageIt);
			m_activeMaterialTextureGroup->RemoveResource(texture.get());
			continue;
		}

		usageIt->second -= releaseCount;
	}
}

std::shared_ptr<Resource> MaterialManager::ProvideResource(ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<ResourceIdentifier> MaterialManager::GetSupportedKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources) {
		keys.push_back(key);
	}
	return keys;
}

std::vector<ResourceIdentifier> MaterialManager::GetSupportedResolverKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resolvers.size());
	for (auto const& [key, _] : m_resolvers) {
		keys.push_back(key);
	}
	return keys;
}

std::shared_ptr<IResourceResolver> MaterialManager::ProvideResolver(ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it == m_resolvers.end()) {
		return nullptr;
	}
	return it->second;
}

// TODO: C++26 will allow optional references
unsigned int MaterialManager::GetMaterialSlot(unsigned int materialID, std::optional<PerMaterialCB> data) {
	unsigned int slot;
	auto it = m_materialIDSlotMapping.find(materialID);
	if (it != m_materialIDSlotMapping.end()) {
		slot = it->second;
		return slot;
	}
	if (!m_freeMaterialSlots.empty()) {
		slot = m_freeMaterialSlots.back();
		m_freeMaterialSlots.pop_back();
		if (slot >= m_materialUploadSignatures.size()) {
			m_materialUploadSignatures.resize(static_cast<size_t>(slot) + 1u);
		}
		m_materialUploadSignatures[slot].valid = false;
		if (data.has_value()) {
			m_perMaterialDataBuffer->UpdateAt(slot, data.value());
		}
		else {
			m_perMaterialDataBuffer->UpdateAt(slot, PerMaterialCB{});
		}
		m_perMaterialEvalDataBuffer->UpdateAt(slot, PerMaterialEvalCB{});
		m_perMaterialOpenPBRDataBuffer->UpdateAt(slot, PerMaterialOpenPBRCB{});
	}
	else {
		slot = m_materialSlotsUsed++;
		m_materialUsageCounts.push_back(0);
		if constexpr (kForceMaterialBufferResizeEveryMaterial) {
			m_perMaterialDataBuffer->Resize(m_materialSlotsUsed);
			m_perMaterialEvalDataBuffer->Resize(m_materialSlotsUsed);
			m_perMaterialOpenPBRDataBuffer->Resize(m_materialSlotsUsed);
			m_materialBufferCapacity = m_materialSlotsUsed;
		} else {
			EnsureMaterialBufferCapacity(m_materialSlotsUsed);
		}
		m_materialUploadSignatures.resize(m_materialSlotsUsed);
		m_materialUploadSignatures[slot].valid = false;
		if (data.has_value()) {
			m_perMaterialDataBuffer->UpdateAt(slot, data.value());
		}
		else {
			m_perMaterialDataBuffer->UpdateAt(slot, PerMaterialCB{});
		}
		m_perMaterialEvalDataBuffer->UpdateAt(slot, PerMaterialEvalCB{});
		m_perMaterialOpenPBRDataBuffer->UpdateAt(slot, PerMaterialOpenPBRCB{});
	}
	m_materialIDSlotMapping[materialID] = slot;
	return slot;
}

void MaterialManager::EnsureMaterialBufferCapacity(unsigned int requiredSlots) {
	if (requiredSlots <= m_materialBufferCapacity) {
		return;
	}

	unsigned int newCapacity = std::max(kInitialMaterialBufferCapacity, m_materialBufferCapacity);
	while (newCapacity < requiredSlots) {
		newCapacity *= 2u;
	}

	spdlog::info("MaterialManager: growing material buffers oldCapacity={} newCapacity={} requiredSlots={}",
		m_materialBufferCapacity,
		newCapacity,
		requiredSlots);

	m_perMaterialDataBuffer->Resize(newCapacity);
	m_perMaterialEvalDataBuffer->Resize(newCapacity);
	m_perMaterialOpenPBRDataBuffer->Resize(newCapacity);
	m_materialBufferCapacity = newCapacity;
}

unsigned int MaterialManager::GetCompileFlagsSlot(MaterialCompileFlags flags) {
	unsigned int slot;
	auto it = m_compileFlagsSlotMapping.find(flags);
	if (it != m_compileFlagsSlotMapping.end()) {
		slot = it->second;
		return slot;
	}
	if (!m_freeCompileFlagsSlots.empty()) {
		slot = m_freeCompileFlagsSlots.back();
		m_freeCompileFlagsSlots.pop_back();
	}
	else {
		slot = m_nextCompileFlagsSlot++;
		m_compileFlagsSlotsUsed++;
		m_compileFlagsUsageCounts.push_back(0);
		// Resize resources to accommodate new material slot
		m_materialPixelCountBuffer->Resize(m_compileFlagsSlotsUsed);
		m_materialOffsetBuffer->Resize(m_compileFlagsSlotsUsed);
		m_materialWriteCursorBuffer->Resize(m_compileFlagsSlotsUsed);
		m_materialEvaluationCommandBuffer->Resize(m_compileFlagsSlotsUsed);

		// Resize per-block buffers to match new block count
		const uint32_t numBlocks = (m_compileFlagsSlotsUsed + kScanBlockSize - 1u) / kScanBlockSize;
		m_blockSumsBuffer->Resize(std::max(1u, numBlocks));
		m_scannedBlockSumsBuffer->Resize(std::max(1u, numBlocks));		
	}
	m_compileFlagsSlotMapping[flags] = slot;
	if (std::find(m_activeCompileFlagsSlots.begin(), m_activeCompileFlagsSlots.end(), slot) == m_activeCompileFlagsSlots.end()) {
		m_activeCompileFlagsSlots.push_back(slot);
	}
	if (std::find(m_activeCompileFlags.begin(), m_activeCompileFlags.end(), flags) == m_activeCompileFlags.end()) {
		m_activeCompileFlags.push_back(flags);
	}
	return slot;
}

unsigned int MaterialManager::AcquireRasterBucket(MaterialRasterFlags rasterFlags, unsigned int count) {
	if (count == 0u) {
		return GetRasterBucketForFlags(rasterFlags);
	}

	unsigned int slot;
	auto it = m_rasterFlagToBucketMapping.find(static_cast<uint32_t>(rasterFlags));
	if (it != m_rasterFlagToBucketMapping.end()) {
		slot = it->second;
		m_rasterBucketUsageCounts[slot] += count;
		return slot;
	}
	if (!m_freeRasterBuckets.empty()) {
		slot = m_freeRasterBuckets.back();
		m_freeRasterBuckets.pop_back();
		m_bucketToRasterFlagMapping[slot] = rasterFlags;
		m_rasterBucketUsageCounts[slot] = count;
	}
	else {
		slot = m_rasterBucketsUsed++;
		m_bucketToRasterFlagMapping.push_back(rasterFlags);
		m_rasterBucketUsageCounts.push_back(count);
	}

	m_rasterFlagToBucketMapping[static_cast<uint32_t>(rasterFlags)] = slot;
	return slot;
}

void MaterialManager::ReleaseRasterBucket(MaterialRasterFlags rasterFlags) {
	const auto it = m_rasterFlagToBucketMapping.find(static_cast<uint32_t>(rasterFlags));
	if (it == m_rasterFlagToBucketMapping.end()) {
		spdlog::error("Raster flags not found in mapping during release!");
		return;
	}

	const unsigned int slot = it->second;
	if (slot >= m_rasterBucketUsageCounts.size() || m_rasterBucketUsageCounts[slot] == 0u) {
		spdlog::error("Raster bucket usage underflow for slot {}!", slot);
		return;
	}

	m_rasterBucketUsageCounts[slot]--;
	if (m_rasterBucketUsageCounts[slot] != 0u) {
		return;
	}

	m_rasterFlagToBucketMapping.erase(it);
	m_bucketToRasterFlagMapping[slot] = MaterialRasterFlagsNone;
	m_freeRasterBuckets.push_back(slot);

	while (m_rasterBucketsUsed > 0u) {
		const unsigned int tailSlot = m_rasterBucketsUsed - 1u;
		if (tailSlot >= m_rasterBucketUsageCounts.size() ||
			m_rasterBucketUsageCounts[tailSlot] != 0u ||
			m_bucketToRasterFlagMapping[tailSlot] != MaterialRasterFlagsNone) {
			break;
		}

		m_rasterBucketsUsed--;
		m_bucketToRasterFlagMapping.pop_back();
		m_rasterBucketUsageCounts.pop_back();
		m_freeRasterBuckets.erase(
			std::remove(m_freeRasterBuckets.begin(), m_freeRasterBuckets.end(), tailSlot),
			m_freeRasterBuckets.end());
	}
}
