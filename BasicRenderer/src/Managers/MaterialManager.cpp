#include "Managers/MaterialManager.h"
#include "../generated/BuiltinResources.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/DynamicResource.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureBindingArtifacts.h"
#include "Render/RasterBucketFlags.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IUploadService.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_set>

#include <BasicTelemetry/Tracy.h>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	constexpr uint64_t kTextureStreamingIdleFramesBeforeCoarsen = 180u;
	constexpr std::string_view kTextureStreamingFeedbackReadbackAnchorPass = "MenuRenderPass";
	constexpr bool kEnableMaterialStateGraph = true;

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
		result.sourceMaterialId = base.sourceMaterialId;
		result.semanticFamily = base.semanticFamily;
		result.surfaceFlags = base.surfaceFlags;
		result.glintParameters = base.glintParameters;
		result.glintEnabled = base.glintEnabled;
		result.diagnosticReason = base.diagnosticReason;
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
	m_snapshotCommitScope = TaskSchedulerManager::GetInstance().CreateScope(
		"MaterialManager::SnapshotCommit");
	auto& rm = ::ResourceManager::GetInstance();

	m_materialBufferCapacity = kInitialMaterialBufferCapacity;
	m_materialBaseJournal.Initialize({}, 0, m_materialBufferCapacity);
	m_materialEvalJournal.Initialize({}, 0, m_materialBufferCapacity);
	m_materialOpenPbrJournal.Initialize({}, 0, m_materialBufferCapacity);
	m_materialBufferFamilies[0] = std::make_unique<br::render::VersionedBufferFamily>(
		br::render::VersionedBufferFamily::Config{
			{ br::render::ArtifactKind::BufferVersion, 0, br::render::kMaterialBaseTableVariant },
			"Published::PerMaterialDataBuffer", sizeof(PerMaterialCB), false, false,
			br::render::PublishedFragmentKind::Materials,
			br::render::PublishedResourceUsage::ShaderResource, br::render::kMaterialBaseTableVariant });
	m_materialBufferFamilies[1] = std::make_unique<br::render::VersionedBufferFamily>(
		br::render::VersionedBufferFamily::Config{
			{ br::render::ArtifactKind::BufferVersion, 0, br::render::kMaterialEvalTableVariant },
			"Published::PerMaterialEvalDataBuffer", sizeof(PerMaterialEvalCB), false, false,
			br::render::PublishedFragmentKind::Materials,
			br::render::PublishedResourceUsage::ShaderResource, br::render::kMaterialEvalTableVariant });
	m_materialBufferFamilies[2] = std::make_unique<br::render::VersionedBufferFamily>(
		br::render::VersionedBufferFamily::Config{
			{ br::render::ArtifactKind::BufferVersion, 0, br::render::kMaterialOpenPbrTableVariant },
			"Published::PerMaterialOpenPBRDataBuffer", sizeof(PerMaterialOpenPBRCB), false, false,
			br::render::PublishedFragmentKind::Materials,
			br::render::PublishedResourceUsage::ShaderResource, br::render::kMaterialOpenPbrTableVariant });
	auto startupBase = DynamicStructuredBuffer<PerMaterialCB>::CreateShared(1, "StartupFallback::PerMaterialDataBuffer", true);
	auto startupEval = DynamicStructuredBuffer<PerMaterialEvalCB>::CreateShared(1, "StartupFallback::PerMaterialEvalDataBuffer", true);
	auto startupOpenPbr = DynamicStructuredBuffer<PerMaterialOpenPBRCB>::CreateShared(1, "StartupFallback::PerMaterialOpenPBRDataBuffer", true);
	m_textureStreamingManager = TextureStreamingManager::CreateUnique();
	org::memory::SetResourceUsageHint(*startupBase, "Material startup fallback");
	org::memory::SetResourceUsageHint(*startupEval, "Material startup fallback");
	org::memory::SetResourceUsageHint(*startupOpenPbr, "Material startup fallback");

	// Visibility buffer resources
    m_materialPixelCountBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialPixelCountBuffer", true);
    m_materialOffsetBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialOffsetBuffer", true);
	m_materialWriteCursorBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "VisUtil::MaterialWriteCursorBuffer", true);
	org::memory::SetResourceUsageHint(*m_materialPixelCountBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_materialOffsetBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_materialWriteCursorBuffer, "Material evaluation buffers");

	// Per-block arrays for hierarchical scan
	const uint32_t numBlocks = (m_compileFlagsRegistry.GetSlotsUsed() + kScanBlockSize - 1u) / kScanBlockSize;
	m_blockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::BlockSumsBuffer", true);
	m_scannedBlockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::ScannedBlockSumsBuffer", true);
	org::memory::SetResourceUsageHint(*m_blockSumsBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_scannedBlockSumsBuffer, "Material evaluation buffers");

	// Indirect command buffer for material evaluation
	m_materialEvaluationCommandBuffer = DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>::CreateShared(m_compileFlagsRegistry.GetSlotsUsed(), "IndirectCommandBuffers::MaterialEvaluationCommandBuffer", true);
	org::memory::SetResourceUsageHint(*m_materialEvaluationCommandBuffer, "Indirect command buffers");

	m_resources["Builtin::VisUtil::MaterialPixelCountBuffer"] = m_materialPixelCountBuffer;
	m_resources["Builtin::VisUtil::MaterialOffsetBuffer"] = m_materialOffsetBuffer;
	m_resources["Builtin::VisUtil::MaterialWriteCursorBuffer"] = m_materialWriteCursorBuffer;
	m_resources["Builtin::VisUtil::BlockSumsBuffer"] = m_blockSumsBuffer;
	m_resources["Builtin::VisUtil::ScannedBlockSumsBuffer"] = m_scannedBlockSumsBuffer;
	m_resources["Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer"] = m_materialEvaluationCommandBuffer;
	m_materialStartupFallbacks = {
		std::make_shared<org::DynamicGloballyIndexedResource>(startupBase),
		std::make_shared<org::DynamicGloballyIndexedResource>(startupEval),
		std::make_shared<org::DynamicGloballyIndexedResource>(startupOpenPbr)
	};
	m_materialStartupFallbacks[0]->SetName("StartupFallback::PerMaterialDataBuffer");
	m_materialStartupFallbacks[1]->SetName("StartupFallback::PerMaterialEvalDataBuffer");
	m_materialStartupFallbacks[2]->SetName("StartupFallback::PerMaterialOpenPBRDataBuffer");
	const auto publishedSource = br::render::PublishedStateSource::ProcessSource();
	const auto makeMaterialResolver = [&](std::uint64_t variant,
		const std::shared_ptr<Resource>& fallback) {
		return std::make_shared<PublishedStateResourceResolver>(publishedSource,
			br::render::PublishedResourceKey{
				br::render::PublishedFragmentKind::Materials,
				br::render::PublishedResourceUsage::ShaderResource, 0, 0, variant },
			fallback, true);
	};
	m_materialTableResolvers = {
		makeMaterialResolver(br::render::kMaterialBaseTableVariant, m_materialStartupFallbacks[0]),
		makeMaterialResolver(br::render::kMaterialEvalTableVariant, m_materialStartupFallbacks[1]),
		makeMaterialResolver(br::render::kMaterialOpenPbrTableVariant, m_materialStartupFallbacks[2])
	};
	m_resolvers[Builtin::PerMaterialDataBuffer] = m_materialTableResolvers[0];
	m_resolvers["Builtin::PerMaterialEvalDataBuffer"] = m_materialTableResolvers[1];
	m_resolvers[Builtin::PerMaterialOpenPBRDataBuffer] = m_materialTableResolvers[2];

	// Reserve built-in material bins up front so render-graph material evaluation buffers are
	// fully sized before passes/materialization/upload steps touch them.
	AcquireCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel);
	CommitGpuVisibleSnapshot();
}

