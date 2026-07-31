#include "Managers/MaterialManager.h"
#include "../generated/BuiltinResources.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RasterBucketFlags.h"
#include "Render/Runtime/IReadbackService.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_set>

#include <tracy/Tracy.hpp>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	constexpr uint64_t kTextureStreamingIdleFramesBeforeCoarsen = 180u;
	constexpr std::string_view kTextureStreamingFeedbackReadbackAnchorPass = "MenuRenderPass";

	const std::string& MaterialTextureTraceFilter() {
		static const std::string filter = [] {
			char* value = nullptr;
			size_t valueLength = 0;
			if (_dupenv_s(&value, &valueLength, "SARP_MATERIAL_TEXTURE_TRACE_FILTER") != 0 ||
				value == nullptr) {
				std::free(value);
				return std::string{};
			}
			std::string result(value);
			std::free(value);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return result;
		}();
		return filter;
	}

	bool ShouldTraceMaterialTexture(std::string_view identifier) {
		const auto& filter = MaterialTextureTraceFilter();
		if (filter.empty() || identifier.empty()) {
			return false;
		}
		std::string normalized(identifier);
		std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		return normalized.find(filter) != std::string::npos;
	}

	const std::wstring& MaterialTextureReadbackPath() {
		static const std::wstring path = [] {
			wchar_t* value = nullptr;
			size_t valueLength = 0;
			if (_wdupenv_s(&value, &valueLength, L"SARP_MATERIAL_TEXTURE_READBACK_PATH") != 0 ||
				value == nullptr) {
				std::free(value);
				return std::wstring{};
			}
			std::wstring result(value);
			std::free(value);
			return result;
		}();
		return path;
	}

	const std::wstring& MaterialTextureLateReadbackPath() {
		static const std::wstring path = [] {
			wchar_t* value = nullptr;
			size_t valueLength = 0;
			if (_wdupenv_s(&value, &valueLength, L"SARP_MATERIAL_TEXTURE_LATE_READBACK_PATH") != 0 ||
				value == nullptr) {
				std::free(value);
				return std::wstring{};
			}
			std::wstring result(value);
			std::free(value);
			return result;
		}();
		return path;
	}

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

			auto image = binding.texture->ImagePtr();
			textureIndex = image ? image->GetSRVInfo(0).slot.index : kInvalidDescriptor;
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

			auto image = binding.texture->ImagePtr();
			textureIndex = image ? image->GetSRVInfo(0).slot.index : kInvalidDescriptor;
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
		result.terrainSetIndex = base.terrainSetIndex;
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
		result.reyesUvDensity = base.reyesUvDensity;
		result.objectSurfaceTexelDensity = base.objectSurfaceTexelDensity;
		result.objectSurfaceSamplingMode = base.objectSurfaceSamplingMode;
		result.glintParameters = base.glintParameters;
		result.glintEnabled = base.glintEnabled;
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
		const auto externallyStreamedHeightAtlas = material.IsObjectReyesAtlasHeightMaterial()
			? material.GetHeightMapTexture()
			: nullptr;
		material.ForEachReferencedTexture([&](const std::shared_ptr<TextureAsset>& texture) {
			if (!texture || texture == externallyStreamedHeightAtlas) {
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
		? m_compileFlagsRegistry.GetSlotsUsed()
		: kInitialMaterialBufferCapacity;
	m_perMaterialDataBuffer = DynamicStructuredBuffer<PerMaterialCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialDataBuffer", true);
	m_perMaterialEvalDataBuffer = DynamicStructuredBuffer<PerMaterialEvalCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialEvalDataBuffer", true);
	m_perMaterialOpenPBRDataBuffer = DynamicStructuredBuffer<PerMaterialOpenPBRCB>::CreateShared(m_materialBufferCapacity, "Builtin::PerMaterialOpenPBRDataBuffer", true);
	m_textureStreamingManager = TextureStreamingManager::CreateUnique();
	rg::memory::SetResourceUsageHint(*m_perMaterialDataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_perMaterialEvalDataBuffer, "Material buffers");
	rg::memory::SetResourceUsageHint(*m_perMaterialOpenPBRDataBuffer, "Material buffers");

	// Visibility buffer resources
    m_materialPixelCountBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialPixelCountBuffer", true);
    m_materialOffsetBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialOffsetBuffer", true);
	m_materialWriteCursorBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialWriteCursorBuffer", true);
	rg::memory::SetResourceUsageHint(*m_materialPixelCountBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_materialOffsetBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_materialWriteCursorBuffer, "Material evaluation buffers");

	// Per-block arrays for hierarchical scan
	const uint32_t numBlocks = (m_compileFlagsRegistry.GetSlotsUsed() + kScanBlockSize - 1u) / kScanBlockSize;
	m_blockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::BlockSumsBuffer", true);
	m_scannedBlockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::ScannedBlockSumsBuffer", true);
	rg::memory::SetResourceUsageHint(*m_blockSumsBuffer, "Material evaluation buffers");
	rg::memory::SetResourceUsageHint(*m_scannedBlockSumsBuffer, "Material evaluation buffers");

	// Indirect command buffer for material evaluation
	m_materialEvaluationCommandBuffer = DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "IndirectCommandBuffers::MaterialEvaluationCommandBuffer", true);
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
	m_resolvers[Builtin::Material::TextureGroup] = std::make_shared<ResourceGroupResolver>(m_activeMaterialTextureGroup);

	// Reserve built-in material bins up front so render-graph material evaluation buffers are
	// fully sized before passes/materialization/upload steps touch them.
	AcquireCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel);
	CommitGpuVisibleSnapshot();
}