MaterialManager::~MaterialManager() {
	if (m_snapshotCommitScope.Valid()) m_snapshotCommitScope.CancelAndWait();
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
	std::lock_guard mutationLock(m_materialMutationMutex);
	const uint32_t materialID = material.GetMaterialID();
	if (m_dirtyMaterialIDSet.insert(materialID).second) {
		m_dirtyMaterialIDs.push_back(materialID);
	}
}

void MaterialManager::ProcessPendingMaterialUpdates(uint64_t frameIndex) {
	std::unique_lock mutationLock(m_materialMutationMutex, std::try_to_lock);
	if (!mutationLock.owns_lock()) return;
	ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates");
	const auto updateStart = std::chrono::steady_clock::now();
	const auto streamingStart = std::chrono::steady_clock::now();
	if (m_textureStreamingManager) {
		BT_ZONE_SCOPE("MaterialManager::ProcessPendingMaterialUpdates::TextureStreaming");
		{
			BT_ZONE_SCOPE("MaterialManager::ProcessPendingMaterialUpdates::TextureStreaming::EnqueueFrameTick");
			m_textureStreamingManager->EnqueueFrameTick(frameIndex);
		}
		// Ordinary material bindings are latest-state candidates, not exact graph
		// versions. Drain all ready candidates as one cooperative batch. The drain
		// never waits: candidates whose transfer has not completed stay coalesced.
		(void)m_textureStreamingManager->DrainPendingBindingChanges();
	}
	static bool debugTextureReadbackRequested = false;
	if (!debugTextureReadbackRequested && frameIndex >= 120u && m_textureStreamingManager) {
		char* idValue = nullptr;
		size_t idLength = 0;
		wchar_t* pathValue = nullptr;
		size_t pathLength = 0;
		if (_dupenv_s(&idValue, &idLength, "SARP_TEXTURE_READBACK_STREAMING_ID") == 0 && idValue &&
			_wdupenv_s(&pathValue, &pathLength, L"SARP_TEXTURE_READBACK_PATH") == 0 && pathValue) {
			char* end = nullptr;
			const auto id = std::strtoul(idValue, &end, 10);
			if (end != idValue && *end == '\0') {
				debugTextureReadbackRequested = m_textureStreamingManager->RequestStreamingTextureReadback(
					static_cast<uint32_t>(id), pathValue, {});
			}
		}
		std::free(idValue);
		std::free(pathValue);
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
		BT_ZONE_SCOPE("MaterialManager::ProcessPendingMaterialUpdates::FlushDirtyMaterials");
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
	std::lock_guard mutationLock(m_materialMutationMutex);
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

MaterialTextureStreamingReadinessStats MaterialManager::GetMaterialTextureStreamingReadinessStats() const {
	return m_textureStreamingManager
		? m_textureStreamingManager->GetTextureStreamingReadinessStats()
		: MaterialTextureStreamingReadinessStats{};
}

std::shared_ptr<const br::render::PublishedMaterialUsageBatch>
MaterialManager::ApplyMaterialUsageBatch(
	const br::render::MaterialUsageBatchBuildInput& input) {
	auto result = std::make_shared<br::render::PublishedMaterialUsageBatch>();
	result->sourceFingerprint = input.sourceFingerprint;
	result->materialSlots.reserve(input.entries.size());
	{
		std::lock_guard mutationLock(m_materialMutationMutex);
		struct Admission { Material* material = nullptr; std::uint64_t count = 0; };
		std::unordered_map<std::uint32_t, Admission> admissions;
		for (const auto& entry : input.entries) {
			if (!entry.material || entry.count == 0) return {};
			const auto materialID = entry.material->GetMaterialID();
			auto& admission = admissions[materialID];
			if (admission.material && admission.material != entry.material.get()) return {};
			admission.material = entry.material.get();
			admission.count += entry.count;
			if (admission.count > (std::numeric_limits<unsigned int>::max)()) return {};
		}
		for (const auto& [materialID, admission] : admissions) {
			const auto slot = IncrementMaterialUsageCount(
				*admission.material, input.textureFactory,
				static_cast<unsigned int>(admission.count));
			result->materialSlots.emplace_back(materialID, slot);
		}
		std::ranges::sort(result->materialSlots);
	}
	// Commit owns its own try-lock. Calling it while the admission lock was held
	// made every worker-side batch silently skip its graph publication request;
	// progress then depended on an unrelated later caller happening to commit.
	(void)CommitGpuVisibleSnapshot(true);
	return result;
}

bool MaterialManager::ApplyMaterialRowArtifact(const br::render::MaterialRowArtifact& row) {
	std::lock_guard mutationLock(m_materialMutationMutex);
	const auto expected = m_materialRowSourceRevisions.find(row.materialID);
	if (expected == m_materialRowSourceRevisions.end()) {
		basic_telemetry::AddCounter("SARP.Material.RowApplyRejected.MissingSource");
		return false;
	}
	if (expected->second != row.sourceRevision) {
		basic_telemetry::AddCounter("SARP.Material.RowApplyRejected.StaleSource");
		return false;
	}
	const auto slot = m_materialIDSlotMapping.find(row.materialID);
	if (slot == m_materialIDSlotMapping.end() || slot->second != row.materialSlot) {
		basic_telemetry::AddCounter("SARP.Material.RowApplyRejected.SlotMismatch");
		return false;
	}
	if (row.materialSlot >= m_materialUsageCounts.size() || m_materialUsageCounts[row.materialSlot] == 0u) {
		basic_telemetry::AddCounter("SARP.Material.RowApplyRejected.NotLive");
		return false;
	}
	if (row.materialSlot >= m_materialUploadSignatures.size())
		m_materialUploadSignatures.resize(static_cast<std::size_t>(row.materialSlot) + 1u);
	auto& signature = m_materialUploadSignatures[row.materialSlot];
	signature.materialData = row.base;
	signature.evalData = row.evaluation;
	signature.openPBRData = row.openPbr;
	signature.valid = true;
	JournalMaterialRow(row.materialSlot);
	++m_materialRowsAppliedSinceGraphSnapshot;
	basic_telemetry::AddCounter("SARP.Material.RowApplyAccepted");
	basic_telemetry::SetGauge("SARP.Material.RowApplyAccepted.MaxSlot",
		static_cast<std::int64_t>(row.materialSlot));
	// Row acceptance only dirties the aggregate material snapshot. Forcing a
	// graph request here caused the single RendererState lane to alternate a
	// partial table commit with each cooperative acceptance-mailbox slice during
	// bulk import. The commit's bounded quiet-window still guarantees progress,
	// while allowing rows from several slices to share one immutable snapshot.
	ScheduleGpuVisibleSnapshotCommit(false);
	return true;
}

void MaterialManager::UpdateMaterialDataBuffer(Material& material) {
	std::lock_guard mutationLock(m_materialMutationMutex);
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
				"usable={} fallback={} pendingWork={} processing={} reload={} directStorage={} "
				"initialData='{}' loadPath={} uploadPath={}",
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
				texture->HasPendingUploadWork(),
				pending.processingState,
				pending.reloadState,
				pending.directStorageState,
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
	if ((dataChanged || textureAssetsChanged) && m_rendererStateRequests) {
		auto input = std::make_shared<br::render::MaterialRowInput>();
		input->materialID = material.GetMaterialID();
		input->materialSlot = materialSlot;
		input->sourceRevision = ++m_materialRowSourceRevisions[input->materialID];
		input->base = materialData;
		input->evaluation = evalData;
		input->openPbr = openPBRData;
		std::vector<br::render::ArtifactRequirement> bindingRequirements;
		std::uint64_t fingerprint = 1469598103934665603ull;
		const auto mix = [&fingerprint](const auto& value) {
			for (const auto byte : std::as_bytes(std::span(&value, 1))) {
				fingerprint ^= static_cast<std::uint8_t>(byte);
				fingerprint *= 1099511628211ull;
			}
		};
		mix(input->base); mix(input->evaluation); mix(input->openPbr);
		fingerprint ^= input->sourceRevision;
		if (!fingerprint) fingerprint = 1;
		const auto materialID = input->materialID;
		const auto sourceRevision = input->sourceRevision;
		auto payload = br::render::ArtifactPayload::Make<br::render::MaterialRowInput>(std::move(input));
		const bool graphRowRequested = static_cast<bool>(m_rendererStateRequests->SubmitLatest({
			{ br::render::ArtifactKind::Material, materialID, 0 }, sourceRevision,
			std::move(bindingRequirements), std::move(payload), fingerprint }));
		if (!graphRowRequested) {
			basic_telemetry::AddCounter("SARP.Material.RowRequestRejected");
		} else {
			basic_telemetry::AddCounter("SARP.Material.RowRequestAccepted");
			basic_telemetry::SetGauge("SARP.Material.RowRequestAccepted.MaxSlot",
				static_cast<std::int64_t>(materialSlot));
		}
	}
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
	if (dataChanged || textureAssetsChanged) {
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
	}

	if (dataChanged || textureAssetsChanged || refreshedTextures) {
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
	std::lock_guard mutationLock(m_materialMutationMutex);
	//std::lock_guard<std::mutex> lock(m_materialSlotMappingMutex);
	const uint32_t materialID = material.GetMaterialID();
	const unsigned int materialSlot = GetMaterialSlot(materialID);
	m_materialUsageCounts[materialSlot]--;
	if (m_materialUsageCounts[materialSlot] == 0) {
		UpdateMaterialTextureUsage(material, -1);
		TrackMaterialTextureAssets(material, -1);
		if (materialSlot < m_materialUploadSignatures.size()) {
			m_materialUploadSignatures[materialSlot].valid = false;
			JournalMaterialRow(materialSlot);
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
		m_trackedMaterialTextures[materialId] = std::move(textures);
		return;
	}

	auto trackedIt = m_trackedMaterialTextures.find(materialId);
	if (trackedIt == m_trackedMaterialTextures.end()) {
		return;
	}

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
				{},
				"material:" + std::to_string(materialID),
				TextureStreamingBindingOptions{
					.requiresExactGraphPublication = false,
					.alphaTested = alphaTested,
				});
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
	trackedTextures = std::move(currentTextures);
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
	std::lock_guard mutationLock(m_materialMutationMutex);
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
	}
	else {
		ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot");
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::BumpCounters");
			slot = m_materialSlotsUsed++;
			m_materialUsageCounts.push_back(0);
		}
		EnsureMaterialBufferCapacity(m_materialSlotsUsed);
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::ResizeSignatures");
			m_materialUploadSignatures.resize(m_materialSlotsUsed);
			m_materialUploadSignatures[slot].valid = false;
		}
	}
	{
		ZoneScopedN("MaterialManager::GetMaterialSlot::StoreMapping");
		m_materialIDSlotMapping[materialID] = slot;
	}
	return slot;
}