void MaterialManager::BeginTextureStreamingFeedbackFrame(uint64_t frameIndex) {
	(void)frameIndex;
}
void MaterialManager::InitializeTextureStreaming(TextureFactory& textureFactory, uint32_t framesInFlight) {
	if (m_textureStreamingManager) {
		m_textureStreamingManager->Initialize(textureFactory, framesInFlight);
	}
}
void MaterialManager::ShutdownTextureStreaming() {
	if (m_textureStreamingManager) {
		m_textureStreamingManager->Shutdown();
	}
}
std::shared_ptr<CopyPass> MaterialManager::CreateTextureStreamingFeedbackReadbackPass() {
	if (!m_textureStreamingManager || m_textureStreamingFeedbackSuppressed) {
		return {};
	}
	return m_textureStreamingManager->CreateTextureStreamingFeedbackReadbackPass();
}

MaterialTextureStreamingStats MaterialManager::GetMaterialTextureStreamingStats() const {
	return m_textureStreamingManager
		? m_textureStreamingManager->GetTextureStreamingStats(CollectActiveMaterialTextureResources())
		: MaterialTextureStreamingStats{};
}

void MaterialManager::MarkMaterialDirty(Material& material) {
	const uint32_t materialID = material.GetMaterialID();
	if (m_dirtyMaterialIDSet.insert(materialID).second) {
		m_dirtyMaterialIDs.push_back(materialID);
	}
}

void MaterialManager::ProcessPendingMaterialUpdates(uint64_t frameIndex) {
	ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates");
	const auto updateStart = std::chrono::steady_clock::now();
	const auto streamingStart = std::chrono::steady_clock::now();
	if (m_textureStreamingManager) {
		ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates::TextureStreaming");
		m_textureStreamingManager->EnqueueFrameTick(frameIndex);
		m_textureStreamingManager->DrainPendingBindingChanges();
	}
	const auto streamingEnd = std::chrono::steady_clock::now();
	const auto& lateReadbackPath = MaterialTextureLateReadbackPath();
	if (!m_traceLateReadbackRequested && frameIndex >= 600u && !lateReadbackPath.empty()) {
		if (const auto texture = m_traceBaseColorTexture.lock()) {
			const auto published = texture->GetPublishedBindingSnapshot().image;
			if (published && published->HasValidBackingResource() && m_requestTextureReadback) {
				m_traceLateReadbackRequested = true;
				const uint64_t resourceID = published->GetGlobalResourceID();
				spdlog::info(
					"SARP material texture trace: requesting late base-color readback resource={} frame={}.",
					resourceID,
					frameIndex);
				m_requestTextureReadback(
					published,
					lateReadbackPath,
					[resourceID, lateReadbackPath]() {
						spdlog::info(
							"SARP material texture trace: completed late base-color readback resource={} output='{}'.",
							resourceID,
							std::filesystem::path(lateReadbackPath).string());
					});
			}
		}
	}

	std::vector<uint32_t> dirtyMaterialIDs;
	{
		ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates::CollectDirtyMaterials");
		dirtyMaterialIDs.swap(m_dirtyMaterialIDs);
		m_dirtyMaterialIDSet.clear();
		TracyPlot("MaterialManager.DirtyMaterialCount", static_cast<int64_t>(dirtyMaterialIDs.size()));
	}
	const auto dirtyMaterialStart = std::chrono::steady_clock::now();
	std::size_t dirtyMaterialsVisited = 0;
	std::size_t dirtyMaterialsFlushed = 0;
	{
		ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates::FlushDirtyMaterials");
		for (const uint32_t materialID : dirtyMaterialIDs) {
			ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates::FlushDirtyMaterials::Material");
			ZoneValue(materialID);
			++dirtyMaterialsVisited;
			auto materialIt = m_activeMaterialsByID.find(materialID);
			if (materialIt == m_activeMaterialsByID.end() || materialIt->second == nullptr) {
				continue;
			}

			FlushDirtyMaterial(*materialIt->second, nullptr);
			++dirtyMaterialsFlushed;
		}
	}
	const auto dirtyMaterialEnd = std::chrono::steady_clock::now();

	const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - updateStart).count();
	const auto streamingUs = std::chrono::duration_cast<std::chrono::microseconds>(streamingEnd - streamingStart).count();
	const auto dirtyMaterialUs = std::chrono::duration_cast<std::chrono::microseconds>(dirtyMaterialEnd - dirtyMaterialStart).count();
	const auto now = std::chrono::steady_clock::now();
	const bool hadWork =
		dirtyMaterialsVisited != 0 ||
		elapsedUs >= 2000;
	if (hadWork && now - m_lastMaterialUpdateStatsLog >= std::chrono::seconds(1)) {
		m_lastMaterialUpdateStatsLog = now;
		spdlog::debug(
			"MaterialManager::ProcessPendingMaterialUpdates stats: elapsed_us={} textureStreaming_us={} dirtyMaterial_us={} dirtyMaterials visited={} flushed={} activeMaterials={}",
			elapsedUs,
			streamingUs,
			dirtyMaterialUs,
			dirtyMaterialsVisited,
			dirtyMaterialsFlushed,
			m_activeMaterialsByID.size());
	}
}

unsigned int MaterialManager::IncrementMaterialUsageCount(Material& material, TextureFactory* textureFactory, unsigned int count) {
	ZoneScopedN("MaterialManager::IncrementMaterialUsageCount");
	ZoneValue(material.GetMaterialID());
	//std::lock_guard<std::mutex> lock(m_materialSlotMappingMutex);
	if (count == 0u) {
		ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::CountZeroSlotLookup");
		return GetMaterialSlot(material.GetMaterialID());
	}

	uint32_t materialID = material.GetMaterialID();
	decltype(m_materialIDSlotMapping)::iterator existingSlotIt;
	bool alreadyResident = false;
	{
		ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::ResidentLookup");
		existingSlotIt = m_materialIDSlotMapping.find(materialID);
		alreadyResident =
			existingSlotIt != m_materialIDSlotMapping.end()
			&& existingSlotIt->second < m_materialUsageCounts.size()
			&& m_materialUsageCounts[existingSlotIt->second] > 0u;
		TracyPlot("MaterialManager.IncrementUsage.AlreadyResident", alreadyResident ? int64_t{ 1 } : int64_t{ 0 });
	}

	unsigned int materialSlot = 0;
	{
		ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::ResolveMaterialSlot");
		materialSlot = alreadyResident
			? existingSlotIt->second
			: GetMaterialSlot(materialID, textureFactory ? std::optional<PerMaterialCB>{ material.GetData() } : std::nullopt);
		material.SetOpenPBRMaterialDataIndex(materialSlot);
		m_activeMaterialsByID[materialID] = &material;
	}

	m_materialUsageCounts[materialSlot] += count;
	if (m_materialUsageCounts[materialSlot] == 1u) {
		ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::FirstUse");
		if (textureFactory) {
			ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::FirstUse::FlushDirtyMaterial");
			FlushDirtyMaterial(material, textureFactory);
		} else {
			{
				ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::FirstUse::UpdateTextureUsage");
				UpdateMaterialTextureUsage(material, 1);
			}
			{
				ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::FirstUse::TrackTextureAssets");
				TrackMaterialTextureAssets(material, 1);
			}
			{
				ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::FirstUse::MarkDirty");
				MarkMaterialDirty(material);
			}
		}
	}
	return materialSlot;
}

void MaterialManager::UpdateMaterialDataBuffer(Material& material) {
	FlushDirtyMaterial(material);
}