void MaterialManager::JournalMaterialRow(unsigned int materialSlot) {
	const auto valid = materialSlot < m_materialUploadSignatures.size() &&
		m_materialUploadSignatures[materialSlot].valid;
	const auto base = valid ? m_materialUploadSignatures[materialSlot].materialData : PerMaterialCB{};
	const auto eval = valid ? m_materialUploadSignatures[materialSlot].evalData : PerMaterialEvalCB{};
	const auto openPbr = valid ? m_materialUploadSignatures[materialSlot].openPBRData : PerMaterialOpenPBRCB{};
	const auto capacity = (std::max<unsigned int>)(m_materialBufferCapacity, 1u);
	m_materialBaseJournal.RequestCapacity(capacity);
	m_materialEvalJournal.RequestCapacity(capacity);
	m_materialOpenPbrJournal.RequestCapacity(capacity);
	const auto count = (std::max<std::uint64_t>)(m_materialSlotsUsed,
		static_cast<std::uint64_t>(materialSlot) + 1u);
	const auto asBytes = [](const auto& value) {
		return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(value));
	};
	const auto revision = m_materialBaseJournal.AppendWrite(materialSlot, asBytes(base), count);
	const auto evalRevision = m_materialEvalJournal.AppendWrite(materialSlot, asBytes(eval), count);
	const auto openPbrRevision = m_materialOpenPbrJournal.AppendWrite(materialSlot, asBytes(openPbr), count);
	assert(revision == evalRevision && revision == openPbrRevision);
	m_materialRowsRevision.store(revision, std::memory_order_release);
}