void MaterialManager::FlushDirtyMaterial(Material& material, TextureFactory* textureFactory) {
	ZoneScopedN("MaterialManager::FlushDirtyMaterial");
	ZoneValue(material.GetMaterialID());
	const unsigned int materialSlot = GetMaterialSlot(material.GetMaterialID());
	material.SetOpenPBRMaterialDataIndex(materialSlot);
	const bool textureAssetsChanged = MaterialTextureAssetBindingsChanged(material);
	const bool refreshedTextures = textureFactory != nullptr || textureAssetsChanged;
	if (textureAssetsChanged) {
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::UntrackTextureBindings");
			TrackMaterialTextureAssets(material, -1);
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::TrackTextureBindings");
			TrackMaterialTextureAssets(material, 1);
		}
	}

	PerMaterialCB materialData{};
	PerMaterialEvalCB evalData{};
	PerMaterialOpenPBRCB openPBRData{};
	{
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::BuildMaterialCBs");
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::RefreshTextureBindings");
			material.RefreshTextureBindings();
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::BuildMaterialCBs::Base");
			materialData = material.GetData();
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::BuildMaterialCBs::Eval");
			evalData = BuildMaterialEvalData(material);
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::BuildMaterialCBs::OpenPBR");
			openPBRData = BuildOpenPBRMaterialData(material);
		}
	}
	if (materialSlot >= m_materialUploadSignatures.size()) {
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::ResizeSignatures");
		m_materialUploadSignatures.resize(static_cast<size_t>(materialSlot) + 1u);
	}

	auto& signature = m_materialUploadSignatures[materialSlot];
	bool dataChanged = false;
	{
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::CompareUploadSignature");
		dataChanged =
			!signature.valid ||
			!BytewiseEqual(signature.materialData, materialData) ||
			!BytewiseEqual(signature.evalData, evalData) ||
			!BytewiseEqual(signature.openPBRData, openPBRData);
	}
	const auto descForAtlasDebug = material.ToCacheDescription();
	if (ShouldTraceMaterialTexture(descForAtlasDebug.baseColor.sourcePath) ||
		ShouldTraceMaterialTexture(descForAtlasDebug.normal.sourcePath)) {
		auto textureState = [](const std::shared_ptr<TextureAsset>& texture) {
			if (!texture) {
				return std::string("texture=null");
			}
			const auto published = texture->GetPublishedBindingSnapshot();
			const auto prepared = texture->PreparedImagePtr();
			const auto streaming = texture->GetStreamingState();
			const auto pending = texture->GetPendingDebugInfo();
			const auto srv = [](const std::shared_ptr<PixelBuffer>& image) {
				return image && image->HasValidBackingResource()
					? image->GetSRVInfo(0).slot.index
					: UINT32_MAX;
			};
			return fmt::format(
				"streamingID={} bindingRevision={} publishedRevision={} publishedResource={} publishedSrv={} "
				"preparedResource={} preparedSrv={} residentTopMip={} requestedTopMip={} pendingTopMip={} "
				"usable={} fallback={} initialData='{}' loadPath={} uploadPath={}",
				texture->GetStreamingTextureID(),
				texture->GetBindingRevision(),
				published.bindingRevision,
				published.image ? published.image->GetGlobalResourceID() : 0u,
				srv(published.image),
				prepared ? prepared->GetGlobalResourceID() : 0u,
				srv(prepared),
				streaming.residency.residentTopMip,
				streaming.requestedTopMip,
				streaming.pendingTopMip,
				texture->HasUsableImage(),
				texture->IsUsingFallbackImage(),
				pending.initialData,
				pending.loadPath,
				pending.uploadPath);
		};
		spdlog::info(
			"SARP material texture trace: materialID={} slot={} name='{}' dataChanged={} refreshedTextures={} "
			"flags=0x{:x} compileFlags=0x{:x} baseFactor=({},{},{},{}) baseChannels=({},{},{},{}) "
			"baseUv={} normalUv={} basePath='{}' baseCB=(descriptor={},sampler={},streamingID={}) baseState=[{}] "
			"normalPath='{}' normalCB=(descriptor={},streamingID={}) normalState=[{}]",
			material.GetMaterialID(),
			materialSlot,
			descForAtlasDebug.name,
			dataChanged,
			refreshedTextures,
			materialData.materialFlags,
			static_cast<std::uint64_t>(material.Technique().compileFlags),
			materialData.baseColorFactor.x,
			materialData.baseColorFactor.y,
			materialData.baseColorFactor.z,
			materialData.baseColorFactor.w,
			materialData.baseColorChannels.x,
			materialData.baseColorChannels.y,
			materialData.baseColorChannels.z,
			materialData.baseColorChannels.w,
			materialData.baseColorUvSetIndex,
			materialData.normalUvSetIndex,
			descForAtlasDebug.baseColor.sourcePath,
			materialData.baseColorTextureIndex,
			materialData.baseColorSamplerIndex,
			materialData.baseColorStreamingTextureID,
			textureState(descForAtlasDebug.baseColor.texture),
			descForAtlasDebug.normal.sourcePath,
			materialData.normalTextureIndex,
			materialData.normalStreamingTextureID,
			textureState(descForAtlasDebug.normal.texture));

		const auto& readbackPath = MaterialTextureReadbackPath();
		const auto& baseTexture = descForAtlasDebug.baseColor.texture;
		if (ShouldTraceMaterialTexture(descForAtlasDebug.baseColor.sourcePath) && baseTexture) {
			m_traceBaseColorTexture = baseTexture;
		}
		if (!readbackPath.empty() &&
			ShouldTraceMaterialTexture(descForAtlasDebug.baseColor.sourcePath) &&
			baseTexture &&
			baseTexture->GetStreamingState().residency.residentTopMip == 0u) {
			const auto published = baseTexture->GetPublishedBindingSnapshot().image;
			if (published && published->HasValidBackingResource()) {
				const uint64_t resourceID = published->GetGlobalResourceID();
				if (m_requestTextureReadback && m_traceReadbackResourceIDs.insert(resourceID).second) {
					spdlog::info(
						"SARP material texture trace: requesting published base-color readback resource={} path='{}'.",
						resourceID,
						descForAtlasDebug.baseColor.sourcePath);
					m_requestTextureReadback(
						published,
						readbackPath,
						[resourceID, readbackPath]() {
							spdlog::info(
								"SARP material texture trace: completed published base-color readback resource={} output='{}'.",
								resourceID,
								std::filesystem::path(readbackPath).string());
						});
				}
			}
		}
	}
	const bool isObjectReyesAtlasHeightMaterial =
		descForAtlasDebug.heightMap.sourcePath.find("object_reyes_atlas_height") != std::string::npos ||
		descForAtlasDebug.heightMap.uvSetName == "__object_reyes_atlas_height" ||
		materialData.objectSurfaceSamplingMode == static_cast<std::uint32_t>(ObjectSurfaceSamplingMode::AtlasBakedHeight);
	if (isObjectReyesAtlasHeightMaterial && (dataChanged || refreshedTextures)) {
		static std::atomic<std::uint32_t> loggedAtlasPublications{ 0 };
		const auto logIndex = loggedAtlasPublications.fetch_add(1, std::memory_order_relaxed);
		if (logIndex < 4096u) {
			const auto* heightTexture = descForAtlasDebug.heightMap.texture.get();
			spdlog::info(
				"SARP Object Reyes atlas material publication: id={} slot={} name='{}' base='{}' atlas='{}' dataChanged={} refreshedTextures={} heightIndex={} heightSampler={} heightUv={} heightScale={} geom=[{},{}] geometric={} fallbackHeight={} usableHeight={} objectSurfaceMode={} flags=0x{:x} compileFlags=0x{:x} rasterFlags=0x{:x}.",
				material.GetMaterialID(),
				materialSlot,
				descForAtlasDebug.name,
				descForAtlasDebug.baseColor.sourcePath,
				descForAtlasDebug.heightMap.sourcePath,
				dataChanged ? 1 : 0,
				refreshedTextures ? 1 : 0,
				materialData.heightMapIndex,
				materialData.heightSamplerIndex,
				materialData.heightUvSetIndex,
				materialData.heightMapScale,
				materialData.geometricDisplacementMin,
				materialData.geometricDisplacementMax,
				materialData.geometricDisplacementEnabled,
				heightTexture ? heightTexture->IsUsingFallbackImage() : false,
				heightTexture ? heightTexture->HasUsableImage() : false,
				materialData.objectSurfaceSamplingMode,
				materialData.materialFlags,
				static_cast<std::uint64_t>(material.Technique().compileFlags),
				static_cast<std::uint32_t>(material.Technique().rasterFlags));
		}
	}
	if (dataChanged) {
		if (materialData.geometricDisplacementEnabled != 0u &&
			(materialData.materialFlags & MaterialFlags::MATERIAL_TERRAIN) == 0u) {
			static std::atomic<std::uint32_t> loggedGeometricMaterials{ 0 };
			const auto logIndex = loggedGeometricMaterials.fetch_add(1, std::memory_order_relaxed);
			const auto& desc = descForAtlasDebug;
			const bool forceAtlasHeightLog =
				desc.heightMap.sourcePath.find("object_reyes_atlas_height") != std::string::npos ||
				desc.heightMap.uvSetName == "__object_reyes_atlas_height";
			if (logIndex < 128u || forceAtlasHeightLog) {
				const auto* heightTexture = desc.heightMap.texture.get();
				spdlog::info(
					"SARP material upload: non-terrain geometric material id={} slot={} name='{}' base='{}' height='{}' flags=0x{:x} compileFlags=0x{:x} rasterFlags=0x{:x} baseIndex={} baseSampler={} normalIndex={} mrIndex=({}, {}) aoIndex={} heightIndex={} heightSampler={} heightUv={} heightChannel={} heightScale={} geomMin={} geomMax={} fallbackHeight={} usableHeight={} reyesUvDensity=({}, {}) objectSurfaceMode={} objectSurfaceDensity={}",
					material.GetMaterialID(),
					materialSlot,
					desc.name,
					desc.baseColor.sourcePath,
					desc.heightMap.sourcePath,
					materialData.materialFlags,
					static_cast<std::uint64_t>(material.Technique().compileFlags),
					static_cast<std::uint32_t>(material.Technique().rasterFlags),
					materialData.baseColorTextureIndex,
					materialData.baseColorSamplerIndex,
					materialData.normalTextureIndex,
					materialData.metallicTextureIndex,
					materialData.roughnessTextureIndex,
					materialData.aoMapIndex,
					materialData.heightMapIndex,
					materialData.heightSamplerIndex,
					materialData.heightUvSetIndex,
					materialData.heightChannel,
					materialData.heightMapScale,
					materialData.geometricDisplacementMin,
					materialData.geometricDisplacementMax,
					heightTexture ? heightTexture->IsUsingFallbackImage() : false,
					heightTexture ? heightTexture->HasUsableImage() : false,
					materialData.reyesUvDensity.x,
					materialData.reyesUvDensity.y,
					materialData.objectSurfaceSamplingMode,
					materialData.objectSurfaceTexelDensity);
			}
		}
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::UploadMaterialCBs");
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::UploadMaterialCBs::Base");
			m_perMaterialDataBuffer->UpdateAt(materialSlot, materialData);
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::UploadMaterialCBs::Eval");
			m_perMaterialEvalDataBuffer->UpdateAt(materialSlot, evalData);
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::UploadMaterialCBs::OpenPBR");
			m_perMaterialOpenPBRDataBuffer->UpdateAt(materialSlot, openPBRData);
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::StoreUploadSignature");
			signature.materialData = materialData;
			signature.evalData = evalData;
			signature.openPBRData = openPBRData;
			signature.valid = true;
		}
	}

	if (dataChanged || refreshedTextures) {
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::RefreshTextureUsage");
		RefreshMaterialTextureUsage(material);
	}
}

void MaterialManager::RegisterStreamingTexture(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory) {
	if (!texture) {
		return;
	}

	if (m_textureStreamingManager) {
		m_textureStreamingManager->EnqueueTextureUploadAdvance(texture, "register_streaming_texture");
	}
}

void MaterialManager::DecrementMaterialUsageCount(const Material& material) {
	//std::lock_guard<std::mutex> lock(m_materialSlotMappingMutex);
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

bool MaterialManager::MaterialTextureAssetBindingsChanged(const Material& material) const {
	std::vector<uint32_t> currentTextureIDs;
	for (const auto& texture : CollectMaterialTextureAssets(material)) {
		if (texture && texture->GetStreamingTextureID() != 0u) currentTextureIDs.push_back(texture->GetStreamingTextureID());
	}
	const auto trackedIt = m_materialTextureStreamingTextureIDs.find(material.GetMaterialID());
	return trackedIt == m_materialTextureStreamingTextureIDs.end() || trackedIt->second != currentTextureIDs;
}

void MaterialManager::TrackMaterialTextureAssets(const Material& material, int delta) {
	ZoneScopedN("MaterialManager::TrackMaterialTextureAssets");
	const uint32_t materialID = material.GetMaterialID();
	ZoneValue(materialID);
	if (delta > 0) {
		if (!m_textureStreamingManager) {
			return;
		}
		std::vector<uint64_t> bindingIDs;
		std::vector<uint32_t> streamingTextureIDs;
		std::vector<std::shared_ptr<TextureAsset>> textureAssets;
		{
			ZoneScopedN("MaterialManager::TrackMaterialTextureAssets::CollectAssets");
			textureAssets = CollectMaterialTextureAssets(material);
			TracyPlot("MaterialManager.TrackedTextureAssetCount", static_cast<int64_t>(textureAssets.size()));
		}
		for (const auto& texture : textureAssets) {
			ZoneScopedN("MaterialManager::TrackMaterialTextureAssets::RegisterBinding");
			ZoneValue(materialID);
			if (!texture) {
				continue;
			}

			const uint32_t streamingTextureID = texture->GetStreamingTextureID();
			if (streamingTextureID == 0u) {
				continue;
			}
			TracyPlot("MaterialManager.RegisterBinding.StreamingTextureID", static_cast<int64_t>(streamingTextureID));
			const bool alphaTested =
				(material.Technique().compileFlags & MaterialCompileFlags::MaterialCompileAlphaTest) != 0u;

			const uint64_t bindingID = m_textureStreamingManager->RegisterTextureBinding(
				texture,
				[this, materialID](TextureAsset&) {
					if (auto materialIt = m_activeMaterialsByID.find(materialID);
						materialIt != m_activeMaterialsByID.end() && materialIt->second) {
						MarkMaterialDirty(*materialIt->second);
					}
				},
				"material:" + std::to_string(materialID),
				true,
				alphaTested);
			if (bindingID != 0u) {
				bindingIDs.push_back(bindingID);
				streamingTextureIDs.push_back(streamingTextureID);
			}
		}
		TracyPlot("MaterialManager.RegisteredBindingCountForMaterial", static_cast<int64_t>(bindingIDs.size()));
		m_materialTextureStreamingBindingIDs[materialID] = std::move(bindingIDs);
		m_materialTextureStreamingTextureIDs[materialID] = std::move(streamingTextureIDs);
		return;
	}

	auto trackedIt = m_materialTextureStreamingBindingIDs.find(materialID);
	if (trackedIt == m_materialTextureStreamingBindingIDs.end()) {
		m_materialTextureStreamingTextureIDs.erase(materialID);
		return;
	}

	if (m_textureStreamingManager) {
		ZoneScopedN("MaterialManager::TrackMaterialTextureAssets::UnregisterBindings");
		m_textureStreamingManager->UnregisterTextureBindings(trackedIt->second);
	}

	m_materialTextureStreamingBindingIDs.erase(trackedIt);
	m_materialTextureStreamingTextureIDs.erase(materialID);
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

std::vector<std::shared_ptr<Resource>> MaterialManager::CollectActiveMaterialTextureResources() const {
	std::vector<std::shared_ptr<Resource>> textures;
	std::unordered_set<uint64_t> seenResourceIds;
	for (const auto& [_, trackedTextures] : m_trackedMaterialTextures) {
		for (const auto& texture : trackedTextures) {
			if (!texture) {
				continue;
			}
			if (seenResourceIds.insert(texture->GetGlobalResourceID()).second) {
				textures.push_back(texture);
			}
		}
	}
	return textures;
}

std::shared_ptr<Resource> MaterialManager::ProvideResource(ResourceIdentifier const& key) {
	auto it = m_resources.find(key);
	if (it != m_resources.end()) {
		return it->second;
	}
	return m_textureStreamingManager ? m_textureStreamingManager->ProvideResource(key) : nullptr;
}

std::vector<ResourceIdentifier> MaterialManager::GetSupportedKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources) {
		keys.push_back(key);
	}
	if (m_textureStreamingManager) {
		auto streamingKeys = m_textureStreamingManager->GetSupportedKeys();
		keys.insert(keys.end(), streamingKeys.begin(), streamingKeys.end());
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
	ZoneScopedN("MaterialManager::GetMaterialSlot");
	ZoneValue(materialID);
	unsigned int slot;
	{
		ZoneScopedN("MaterialManager::GetMaterialSlot::Lookup");
		auto it = m_materialIDSlotMapping.find(materialID);
		if (it != m_materialIDSlotMapping.end()) {
			TracyPlot("MaterialManager.GetMaterialSlot.Existing", int64_t{ 1 });
			slot = it->second;
			return slot;
		}
	}
	TracyPlot("MaterialManager.GetMaterialSlot.Existing", int64_t{ 0 });
	if (!m_freeMaterialSlots.empty()) {
		ZoneScopedN("MaterialManager::GetMaterialSlot::ReuseFreeSlot");
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::ReuseFreeSlot::Pop");
			slot = m_freeMaterialSlots.back();
			m_freeMaterialSlots.pop_back();
		}
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::ReuseFreeSlot::ResizeSignatures");
			if (slot >= m_materialUploadSignatures.size()) {
				m_materialUploadSignatures.resize(static_cast<size_t>(slot) + 1u);
			}
			m_materialUploadSignatures[slot].valid = false;
		}
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::ReuseFreeSlot::ClearBuffers");
			if (data.has_value()) {
				m_perMaterialDataBuffer->UpdateAt(slot, data.value());
			}
			else {
				m_perMaterialDataBuffer->UpdateAt(slot, PerMaterialCB{});
			}
			m_perMaterialEvalDataBuffer->UpdateAt(slot, PerMaterialEvalCB{});
			m_perMaterialOpenPBRDataBuffer->UpdateAt(slot, PerMaterialOpenPBRCB{});
		}
	}
	else {
		ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot");
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::BumpCounters");
			slot = m_materialSlotsUsed++;
			m_materialUsageCounts.push_back(0);
		}
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::EnsureCapacity");
			if constexpr (kForceMaterialBufferResizeEveryMaterial) {
				m_perMaterialDataBuffer->Resize(m_materialSlotsUsed);
				m_perMaterialEvalDataBuffer->Resize(m_materialSlotsUsed);
				m_perMaterialOpenPBRDataBuffer->Resize(m_materialSlotsUsed);
				m_materialBufferCapacity = m_materialSlotsUsed;
			} else {
				EnsureMaterialBufferCapacity(m_materialSlotsUsed);
			}
		}
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::ResizeSignatures");
			m_materialUploadSignatures.resize(m_materialSlotsUsed);
			m_materialUploadSignatures[slot].valid = false;
		}
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::ClearBuffers");
			if (data.has_value()) {
				m_perMaterialDataBuffer->UpdateAt(slot, data.value());
			}
			else {
				m_perMaterialDataBuffer->UpdateAt(slot, PerMaterialCB{});
			}
			m_perMaterialEvalDataBuffer->UpdateAt(slot, PerMaterialEvalCB{});
			m_perMaterialOpenPBRDataBuffer->UpdateAt(slot, PerMaterialOpenPBRCB{});
		}
	}
	{
		ZoneScopedN("MaterialManager::GetMaterialSlot::StoreMapping");
		m_materialIDSlotMapping[materialID] = slot;
	}
	return slot;
}