void MaterialManager::UpdateOpenPBRMaterialDataBuffer(
	unsigned int materialSlot, const PerMaterialOpenPBRCB& data) {
	if (materialSlot >= m_materialUploadSignatures.size()) {
		m_materialUploadSignatures.resize(static_cast<std::size_t>(materialSlot) + 1u);
	}
	auto& signature = m_materialUploadSignatures[materialSlot];
	if (signature.valid && std::memcmp(&signature.openPBRData, &data, sizeof(data)) == 0) return;
	signature.openPBRData = data;
	signature.valid = true;
	JournalMaterialRow(materialSlot);
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

bool MaterialManager::RequestExternalMaterialTextureReadback(
	const std::shared_ptr<PixelBuffer>& image,
	std::wstring outputFile,
	std::function<void()> callback)
{
	return m_textureStreamingManager &&
		m_textureStreamingManager->RequestExternalMaterialTextureReadback(
			image, std::move(outputFile), std::move(callback));
}

std::uint64_t MaterialManager::CommitGpuVisibleSnapshot(bool forceGraphSnapshot) {
	const auto commitStarted = std::chrono::steady_clock::now();
	struct CommitDurationRecorder {
		std::chrono::steady_clock::time_point started;
		~CommitDurationRecorder() {
			basic_telemetry::Record("SARP.Material.SnapshotCommitDurationNs",
				static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - started).count()));
		}
	} durationRecorder{ commitStarted };
	basic_telemetry::AddCounter("SARP.Material.SnapshotCommitAttempts");
	if (forceGraphSnapshot) basic_telemetry::AddCounter("SARP.Material.SnapshotCommitForcedAttempts");
	std::unique_lock mutationLock(m_materialMutationMutex, std::try_to_lock);
	if (!mutationLock.owns_lock()) {
		basic_telemetry::AddCounter("SARP.Material.SnapshotCommitMutationLockBusy");
		return m_materialStateRevision;
	}
	BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot");
	const unsigned int compileFlagsSlotsUsed = m_compileFlagsRegistry.GetSlotsUsed();
	if (m_materialPixelCountBuffer && compileFlagsSlotsUsed > m_materialPixelCountBuffer->Capacity()) {
		BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot::EnsureCompileFlagsBufferCapacity");
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

	std::vector<br::render::MaterialCompileFlagEntryDTO> activeCompileFlags;
	{
		BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot::PublishActiveFlags");
		const auto& registryActiveFlags = m_compileFlagsRegistry.GetActiveFlags();
		const auto& registryActiveSlots = m_compileFlagsRegistry.GetActiveSlots();
		::std::vector<br::render::MaterialCompileFlagEntryDTO> captured;
		captured.reserve(registryActiveFlags.size());
		const auto activeCount = (std::min)(registryActiveFlags.size(), registryActiveSlots.size());
		for (std::size_t i = 0; i < activeCount; ++i) {
			const auto slot = registryActiveSlots[i];
			if (slot >= publishedSlots) {
				continue;
			}
			captured.push_back({ registryActiveFlags[i], slot });
		}
		activeCompileFlags = std::move(captured);
	}

	if constexpr (kEnableMaterialStateGraph) if (const auto source = br::render::PublishedStateSource::ProcessSource()) {
		if (const auto published = source->Load()) {
			if (const auto materialState = published->materials.payload.Get<br::render::PublishedMaterialState>()) {
				const auto revision = published->materials.revision;
				// Acknowledgement advances several journals and buffer-family retirement
				// cursors. Replaying it every render update was both unnecessary and a
				// sizeable host-thread cost while a material revision remained current.
				if (revision > m_acknowledgedMaterialPublishedRevision) {
					m_materialBaseJournal.Acknowledge(materialState->baseTable);
					m_materialEvalJournal.Acknowledge(materialState->evalTable);
					m_materialOpenPbrJournal.Acknowledge(materialState->openPbrTable);
					m_materialBufferFamilies[0]->Acknowledge(materialState->baseTable);
					m_materialBufferFamilies[1]->Acknowledge(materialState->evalTable);
					m_materialBufferFamilies[2]->Acknowledge(materialState->openPbrTable);
					m_acknowledgedMaterialPublishedRevision = revision;
				}
				if (materialState->baseTable && materialState->evalTable && materialState->openPbrTable &&
					published->materials.revision > m_observedMaterialPublishedRevision) {
					m_observedMaterialPublishedRevision = published->materials.revision;
					const auto resourceID = [](const auto& table) {
						return table && table->resource ? table->resource->GetGlobalResourceID() : 0u;
					};
					spdlog::info(
						"MaterialManager: graph-owned material table ready epoch={} revision={} rows(desired={} published={} slotsUsed={} capacity={}) resources(base={} eval={} openPbr={})",
						published->epoch, published->materials.revision, m_materialSlotsUsed,
						materialState->baseTable->elementCount, m_materialSlotsUsed,
						materialState->baseTable->capacity,
						resourceID(materialState->baseTable), resourceID(materialState->evalTable),
						resourceID(materialState->openPbrTable));
				}
			}
		}
	}
	if constexpr (kEnableMaterialStateGraph) if (m_rendererStateRequests) {
		// The three parameter tables and their MaterialTable root are one mutable
		// publication epoch. Keep newer row mutations in the authoritative journals
		// until the previously admitted root is render-visible and acknowledged.
		// Admitting another root earlier retains another exact three-backing closure;
		// under streaming churn that exhausts the bounded rings and strands the
		// render-visible table at an early (often startup-only) row extent.
		if (m_materialStateRevision > m_acknowledgedMaterialPublishedRevision) {
			basic_telemetry::AddCounter("SARP.Material.GraphSnapshotMailboxCoalesced");
			return 0;
		}
		std::uint64_t fingerprint = publishedSlots;
		const auto mix = [&fingerprint](std::uint64_t value) {
			fingerprint ^= value + 0x9e3779b97f4a7c15ull + (fingerprint << 6u) + (fingerprint >> 2u);
		};
		for (const auto& entry : activeCompileFlags) {
			mix(static_cast<std::uint64_t>(entry.flags));
			mix(entry.slot);
		}
		mix(m_materialRowsRevision.load(std::memory_order_acquire));
		if (fingerprint != m_pendingMaterialStateFingerprint) {
			m_pendingMaterialStateFingerprint = fingerprint;
			m_materialStateStableFrames = 0;
		} else if (m_materialStateStableFrames < 4u) {
			++m_materialStateStableFrames;
		}
		if (fingerprint != m_materialStateFingerprint) {
			++m_materialStateDirtyFrames;
		} else {
			m_materialStateDirtyFrames = 0;
		}
		// Prefer a short quiet window during bulk creation, but cap the debounce.
		// Texture adoption can change at least one row every frame for a long time;
		// waiting for global quiescence would leave the active table pointing at
		// descriptors displaced by newer texture bindings indefinitely.
		if (fingerprint != m_materialStateFingerprint &&
			(forceGraphSnapshot || m_materialStateStableFrames >= 4u ||
				m_materialStateDirtyFrames >= 4u)) {
			basic_telemetry::AddCounter("SARP.Material.GraphSnapshotAttempts");
			m_materialStateFingerprint = fingerprint;
			m_materialStateDirtyFrames = 0;
			if (!m_uploadService) {
				spdlog::error("Material graph publication skipped: upload service unavailable");
				return m_materialStateRevision;
			}
			auto baseCapture = m_materialBaseJournal.CaptureDesired();
			auto evalCapture = m_materialEvalJournal.CaptureDesired();
			auto openPbrCapture = m_materialOpenPbrJournal.CaptureDesired();
			const auto rowsRevision = baseCapture.writeSequence;
			// The three tables form one material version. Worker admission may
			// append a row between these individually locked captures; defer that
			// sample instead of submitting different bytes under one revision. Slot
			// allocation can also run ahead of immutable row completion; requesting a
			// root for that partial extent creates a permanently obsolete retrying
			// version that prevents its successor from owning the address.
			if (rowsRevision == 0 || evalCapture.writeSequence != rowsRevision ||
				openPbrCapture.writeSequence != rowsRevision ||
				m_materialRowsRevision.load(std::memory_order_acquire) != rowsRevision ||
				baseCapture.elementCount != m_materialSlotsUsed ||
				evalCapture.elementCount != m_materialSlotsUsed ||
				openPbrCapture.elementCount != m_materialSlotsUsed) {
				m_materialStateFingerprint = 0;
				m_materialStateDirtyFrames = 4;
				basic_telemetry::AddCounter("SARP.Material.GraphCaptureRaceDeferred");
				return m_materialStateRevision;
			}
			std::array<br::render::ArtifactRequestResult, 3> tableRequests{};
			const bool reuseTableHandles = m_materialTableHandleRowsRevision == rowsRevision &&
				std::ranges::all_of(m_materialTableHandles, [](const auto& handle) {
					return static_cast<bool>(handle);
				});
			if (reuseTableHandles) {
				for (std::size_t index = 0; index < tableRequests.size(); ++index) {
					tableRequests[index].status = br::render::ArtifactRequestStatus::AlreadyDesired;
					tableRequests[index].version = m_materialTableHandles[index].version;
					tableRequests[index].lease = m_materialTableHandles[index].lease;
				}
				basic_telemetry::AddCounter("SARP.Material.GraphTableVersionsReused", 3);
			} else {
				tableRequests[0] = m_materialBufferFamilies[0]->RequestCapture(
					*m_rendererStateRequests, *m_uploadService, rowsRevision,
					std::move(baseCapture));
				tableRequests[1] = m_materialBufferFamilies[1]->RequestCapture(
					*m_rendererStateRequests, *m_uploadService, rowsRevision,
					std::move(evalCapture));
				tableRequests[2] = m_materialBufferFamilies[2]->RequestCapture(
					*m_rendererStateRequests, *m_uploadService, rowsRevision,
					std::move(openPbrCapture));
			}
			const auto& baseRequest = tableRequests[0];
			const auto& evalRequest = tableRequests[1];
			const auto& openPbrRequest = tableRequests[2];
			if (!baseRequest || !evalRequest || !openPbrRequest) {
				spdlog::error(
					"Material graph table request rejected: rows={} base={} eval={} openPbr={}",
					rowsRevision,
					static_cast<unsigned>(baseRequest.status),
					static_cast<unsigned>(evalRequest.status),
					static_cast<unsigned>(openPbrRequest.status));
				m_materialStateFingerprint = 0;
				m_materialStateDirtyFrames = 4;
				return m_materialStateRevision;
			}
			if (!reuseTableHandles) {
				m_materialTableHandleRowsRevision = rowsRevision;
				for (std::size_t index = 0; index < tableRequests.size(); ++index)
					m_materialTableHandles[index] = tableRequests[index].Handle();
			}
			auto input = std::make_shared<br::render::MaterialStateBuildInput>();
			input->sourceFingerprint = fingerprint;
			input->materialRowsRevision = rowsRevision;
			input->materialRowCount = m_materialSlotsUsed;
			// Texture mip residency is owned by TextureStreamingManager. The descriptor
			// indices captured in the material rows already name usable coarse-mip
			// bindings and remain stable while streaming upgrades their contents. Do not
			// make table publication wait for every texture's asynchronous graph state.
			input->slotsUsed = publishedSlots;
			input->activeCompileFlags = activeCompileFlags;
			input->baseTableKey = m_materialBufferFamilies[0]->Configuration().address;
			input->evalTableKey = m_materialBufferFamilies[1]->Configuration().address;
			input->openPbrTableKey = m_materialBufferFamilies[2]->Configuration().address;
			const auto revision = ++m_materialStateRevision;
			std::vector<br::render::ArtifactRequirement> requirements{
				br::render::Exact(baseRequest.version, br::render::ArtifactReadiness::UploadSubmitted),
				br::render::Exact(evalRequest.version, br::render::ArtifactReadiness::UploadSubmitted),
				br::render::Exact(openPbrRequest.version, br::render::ArtifactReadiness::UploadSubmitted)
			};
			const auto materialRequest = m_rendererStateRequests->SubmitLatest({
				{ br::render::ArtifactKind::MaterialTable, 0, 0 }, revision, requirements,
				br::render::ArtifactPayload::Make<br::render::MaterialStateBuildInput>(std::move(input)),
				fingerprint == 0 ? 1u : fingerprint });
			if (materialRequest) {
				m_materialStateHandle = materialRequest.Handle();
				basic_telemetry::AddCounter("SARP.Material.GraphSnapshotRequests");
				basic_telemetry::Record("SARP.Material.RowsAppliedPerGraphSnapshot",
					m_materialRowsAppliedSinceGraphSnapshot);
				m_materialRowsAppliedSinceGraphSnapshot = 0;
			} else {
				// Admission did not consume this desired state. Reopen the
				// latest-wins mailbox; otherwise the committed fingerprint makes
				// every later frame believe this unpublished snapshot is current.
				m_materialStateFingerprint = 0;
				m_materialStateDirtyFrames = 4;
				basic_telemetry::AddCounter("SARP.Material.GraphSnapshotAdmissionRejected");
			}
		}
	}
	return m_materialStateRevision != 0 &&
		m_materialStateFingerprint == m_pendingMaterialStateFingerprint
		? m_materialStateRevision : 0;
}