void MaterialManager::EnsureMaterialBufferCapacity(unsigned int requiredSlots) {
	ZoneScopedN("MaterialManager::EnsureMaterialBufferCapacity");
	ZoneValue(requiredSlots);
	if (requiredSlots <= m_materialBufferCapacity) {
		TracyPlot("MaterialManager.MaterialBufferGrow", int64_t{ 0 });
		return;
	}
	TracyPlot("MaterialManager.MaterialBufferGrow", int64_t{ 1 });

	unsigned int newCapacity = std::max(kInitialMaterialBufferCapacity, m_materialBufferCapacity);
	while (newCapacity < requiredSlots) {
		newCapacity *= 2u;
	}
	TracyPlot("MaterialManager.MaterialBufferOldCapacity", static_cast<int64_t>(m_materialBufferCapacity));
	TracyPlot("MaterialManager.MaterialBufferNewCapacity", static_cast<int64_t>(newCapacity));

	spdlog::info("MaterialManager: growing material buffers oldCapacity={} newCapacity={} requiredSlots={}",
		m_materialBufferCapacity,
		newCapacity,
		requiredSlots);

	{
		ZoneScopedN("MaterialManager::EnsureMaterialBufferCapacity::ResizeMaterialData");
		m_perMaterialDataBuffer->Resize(newCapacity);
	}
	{
		ZoneScopedN("MaterialManager::EnsureMaterialBufferCapacity::ResizeMaterialEval");
		m_perMaterialEvalDataBuffer->Resize(newCapacity);
	}
	{
		ZoneScopedN("MaterialManager::EnsureMaterialBufferCapacity::ResizeOpenPBR");
		m_perMaterialOpenPBRDataBuffer->Resize(newCapacity);
	}
	m_materialBufferCapacity = newCapacity;
}