void MaterialManager::ScheduleGpuVisibleSnapshotCommit(bool forceGraphSnapshot) {
	if (forceGraphSnapshot) m_forceSnapshotCommit.store(true, std::memory_order_release);
	bool expected = false;
	if (!m_snapshotCommitScheduled.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel)) return;
	const bool submitted = m_snapshotCommitScope.Valid() &&
		TaskSchedulerManager::GetInstance().Submit(
			m_snapshotCommitScope, TaskLane::Streaming, TaskDomain::MaterialAcceptance,
			"MaterialManager::CommitGpuVisibleSnapshot",
			[this](const br::TaskContext& context) {
				if (!context.StopRequested()) {
					const bool force = m_forceSnapshotCommit.exchange(
						false, std::memory_order_acq_rel);
					(void)CommitGpuVisibleSnapshot(force);
				}
				m_snapshotCommitScheduled.store(false, std::memory_order_release);
				// Close the producer race: a force request arriving after the exchange
				// above must schedule a successor even if it observed this task active.
				if (!context.StopRequested() &&
					m_forceSnapshotCommit.load(std::memory_order_acquire)) {
					ScheduleGpuVisibleSnapshotCommit(true);
				}
			});
	if (!submitted) m_snapshotCommitScheduled.store(false, std::memory_order_release);
}