void MaterialManager::EnsureCompileFlagsBufferCapacity(unsigned int requiredSlots) {
	ZoneScopedN("MaterialManager::EnsureCompileFlagsBufferCapacity");
	ZoneValue(requiredSlots);
	const auto currentCapacity = m_materialPixelCountBuffer ? m_materialPixelCountBuffer->Capacity() : 0u;
	if (requiredSlots <= currentCapacity) {
		TracyPlot("MaterialManager.CompileFlagsBufferGrow", int64_t{ 0 });
		return;
	}
	TracyPlot("MaterialManager.CompileFlagsBufferGrow", int64_t{ 1 });

	unsigned int newCapacity = std::max(1u, currentCapacity);
	while (newCapacity < requiredSlots) {
		newCapacity *= 2u;
	}
	TracyPlot("MaterialManager.CompileFlagsBufferOldCapacity", static_cast<int64_t>(currentCapacity));
	TracyPlot("MaterialManager.CompileFlagsBufferNewCapacity", static_cast<int64_t>(newCapacity));

	spdlog::info("MaterialManager: growing compile-flags buffers oldCapacity={} newCapacity={} requiredSlots={}",
		currentCapacity,
		newCapacity,
		requiredSlots);

	{
		ZoneScopedN("MaterialManager::EnsureCompileFlagsBufferCapacity::ResizeSlotBuffers");
		m_materialPixelCountBuffer->Resize(newCapacity);
		m_materialOffsetBuffer->Resize(newCapacity);
		m_materialWriteCursorBuffer->Resize(newCapacity);
		m_materialEvaluationCommandBuffer->Resize(newCapacity);
	}
	{
		ZoneScopedN("MaterialManager::EnsureCompileFlagsBufferCapacity::ResizeBlockBuffers");
		const uint32_t numBlocks = (newCapacity + kScanBlockSize - 1u) / kScanBlockSize;
		m_blockSumsBuffer->Resize(std::max(1u, numBlocks));
		m_scannedBlockSumsBuffer->Resize(std::max(1u, numBlocks));
	}
}