bool MaterialManager::TryActivatePublishedMaterialState() {
	const auto source = br::render::PublishedStateSource::ProcessSource();
	const auto published = source ? source->Load() : nullptr;
	const auto materialState = published
		? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
	if (!materialState || !materialState->baseTable || !materialState->evalTable ||
		!materialState->openPbrTable || published->materials.revision == 0 ||
		published->materials.revision < m_activeMaterialPublishedRevision) {
		return false;
	}
	// A published material state is immutable and closed over the exact texture
	// binding and upload revisions used to build it.  Do not require it to equal
	// the newest desired revision here: texture streaming can invalidate the next
	// revision every frame, and that latest-only gate prevented every completed
	// intermediate state from ever becoming active.  Activate coherent revisions
	// monotonically while the graph continues preparing the newest state.
	for (const auto& resolver : m_materialTableResolvers) {
		if (!resolver) {
			return false;
		}
	}
	if (published->materials.revision == m_activeMaterialPublishedRevision) {
		return true;
	}
	m_materialGraphActive = true;
	m_activeMaterialPublishedRevision = published->materials.revision;
	const auto resolvedBase = m_materialTableResolvers[0]->Resolve();
	const auto resolvedEval = m_materialTableResolvers[1]->Resolve();
	const auto resolvedOpenPbr = m_materialTableResolvers[2]->Resolve();
	spdlog::info(
		"MaterialManager: activated graph-owned material tables epoch={} revision={} rows={} capacity={} compileFlagSlots={} activeCompileFlags={} resolved(base={} expected={} eval={} expected={} openPbr={} expected={})",
		published->epoch, published->materials.revision, materialState->baseTable->elementCount,
		materialState->baseTable->capacity, materialState->compileFlagSlotsUsed,
		materialState->activeCompileFlags.size(),
		resolvedBase.empty() ? 0u : resolvedBase.front()->GetGlobalResourceID(),
		materialState->baseTable->resource->GetGlobalResourceID(),
		resolvedEval.empty() ? 0u : resolvedEval.front()->GetGlobalResourceID(),
		materialState->evalTable->resource->GetGlobalResourceID(),
		resolvedOpenPbr.empty() ? 0u : resolvedOpenPbr.front()->GetGlobalResourceID(),
		materialState->openPbrTable->resource->GetGlobalResourceID());
	return true;
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