bool MaterialManager::TryGetCompileFlagsSlot(MaterialCompileFlags flags, unsigned int& slot) const {
	return m_compileFlagsRegistry.TryGet(flags, slot);
}

void MaterialManager::CommitGpuVisibleSnapshot() {
	const unsigned int compileFlagsSlotsUsed = m_compileFlagsRegistry.GetSlotsUsed();
	if (m_materialPixelCountBuffer && compileFlagsSlotsUsed > m_materialPixelCountBuffer->Capacity()) {
		EnsureCompileFlagsBufferCapacity(compileFlagsSlotsUsed);
	}

	const auto slotResidentCapacity = static_cast<unsigned int>((std::min<uint64_t>)(
		(std::min<uint64_t>)(m_materialPixelCountBuffer ? m_materialPixelCountBuffer->ResidentCapacity() : 0u,
			m_materialOffsetBuffer ? m_materialOffsetBuffer->ResidentCapacity() : 0u),
		(std::min<uint64_t>)(m_materialWriteCursorBuffer ? m_materialWriteCursorBuffer->ResidentCapacity() : 0u,
			m_materialEvaluationCommandBuffer ? m_materialEvaluationCommandBuffer->ResidentCapacity() : 0u)));
	const auto blockResidentCapacity = static_cast<unsigned int>((std::min<uint64_t>)(
		m_blockSumsBuffer ? m_blockSumsBuffer->ResidentCapacity() : 0u,
		m_scannedBlockSumsBuffer ? m_scannedBlockSumsBuffer->ResidentCapacity() : 0u));
	const auto scanCoveredSlots = blockResidentCapacity * kScanBlockSize;
	const auto publishedSlots = (std::min<unsigned int>)(
		compileFlagsSlotsUsed,
		(std::min<unsigned int>)(slotResidentCapacity, scanCoveredSlots));

	m_publishedCompileFlagsSlotsUsed = publishedSlots;
	m_publishedActiveCompileFlags.clear();
	m_publishedActiveCompileFlagsSlots.clear();
	const auto& activeCompileFlags = m_compileFlagsRegistry.GetActiveFlags();
	m_publishedActiveCompileFlags.reserve(activeCompileFlags.size());
	m_publishedActiveCompileFlagsSlots.reserve(m_compileFlagsRegistry.GetActiveSlots().size());
	for (MaterialCompileFlags flags : activeCompileFlags) {
		unsigned int slot = 0u;
		if (!TryGetCompileFlagsSlot(flags, slot) || slot >= publishedSlots) {
			continue;
		}
		m_publishedActiveCompileFlags.push_back(flags);
		m_publishedActiveCompileFlagsSlots.push_back(slot);
	}
}

unsigned int MaterialManager::AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) {
	ZoneScopedN("MaterialManager::AcquireCompileFlagsSlot");
	if (count == 0u) {
		throw std::invalid_argument("AcquireCompileFlagsSlot requires a non-zero count");
	}
	const auto result = m_compileFlagsRegistry.Acquire(flags, count);
	if (result.createdSlot) {
		EnsureCompileFlagsBufferCapacity(m_compileFlagsRegistry.GetSlotsUsed());
	}
	return result.slot;
}

bool MaterialManager::ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) {
	ZoneScopedN("MaterialManager::ReleaseCompileFlagsSlot");
	if (!m_compileFlagsRegistry.Release(flags, count)) {
		spdlog::error(
			"MaterialManager::ReleaseCompileFlagsSlot rejected flags=0x{:X} count={}",
			static_cast<uint64_t>(flags),
			count);
		return false;
	}
	return true;
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
