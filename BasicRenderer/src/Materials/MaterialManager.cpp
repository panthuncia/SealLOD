#include "Materials/TextureStreaming/TextureStreamingManager.h"
#include "Materials/MaterialManager.h"
#include "Runtime/Resources/ResourceManager.h"

#include "Materials/MaterialOwnershipGuards.h"
#include "../generated/BuiltinResources.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Resources/DynamicResource.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/IDescriptorService.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
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

	uint64_t ComputeTextureResidentBytes(const org::TextureDescription& desc) {
		uint64_t totalBytes = 0;
		for (const org::ImageDimensions& dims : desc.imageDimensions) {
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

	PerMaterialOpenPBRCB BuildOpenPBRMaterialData(
		const Material& material,
		org::runtime::IDescriptorService& descriptorService) {
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
			samplerIndex = binding.texture->SamplerDescriptorIndex(descriptorService);
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
			samplerIndex = binding.texture->SamplerDescriptorIndex(descriptorService);
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

	std::vector<std::shared_ptr<org::Resource>> CollectMaterialTextureResources(const Material& material) {
		std::vector<std::shared_ptr<org::Resource>> textures;
		std::unordered_set<uint64_t> seenResourceIds;

		material.ForEachReferencedTexture([&](const std::shared_ptr<TextureAsset>& texture) {
			std::shared_ptr<org::Resource> image = texture ? texture->ImagePtr() : nullptr;
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

// See MaterialOwnershipGuards.h for the two rules these enforce.
using MaterialAcceptanceScope = br::materials::AcceptanceScope;
using RegistryLock = br::materials::RegistryLock;

namespace {
	bool OnMaterialAcceptanceDomain() { return br::materials::OnMaterialAcceptance(); }

	void AssertOnMaterialAcceptance(const char* what) {
		br::materials::RequireAcceptanceDomain(what);
	}
}

void MaterialManager::PostMaterialMutation(std::function<void()> mutation) {
	if (!mutation) return;
	// Applied in post order at the head of the next acceptance task, so a
	// producer's mutations land in the order it issued them.
	m_postedMaterialMutations.push(std::move(mutation));
	ScheduleDirtyMaterialFlush();
}

void MaterialManager::DrainPostedMaterialMutations() {
	AssertOnMaterialAcceptance("DrainPostedMaterialMutations");
	// Must be called only from an acceptance task entry point, never while
	// m_registryMutex is held: the mutations below take that lock themselves
	// (a posted first use flushes the material, which allocates a slot).
	br::materials::RequireRegistryLockNotHeld(
		"posted material mutations drained while the registry lock is held");
	//
	// Signature resets first: a slot the registry just recycled must read as
	// having no uploaded row before any dirty material that now owns it is
	// flushed.
	unsigned int slot = 0;
	while (m_slotsNeedingSignatureReset.try_pop(slot)) {
		if (slot >= m_materialUploadSignatures.size()) {
			m_materialUploadSignatures.resize(static_cast<std::size_t>(slot) + 1u);
		}
		m_materialUploadSignatures[slot].valid = false;
	}
	std::function<void()> mutation;
	while (m_postedMaterialMutations.try_pop(mutation)) {
		if (mutation) mutation();
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

	// GPU-written frame scratch is not authored scene data. Allocate the bounded
	// compile-flag domain up front so async material admission never replaces a
	// backing resource while a frame snapshot is recording against it.
	constexpr uint32_t initialCompileFlagCapacity = 256u;
    m_materialPixelCountBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(initialCompileFlagCapacity, "VisUtil::MaterialPixelCountBuffer", true);
    m_materialOffsetBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(initialCompileFlagCapacity, "VisUtil::MaterialOffsetBuffer", true);
	m_materialWriteCursorBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(initialCompileFlagCapacity, "VisUtil::MaterialWriteCursorBuffer", true);
	org::memory::SetResourceUsageHint(*m_materialPixelCountBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_materialOffsetBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_materialWriteCursorBuffer, "Material evaluation buffers");

	// Per-block arrays for hierarchical scan
	const uint32_t numBlocks = (initialCompileFlagCapacity + kScanBlockSize - 1u) / kScanBlockSize;
	m_blockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::BlockSumsBuffer", true);
	m_scannedBlockSumsBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(std::max(1u, numBlocks), "VisUtil::ScannedBlockSumsBuffer", true);
	org::memory::SetResourceUsageHint(*m_blockSumsBuffer, "Material evaluation buffers");
	org::memory::SetResourceUsageHint(*m_scannedBlockSumsBuffer, "Material evaluation buffers");

	// Indirect command buffer for material evaluation
	m_materialEvaluationCommandBuffer = DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>::CreateShared(initialCompileFlagCapacity, "IndirectCommandBuffers::MaterialEvaluationCommandBuffer", true);
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
		const std::shared_ptr<org::Resource>& fallback) {
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
	m_reservationLifetime.reset();
	if (m_snapshotCommitScope.Valid()) m_snapshotCommitScope.CancelAndWait();
}

void MaterialManager::AcknowledgePublishedTextureImageTable(
	const std::shared_ptr<const br::render::PublishedRendererState>& published) {
	if (m_textureStreamingManager)
		m_textureStreamingManager->AcknowledgePublishedImageTable(published);
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
std::shared_ptr<org::RenderPass> MaterialManager::CreateTextureStreamingFeedbackReadbackPass() {
	if (!m_textureStreamingManager || m_textureStreamingFeedbackSuppressed) {
		return {};
	}
	return m_textureStreamingManager->CreateTextureStreamingFeedbackReadbackPass();
}

MaterialTextureStreamingStats MaterialManager::GetMaterialTextureStreamingStats() const {
	if (!m_textureStreamingManager) return {};
	// Called twice per frame from the render thread for the debug menu. Walking
	// every tracked material texture took milliseconds during streaming, so the
	// walk runs on the material acceptance task and this returns the latest
	// completed result (at most one refresh behind).
	const auto published = m_textureStreamingManager->PublishedStatsSequence();
	MaterialTextureStreamingStats stats;
	bool stale = false;
	{
		std::lock_guard lock(m_streamingStatsMutex);
		stale = !m_cachedStreamingStatsValid ||
			m_cachedStreamingStatsTrackedRevision != m_trackedTexturesRevision.load(std::memory_order_acquire) ||
			m_cachedStreamingStatsPublishedSequence != published;
		stats = m_cachedStreamingStats;
	}
	if (stale) ScheduleStreamingStatsRefresh();
	return stats;
}

void MaterialManager::ScheduleStreamingStatsRefresh() const {
	bool expected = false;
	if (!m_streamingStatsRefreshScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	auto* self = const_cast<MaterialManager*>(this);
	const bool submitted = m_snapshotCommitScope.Valid() &&
		TaskSchedulerManager::GetInstance().Submit(
			m_snapshotCommitScope, TaskLane::Streaming, TaskDomain::MaterialAcceptance,
			"MaterialManager::RefreshStreamingStats",
			[self](const br::TaskContext& context) {
				MaterialAcceptanceScope acceptance;
				self->m_streamingStatsRefreshScheduled.store(false, std::memory_order_release);
				if (context.StopRequested() || !self->m_textureStreamingManager) return;
				BT_ZONE_SCOPE("MaterialManager::RefreshStreamingStats");
				self->DrainPostedMaterialMutations();
				const auto trackedRevision = self->m_trackedTexturesRevision.load(std::memory_order_acquire);
				// m_trackedMaterialTextures is authoring state owned by this
				// domain, so collecting from it needs no lock.
				auto resources = self->CollectActiveMaterialTextureResources();
				uint64_t sequence = 0;
				auto stats = self->m_textureStreamingManager->GetTextureStreamingStats(resources, &sequence);
				std::lock_guard lock(self->m_streamingStatsMutex);
				self->m_cachedStreamingStats = std::move(stats);
				self->m_cachedStreamingStatsTrackedRevision = trackedRevision;
				self->m_cachedStreamingStatsPublishedSequence = sequence;
				self->m_cachedStreamingStatsValid = true;
			});
	if (!submitted) m_streamingStatsRefreshScheduled.store(false, std::memory_order_release);
}

void MaterialManager::MarkMaterialDirty(Material& material) {
	const uint32_t materialID = material.GetMaterialID();
	if (OnMaterialAcceptanceDomain()) {
		if (m_dirtyMaterialIDSet.insert(materialID).second) {
			m_dirtyMaterialIDs.push_back(materialID);
		}
		ScheduleDirtyMaterialFlush();
		return;
	}
	PostMaterialMutation([this, materialID] {
		if (m_dirtyMaterialIDSet.insert(materialID).second) {
			m_dirtyMaterialIDs.push_back(materialID);
		}
	});
}

void MaterialManager::ScheduleDirtyMaterialFlush() {
	bool expected = false;
	if (!m_dirtyMaterialFlushScheduled.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel)) return;
	const bool submitted = m_snapshotCommitScope.Valid() &&
		TaskSchedulerManager::GetInstance().Submit(
			m_snapshotCommitScope, TaskLane::Streaming, TaskDomain::MaterialAcceptance,
			"MaterialManager::FlushDirtyMaterials",
			[this](const br::TaskContext& context) {
				MaterialAcceptanceScope acceptance;
				m_dirtyMaterialFlushScheduled.store(false, std::memory_order_release);
				if (context.StopRequested()) return;
				FlushDirtyMaterials();
				if (!m_dirtyMaterialIDs.empty() || !m_postedMaterialMutations.empty()) {
					ScheduleDirtyMaterialFlush();
				}
			});
	if (!submitted) m_dirtyMaterialFlushScheduled.store(false, std::memory_order_release);
}

void MaterialManager::FlushDirtyMaterials() {
	BT_ZONE_SCOPE("MaterialManager::FlushDirtyMaterials");
	// Authoring state, owned by this domain. The whole flush used to hold the
	// single mutation mutex, so every import worker asking for a material slot
	// queued behind it.
	AssertOnMaterialAcceptance("FlushDirtyMaterials");
	DrainPostedMaterialMutations();
	std::vector<uint32_t> dirtyMaterialIDs;
	dirtyMaterialIDs.swap(m_dirtyMaterialIDs);
	m_dirtyMaterialIDSet.clear();
	TracyPlot("MaterialManager.DirtyMaterialCount", static_cast<int64_t>(dirtyMaterialIDs.size()));
	const auto flushStart = std::chrono::steady_clock::now();
	std::size_t flushed = 0;
	for (const uint32_t materialID : dirtyMaterialIDs) {
		ZoneScopedN("MaterialManager::FlushDirtyMaterials::Material");
		ZoneValue(materialID);
		Material* material = nullptr;
		if (const auto materialIt = m_activeMaterialsByID.find(materialID);
			materialIt != m_activeMaterialsByID.end()) {
			material = materialIt->second;
		}
		std::shared_ptr<Material> ingestedOwner;
		if (!material) {
			if (const auto source = m_ingestedMaterialSourcesByID.find(materialID);
				source != m_ingestedMaterialSourcesByID.end()) {
				ingestedOwner = source->second.lock();
				material = ingestedOwner.get();
			}
		}
		if (!material) continue;
		FlushDirtyMaterial(*material, false);
		++flushed;
	}
	const auto now = std::chrono::steady_clock::now();
	if (!dirtyMaterialIDs.empty() && now - m_lastMaterialUpdateStatsLog >= std::chrono::seconds(1)) {
		m_lastMaterialUpdateStatsLog = now;
		spdlog::debug(
			"MaterialManager::FlushDirtyMaterials stats: elapsed_us={} dirtyMaterials visited={} flushed={} activeMaterials={}",
			std::chrono::duration_cast<std::chrono::microseconds>(now - flushStart).count(),
			dirtyMaterialIDs.size(), flushed, m_activeMaterialsByID.size());
	}
}

void MaterialManager::ProcessPendingMaterialUpdates(uint64_t frameIndex) {
	ZoneScopedN("MaterialManager::ProcessPendingMaterialUpdates");
	// The frame tick is a queue notification; it must not wait behind the
	// acceptance-domain flush that holds the mutation mutex.
	if (m_textureStreamingManager) {
		BT_ZONE_SCOPE("MaterialManager::ProcessPendingMaterialUpdates::TextureStreaming::EnqueueFrameTick");
		m_textureStreamingManager->EnqueueFrameTick(frameIndex);
	}
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

	// Dirty material rows are flushed on the material acceptance domain
	// (ScheduleDirtyMaterialFlush); nothing else here touches the state graph.
}

unsigned int MaterialManager::IncrementMaterialUsageCount(
	Material& material, bool refreshTextureBindings, unsigned int count) {
	ZoneScopedN("MaterialManager::IncrementMaterialUsageCount");
	ZoneValue(material.GetMaterialID());
	const uint32_t materialID = material.GetMaterialID();
	if (count == 0u) {
		ZoneScopedN("MaterialManager::IncrementMaterialUsageCount::CountZeroSlotLookup");
		return GetMaterialSlot(materialID);
	}

	// Registry work only, in one short critical section: the slot and the usage
	// count. Building the row, tracking textures and journaling all belong to
	// the acceptance domain and are handed over below.
	unsigned int materialSlot = 0;
	bool firstUse = false;
	{
		RegistryLock registryLock(m_registryMutex);
		const auto existingSlotIt = m_materialIDSlotMapping.find(materialID);
		const bool alreadyResident =
			existingSlotIt != m_materialIDSlotMapping.end()
			&& existingSlotIt->second < m_materialUsageCounts.size()
			&& m_materialUsageCounts[existingSlotIt->second] > 0u;
		TracyPlot("MaterialManager.IncrementUsage.AlreadyResident", alreadyResident ? int64_t{ 1 } : int64_t{ 0 });
		materialSlot = alreadyResident
			? existingSlotIt->second
			: GetMaterialSlotLocked(materialID, refreshTextureBindings
				? std::optional<PerMaterialCB>{ material.GetData() } : std::nullopt);
		m_materialUsageCounts[materialSlot] += count;
		firstUse = m_materialUsageCounts[materialSlot] == count;
	}
	material.SetOpenPBRMaterialDataIndex(materialSlot);

	const auto activate = [this, materialID, &material] {
		m_activeMaterialsByID[materialID] = &material;
	};
	if (!firstUse) {
		if (OnMaterialAcceptanceDomain()) activate();
		else PostMaterialMutation([this, materialID, target = &material] {
			m_activeMaterialsByID[materialID] = target;
		});
		return materialSlot;
	}

	const auto applyFirstUse = [this, materialID, refreshTextureBindings](Material& target) {
		m_activeMaterialsByID[materialID] = &target;
		if (refreshTextureBindings) {
			// The row is gated on its texture bindings by the graph, so it does
			// not matter that this now happens a slice later than the caller.
			FlushDirtyMaterial(target, true);
			return;
		}
		UpdateMaterialTextureUsage(target, 1);
		TrackMaterialTextureAssets(target, 1);
		MarkMaterialDirty(target);
	};
	if (OnMaterialAcceptanceDomain()) {
		applyFirstUse(material);
		return materialSlot;
	}
	PostMaterialMutation([this, applyFirstUse, target = &material] { applyFirstUse(*target); });
	return materialSlot;
}

void MaterialManager::RegisterMaterialSource(const std::shared_ptr<Material>& material) {
	if (!material) return;
	const auto materialID = material->GetMaterialID();
	if (OnMaterialAcceptanceDomain()) {
		m_ingestedMaterialSourcesByID[materialID] = material;
		return;
	}
	// Weak, as the map stores it: posting must not extend the material's life.
	PostMaterialMutation([this, materialID, weak = std::weak_ptr<Material>(material)] {
		m_ingestedMaterialSourcesByID[materialID] = weak;
	});
}

void MaterialManager::SetRendererStateServices(br::render::RendererStateRequestService* requests,
	std::shared_ptr<org::runtime::IUploadService> uploads) {
	m_rendererStateRequests = requests;
	m_uploadService = uploads;
	if (m_textureStreamingManager) m_textureStreamingManager->SetRendererStateRequestService(requests, uploads);
}

void MaterialManager::SetDescriptorService(std::shared_ptr<org::runtime::IDescriptorService> descriptors) {
	if (m_descriptorService == descriptors) return;
	m_descriptorService = std::move(descriptors);
	if (m_textureStreamingManager) m_textureStreamingManager->SetDescriptorService(m_descriptorService);
	// Every existing material's descriptor indices are now stale. Re-dirtying
	// them walks the acceptance domain's own maps, so it runs there.
	const auto redirtyAll = [this] {
		for (const auto& [materialID, material] : m_activeMaterialsByID) {
			if (material && m_dirtyMaterialIDSet.insert(materialID).second)
				m_dirtyMaterialIDs.push_back(materialID);
		}
		for (const auto& [materialID, weakMaterial] : m_ingestedMaterialSourcesByID) {
			if (!weakMaterial.expired() && m_dirtyMaterialIDSet.insert(materialID).second)
				m_dirtyMaterialIDs.push_back(materialID);
		}
	};
	if (OnMaterialAcceptanceDomain()) {
		redirtyAll();
		ScheduleDirtyMaterialFlush();
		return;
	}
	PostMaterialMutation(redirtyAll);
}

void MaterialManager::AppendTextureDisplayRequirements(
	const Material& material, std::vector<br::render::ArtifactRequirement>& requirements) {
	const auto quality = (material.Technique().compileFlags & MaterialCompileFlags::MaterialCompileAlphaTest) != 0u
		? br::render::TextureDisplayQuality::AlphaCoverage
		: br::render::TextureDisplayQuality::AnyImage;
	for (const auto& texture : CollectMaterialTextureAssets(material)) {
		const auto streamingTextureID = texture ? texture->GetStreamingTextureID() : 0u;
		// Unstreamed textures never pass through the binding publication boundary.
		if (streamingTextureID == 0u) continue;
		const auto address = br::render::TextureDisplayGateAddress(streamingTextureID, quality);
		if (std::ranges::any_of(requirements, [&](const auto& existing) { return existing.key == address; })) continue;
		requirements.push_back(br::render::LatestAtLeast(address, 1u, br::render::ArtifactReadiness::CpuReady));
	}
}

void br::render::AppendTextureDisplayRequirements(
	const Material& material, std::vector<ArtifactRequirement>& requirements) {
	MaterialManager::AppendTextureDisplayRequirements(material, requirements);
}

br::render::MaterialUsageCapture MaterialManager::CaptureMaterialUsage(
	Material& material, unsigned int count, bool refreshTextureBindings) {
	if (!m_descriptorService) {
		throw std::runtime_error("MaterialManager: descriptor service unavailable while capturing material usage");
	}
	// No manager state is read here beyond the descriptor service, which is
	// installed once during startup: everything else comes from the Material
	// itself. This used to serialize against the whole acceptance-domain flush.
	if (refreshTextureBindings) material.RefreshTextureBindings(m_descriptorService.get());
	br::render::MaterialUsageCapture capture{};
	auto& entry = capture.entry;
	entry.materialID = material.GetMaterialID();
	entry.count = count;
	entry.base = material.GetData();
	entry.evaluation = BuildMaterialEvalData(material);
	entry.openPbr = BuildOpenPBRMaterialData(material, *m_descriptorService);
	entry.compileFlags = material.Technique().compileFlags;
	capture.textureServiceInputs = CollectMaterialTextureAssets(material);
	entry.retainedTextureResources = CollectMaterialTextureResources(material);
	entry.textureBindings.reserve(capture.textureServiceInputs.size());
	for (const auto& texture : capture.textureServiceInputs) {
		if (!texture) continue;
		const auto binding = texture->GetPublishedBindingSnapshot();
		if (texture->GetStreamingTextureID() == 0 || binding.bindingRevision == 0 ||
			!binding.image || !binding.image->HasValidBackingResource()) continue;
		const auto imageIndex = binding.image && binding.image->HasValidBackingResource()
			? binding.image->GetSRVInfo(0).slot.index : UINT32_MAX;
		entry.textureBindings.push_back({
			texture->GetStreamingTextureID(), binding.bindingRevision, imageIndex,
			texture->SamplerDescriptorIndex(*m_descriptorService) });
	}
	return capture;
}

std::shared_ptr<const br::render::MaterialUsageReservation>
MaterialManager::ReserveMaterialUsage(
	const std::vector<br::render::MaterialUsageCapture>& captures) {
	struct ReservedEntry {
		br::render::MaterialUsageBatchEntry entry;
		std::uint32_t slot = 0;
	};
	struct ReservedBindings {
		std::uint32_t materialID = 0;
		std::vector<std::uint64_t> bindingIDs;
		std::vector<std::uint32_t> streamingTextureIDs;
	};
	auto result = std::make_shared<br::render::PublishedMaterialUsageBatch>();
	std::vector<ReservedBindings> reserved;
	std::vector<ReservedEntry> entries;
	{
		// Registry only. RegisterTextureBinding used to run inside this lock,
		// which held it across another subsystem's locks for every texture of
		// every captured material.
		RegistryLock registryLock(m_registryMutex);
		std::unordered_set<std::uint32_t> materialIDs;
		for (const auto& capture : captures) {
			const auto& entry = capture.entry;
			if (entry.materialID == 0 || entry.count == 0 ||
				!materialIDs.insert(entry.materialID).second) return {};
			const auto existing = m_materialIDSlotMapping.find(entry.materialID);
			const auto current = existing != m_materialIDSlotMapping.end() &&
				existing->second < m_materialUsageCounts.size()
				? static_cast<std::uint64_t>(m_materialUsageCounts[existing->second]) : 0u;
			const auto pending = m_pendingMaterialUsageCounts[entry.materialID];
			if (current + pending + entry.count >
				(std::numeric_limits<unsigned int>::max)()) return {};
		}
		reserved.reserve(captures.size());
		entries.reserve(captures.size());
		result->materialSlots.reserve(captures.size());
		for (const auto& capture : captures) {
			const auto& entry = capture.entry;
			const auto existing = m_materialIDSlotMapping.find(entry.materialID);
			const bool alreadyResident = existing != m_materialIDSlotMapping.end() &&
				existing->second < m_materialUsageCounts.size() &&
				m_materialUsageCounts[existing->second] != 0u;
			if (existing == m_materialIDSlotMapping.end())
				m_materialReservationOwnedIDs.insert(entry.materialID);
			const auto slot = GetMaterialSlotLocked(entry.materialID, entry.base);
			m_pendingMaterialUsageCounts[entry.materialID] += entry.count;
			entries.push_back({ entry, slot });
			result->materialSlots.emplace_back(entry.materialID, slot);
			ReservedBindings material{ .materialID = entry.materialID };
			if (alreadyResident || !m_textureStreamingManager) {
				reserved.push_back(std::move(material));
				continue;
			}
			const bool alphaTested =
				(entry.compileFlags & MaterialCompileFlags::MaterialCompileAlphaTest) != 0u;
			for (const auto& texture : capture.textureServiceInputs) {
				if (!texture || texture->GetStreamingTextureID() == 0u) continue;
				const auto bindingID = m_textureStreamingManager->RegisterTextureBinding(
					texture, {}, "material-reservation:" + std::to_string(entry.materialID),
					TextureStreamingBindingOptions{
						.requiresExactGraphPublication = true,
						.alphaTested = alphaTested });
				if (bindingID == 0u) continue;
				material.bindingIDs.push_back(bindingID);
				material.streamingTextureIDs.push_back(texture->GetStreamingTextureID());
			}
			reserved.push_back(std::move(material));
		}
		std::ranges::sort(result->materialSlots);
	}
	auto weakLifetime = std::weak_ptr<void>(m_reservationLifetime);
	return std::make_shared<br::render::MaterialUsageReservation>(
		result,
		[this, weakLifetime, entries = std::move(entries),
			reserved = std::move(reserved)](bool commit) mutable {
			if (weakLifetime.expired()) return !commit;
			// Commit runs in the graph's acceptance callback on
			// TaskDomain::MaterialAcceptance; rollback can run anywhere, so it
			// takes the registry lock for the counts it unwinds.
			RegistryLock registryLock(m_registryMutex);
			if (!commit) {
				if (m_textureStreamingManager) {
					for (const auto& material : reserved)
						m_textureStreamingManager->UnregisterTextureBindings(material.bindingIDs);
				}
				for (const auto& reservedEntry : entries) {
					const auto materialID = reservedEntry.entry.materialID;
					auto pending = m_pendingMaterialUsageCounts.find(materialID);
					if (pending != m_pendingMaterialUsageCounts.end()) {
						pending->second -= reservedEntry.entry.count;
						if (pending->second == 0) m_pendingMaterialUsageCounts.erase(pending);
					}
					const auto mapping = m_materialIDSlotMapping.find(materialID);
					if (!m_pendingMaterialUsageCounts.contains(materialID) &&
						mapping != m_materialIDSlotMapping.end() &&
						mapping->second < m_materialUsageCounts.size() &&
						m_materialUsageCounts[mapping->second] == 0 &&
						m_materialReservationOwnedIDs.erase(materialID) != 0) {
						// Authoring state: hand the reset to the acceptance domain
						// rather than writing it from whatever thread released
						// the reservation.
						m_slotsNeedingSignatureReset.push(mapping->second);
						m_freeMaterialSlots.push_back(mapping->second);
						m_materialIDSlotMapping.erase(mapping);
					}
				}
				basic_telemetry::AddCounter("SARP.Material.UsageReservation.Cancelled");
				return true;
			}
			for (const auto& reservedEntry : entries) {
				const auto mapping = m_materialIDSlotMapping.find(reservedEntry.entry.materialID);
				const auto pending = m_pendingMaterialUsageCounts.find(reservedEntry.entry.materialID);
				if (mapping == m_materialIDSlotMapping.end() || mapping->second != reservedEntry.slot ||
					pending == m_pendingMaterialUsageCounts.end() ||
					pending->second < reservedEntry.entry.count ||
					reservedEntry.slot >= m_materialUsageCounts.size() ||
					m_materialUsageCounts[reservedEntry.slot] >
						(std::numeric_limits<unsigned int>::max)() - reservedEntry.entry.count) return false;
			}
			for (const auto& reservedEntry : entries) {
				const auto& entry = reservedEntry.entry;
				auto pending = m_pendingMaterialUsageCounts.find(entry.materialID);
				pending->second -= entry.count;
				if (pending->second == 0) m_pendingMaterialUsageCounts.erase(pending);
				const bool firstUse = m_materialUsageCounts[reservedEntry.slot] == 0;
				m_materialUsageCounts[reservedEntry.slot] += entry.count;
				m_materialReservationOwnedIDs.erase(entry.materialID);
				if (firstUse) {
					m_trackedMaterialTextures[entry.materialID] = entry.retainedTextureResources;
					++m_trackedTexturesRevision;
					const auto sourceRevision = ++m_materialRowSourceRevisions[entry.materialID];
					if (!ApplyMaterialRowArtifactLocked({ entry.materialID, reservedEntry.slot, sourceRevision,
						entry.base, entry.evaluation, entry.openPbr })) return false;
				}
			}
			for (auto& material : reserved) {
				if (m_materialTextureStreamingBindingIDs.contains(material.materialID)) {
					if (m_textureStreamingManager)
						m_textureStreamingManager->UnregisterTextureBindings(material.bindingIDs);
					continue;
				}
				m_materialTextureStreamingBindingIDs[material.materialID] =
					std::move(material.bindingIDs);
				m_materialTextureStreamingTextureIDs[material.materialID] =
					std::move(material.streamingTextureIDs);
			}
			basic_telemetry::AddCounter("SARP.Material.UsageReservation.Committed");
			registryLock.Unlock();
			(void)CommitGpuVisibleSnapshot(true);
			return true;
		});
}

MaterialTextureStreamingReadinessStats MaterialManager::GetMaterialTextureStreamingReadinessStats() const {
	return m_textureStreamingManager
		? m_textureStreamingManager->GetTextureStreamingReadinessStats()
		: MaterialTextureStreamingReadinessStats{};
}

bool MaterialManager::ApplyMaterialRowArtifact(const br::render::MaterialRowArtifact& row) {
	// Reached from the row reservation's acceptance callback, which the graph
	// dispatches on TaskDomain::MaterialAcceptance.
	RegistryLock registryLock(m_registryMutex);
	return ApplyMaterialRowArtifactLocked(row);
}

bool MaterialManager::ApplyMaterialRowArtifactLocked(const br::render::MaterialRowArtifact& row) {
	// The slot and usage-count reads below are registry state, so the caller
	// holds m_registryMutex. It must not drain posted mutations here: those
	// take that same lock, which is not recursive.
	MaterialAcceptanceScope acceptance;
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
	if (OnMaterialAcceptanceDomain()) {
		FlushDirtyMaterial(material);
		return;
	}
	// Flushing builds constant buffers, journals a row and submits to the graph;
	// it belongs to the acceptance domain. Mark dirty and let the flush pick it
	// up rather than performing that work on the caller's thread.
	MarkMaterialDirty(material);
}

void MaterialManager::FlushDirtyMaterial(Material& material, bool refreshTextureBindings) {
	ZoneScopedN("MaterialManager::FlushDirtyMaterial");
	ZoneValue(material.GetMaterialID());
	const unsigned int materialSlot = GetMaterialSlot(material.GetMaterialID());
	material.SetOpenPBRMaterialDataIndex(materialSlot);
	// Collected once and reused for the change test, the binding registration and
	// the graph requirements below. Each of those used to walk the material's
	// textures again, so a single flush built this list three times.
	const auto textureAssets = CollectMaterialTextureAssets(material);
	const bool textureAssetsChanged = MaterialTextureAssetBindingsChanged(material, textureAssets);
	const bool refreshedTextures = refreshTextureBindings || textureAssetsChanged;
	if (textureAssetsChanged) {
		{
			// Untracking reads the stored binding IDs and ignores the asset list,
			// so there is nothing to collect for it.
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::UntrackTextureBindings");
			UntrackMaterialTextureAssets(material.GetMaterialID());
		}
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::TrackTextureBindings");
			TrackMaterialTextureAssets(material.GetMaterialID(), textureAssets,
				(material.Technique().compileFlags & MaterialCompileFlags::MaterialCompileAlphaTest) != 0u,
				1);
		}
	}

	PerMaterialCB materialData{};
	PerMaterialEvalCB evalData{};
	PerMaterialOpenPBRCB openPBRData{};
	{
		ZoneScopedN("MaterialManager::FlushDirtyMaterial::BuildMaterialCBs");
		{
			ZoneScopedN("MaterialManager::FlushDirtyMaterial::RefreshTextureBindings");
			material.RefreshTextureBindings(m_descriptorService.get());
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
			if (!m_descriptorService) {
				throw std::runtime_error("MaterialManager: descriptor service unavailable while publishing material data");
			}
			openPBRData = BuildOpenPBRMaterialData(material, *m_descriptorService);
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
	// ToCacheDescription copies every texture path, name and UV set of the
	// material. Its only consumers are the diagnostic blocks below, all of which
	// are inert in a normal run, so it is built only when one of them can fire.
	// A material identified as an atlas-height material purely by its height-map
	// path, with no trace filter configured, now skips an spdlog::info line it
	// used to emit; nothing else changes.
	const bool materialDiagnosticsPossible =
		!MaterialTextureTraceFilter().empty() ||
		materialData.objectSurfaceSamplingMode ==
			static_cast<std::uint32_t>(ObjectSurfaceSamplingMode::AtlasBakedHeight) ||
		(materialData.geometricDisplacementEnabled != 0u &&
			(materialData.materialFlags & MaterialFlags::MATERIAL_TERRAIN) == 0u);
	const auto descForAtlasDebug = materialDiagnosticsPossible
		? material.ToCacheDescription() : MaterialDescription{};
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
			const auto srv = [](const std::shared_ptr<org::PixelBuffer>& image) {
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
		auto row = std::make_shared<br::render::MaterialRowArtifact>();
		row->materialID = input->materialID;
		row->materialSlot = input->materialSlot;
		row->sourceRevision = input->sourceRevision;
		row->base = input->base;
		row->evaluation = input->evaluation;
		row->openPbr = input->openPbr;
		const auto weakLifetime = std::weak_ptr<void>(m_reservationLifetime);
		input->reservation = std::make_shared<br::render::MaterialRowReservation>(
			row, [this, weakLifetime, row](bool commit) {
				if (weakLifetime.expired()) return !commit;
				if (!commit) {
					basic_telemetry::AddCounter("SARP.Material.RowReservation.Cancelled");
					return true;
				}
				const bool applied = ApplyMaterialRowArtifact(*row);
				basic_telemetry::AddCounter(applied
					? "SARP.Material.RowReservation.Committed"
					: "SARP.Material.RowReservation.CommitFailed");
				return applied;
			});
		// A material row must not publish ahead of the textures it references.
		// This edge is what makes that the graph's job: the row stays Blocked
		// until every referenced binding reaches UploadSubmitted, and the graph
		// re-queues the row by itself when a binding publishes a newer version,
		// so no texture-to-material dirty propagation is needed.
		//
		// LatestAtLeast rather than Exact, for the reason the static-import path
		// documents: a texture successor can be published between this capture
		// and admission, and an unleased exact requirement would then name a
		// reclaimed version forever. The descriptor indices in the row are
		// verified against the selected binding by the producer.
		std::vector<br::render::ArtifactRequirement> bindingRequirements;
		std::unordered_set<std::uint32_t> requiredStreamingTextureIDs;
		for (const auto& texture : textureAssets) {
			if (!texture) continue;
			const auto streamingTextureID = texture->GetStreamingTextureID();
			// A texture with no streaming ID is not registered for streaming and
			// will never publish a binding, so requiring one would block forever.
			// Every texture that does have one had a binding registered by
			// TrackMaterialTextureAssets, so a binding is guaranteed to arrive.
			if (streamingTextureID == 0u) continue;
			if (!requiredStreamingTextureIDs.insert(streamingTextureID).second) continue;
			// Deliberately not skipped when no binding has been published yet:
			// that is exactly the case this gate exists for. A minimum of 1 means
			// "any published binding", so a material whose textures have not
			// loaded stays Blocked instead of publishing against nothing.
			const auto binding = texture->GetPublishedBindingSnapshot();
			bindingRequirements.push_back(br::render::LatestAtLeast(
				br::render::ArtifactAddress{
					br::render::ArtifactKind::TextureBinding, streamingTextureID, 0 },
				(std::max<std::uint64_t>)(binding.bindingRevision, 1u),
				br::render::ArtifactReadiness::UploadSubmitted));
		}
		basic_telemetry::Record("SARP.Material.RowTextureDependencies",
			static_cast<std::uint64_t>(bindingRequirements.size()));
		std::uint64_t fingerprint = 1469598103934665603ull;
		const auto mix = [&fingerprint](const auto& value) {
			for (const auto byte : std::as_bytes(std::span(&value, 1))) {
				fingerprint ^= static_cast<std::uint8_t>(byte);
				fingerprint *= 1099511628211ull;
			}
		};
		mix(input->base); mix(input->evaluation); mix(input->openPbr);
		fingerprint ^= input->sourceRevision;
		// The requirement set is part of the request's identity: two requests for
		// the same revision with different texture closures are a conflict, not a
		// duplicate, and the graph rejects a mismatched recipe on that basis.
		for (const auto& requirement : bindingRequirements) {
			fingerprint ^= requirement.key.primaryID + 0x9e3779b97f4a7c15ull +
				(fingerprint << 6u) + (fingerprint >> 2u);
			fingerprint ^= requirement.minimumRevision + 0x9e3779b97f4a7c15ull +
				(fingerprint << 6u) + (fingerprint >> 2u);
		}
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
	const uint32_t materialID = material.GetMaterialID();
	// Registry half: the count, and slot recycling once it reaches zero.
	bool reachedZero = false;
	unsigned int materialSlot = 0;
	{
		RegistryLock registryLock(m_registryMutex);
		materialSlot = GetMaterialSlotLocked(materialID);
		if (materialSlot < m_materialUsageCounts.size() &&
			m_materialUsageCounts[materialSlot] != 0u) {
			m_materialUsageCounts[materialSlot]--;
			reachedZero = m_materialUsageCounts[materialSlot] == 0u;
		}
		if (reachedZero) {
			// A queued graph admission owns this identity/slot reservation even though
			// the currently published usage reached zero. Recycle it only when that
			// reservation commits or is joined and cancelled.
			if (m_pendingMaterialUsageCounts.contains(materialID)) {
				m_materialReservationOwnedIDs.insert(materialID);
			} else {
				m_freeMaterialSlots.push_back(materialSlot);
				m_materialIDSlotMapping.erase(materialID);
			}
		}
	}
	if (!reachedZero) return;

	// Authoring half: texture tracking, the upload signature and the journal.
	const auto retire = [this, materialID, materialSlot](const Material& target) {
		UpdateMaterialTextureUsage(target, -1);
		UntrackMaterialTextureAssets(materialID);
		if (materialSlot < m_materialUploadSignatures.size()) {
			m_materialUploadSignatures[materialSlot].valid = false;
			JournalMaterialRow(materialSlot);
		}
		m_activeMaterialsByID.erase(materialID);
		m_ingestedMaterialSourcesByID.erase(materialID);
		m_dirtyMaterialIDSet.erase(materialID);
		std::erase(m_dirtyMaterialIDs, materialID);
	};
	if (OnMaterialAcceptanceDomain()) {
		retire(material);
		return;
	}
	// UpdateMaterialTextureUsage only needs the material's texture list, which
	// the caller owns for the duration of this call, so it is resolved now and
	// the rest is applied on the acceptance domain.
	auto textures = CollectMaterialTextureResources(material);
	PostMaterialMutation([this, materialID, materialSlot, textures = std::move(textures)] {
		auto& tracked = m_trackedMaterialTextures[materialID];
		tracked.clear();
		m_trackedMaterialTextures.erase(materialID);
		++m_trackedTexturesRevision;
		UntrackMaterialTextureAssets(materialID);
		if (materialSlot < m_materialUploadSignatures.size()) {
			m_materialUploadSignatures[materialSlot].valid = false;
			JournalMaterialRow(materialSlot);
		}
		m_activeMaterialsByID.erase(materialID);
		m_ingestedMaterialSourcesByID.erase(materialID);
		m_dirtyMaterialIDSet.erase(materialID);
		std::erase(m_dirtyMaterialIDs, materialID);
	});
}

void MaterialManager::UpdateMaterialTextureUsage(const Material& material, int delta) {
	const uint32_t materialId = material.GetMaterialID();
	if (delta > 0) {
		auto textures = CollectMaterialTextureResources(material);
		m_trackedMaterialTextures[materialId] = std::move(textures);
		++m_trackedTexturesRevision;
		return;
	}

	auto trackedIt = m_trackedMaterialTextures.find(materialId);
	if (trackedIt == m_trackedMaterialTextures.end()) {
		return;
	}

	m_trackedMaterialTextures.erase(trackedIt);
	++m_trackedTexturesRevision;
}

bool MaterialManager::MaterialTextureAssetBindingsChanged(const Material& material) const {
	return MaterialTextureAssetBindingsChanged(material, CollectMaterialTextureAssets(material));
}

bool MaterialManager::MaterialTextureAssetBindingsChanged(const Material& material,
	const std::vector<std::shared_ptr<TextureAsset>>& textureAssets) const {
	std::vector<uint32_t> currentTextureIDs;
	currentTextureIDs.reserve(textureAssets.size());
	for (const auto& texture : textureAssets) {
		if (texture && texture->GetStreamingTextureID() != 0u) currentTextureIDs.push_back(texture->GetStreamingTextureID());
	}
	const auto trackedIt = m_materialTextureStreamingTextureIDs.find(material.GetMaterialID());
	return trackedIt == m_materialTextureStreamingTextureIDs.end() || trackedIt->second != currentTextureIDs;
}

void MaterialManager::UntrackMaterialTextureAssets(std::uint32_t materialID) {
	TrackMaterialTextureAssets(materialID, {}, false, -1);
}

void MaterialManager::TrackMaterialTextureAssets(const Material& material, int delta) {
	TrackMaterialTextureAssets(material.GetMaterialID(),
		CollectMaterialTextureAssets(material),
		(material.Technique().compileFlags & MaterialCompileFlags::MaterialCompileAlphaTest) != 0u,
		delta);
}

void MaterialManager::TrackMaterialTextureAssets(
	std::uint32_t materialID,
	const std::vector<std::shared_ptr<TextureAsset>>& textureAssets,
	bool alphaTested,
	int delta) {
	ZoneScopedN("MaterialManager::TrackMaterialTextureAssets");
	ZoneValue(materialID);
	if (delta > 0) {
		if (!m_textureStreamingManager) {
			return;
		}
		std::vector<uint64_t> bindingIDs;
		std::vector<uint32_t> streamingTextureIDs;
		TracyPlot("MaterialManager.TrackedTextureAssetCount", static_cast<int64_t>(textureAssets.size()));
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
	++m_trackedTexturesRevision;
}

std::vector<std::shared_ptr<org::Resource>> MaterialManager::CollectActiveMaterialTextureResources() const {
	std::vector<std::shared_ptr<org::Resource>> textures;
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

std::shared_ptr<org::Resource> MaterialManager::ProvideResource(org::ResourceIdentifier const& key) {
	auto it = m_resources.find(key);
	if (it != m_resources.end()) {
		return it->second;
	}
	return m_textureStreamingManager ? m_textureStreamingManager->ProvideResource(key) : nullptr;
}

std::vector<org::ResourceIdentifier> MaterialManager::GetSupportedKeys() {
	std::vector<org::ResourceIdentifier> keys;
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

std::vector<org::ResourceIdentifier> MaterialManager::GetSupportedResolverKeys() {
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resolvers.size());
	for (auto const& [key, _] : m_resolvers) {
		keys.push_back(key);
	}
	if (m_textureStreamingManager) {
		auto streamingKeys = m_textureStreamingManager->GetSupportedResolverKeys();
		keys.insert(keys.end(), streamingKeys.begin(), streamingKeys.end());
	}
	return keys;
}

std::shared_ptr<org::IResourceResolver> MaterialManager::ProvideResolver(org::ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it != m_resolvers.end()) {
		return it->second;
	}
	return m_textureStreamingManager ? m_textureStreamingManager->ProvideResolver(key) : nullptr;
}

// TODO: C++26 will allow optional references
unsigned int MaterialManager::GetMaterialSlot(unsigned int materialID, std::optional<PerMaterialCB> data) {
	RegistryLock registryLock(m_registryMutex);
	return GetMaterialSlotLocked(materialID, std::move(data));
}

unsigned int MaterialManager::GetMaterialSlotLocked(unsigned int materialID, std::optional<PerMaterialCB> data) {
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
		// The signature belongs to the acceptance domain. Hand the reset over
		// rather than reaching into it from a worker: a recycled slot must not
		// present the previous material's row as still uploaded.
		m_slotsNeedingSignatureReset.push(slot);
	}
	else {
		ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot");
		{
			ZoneScopedN("MaterialManager::GetMaterialSlot::AllocateNewSlot::BumpCounters");
			slot = m_materialSlotsUsed++;
			m_materialUsageCounts.push_back(0);
		}
		EnsureMaterialBufferCapacity(m_materialSlotsUsed);
		m_slotsNeedingSignatureReset.push(slot);
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
	const auto currentCapacity = m_materialBufferCapacity.load(std::memory_order_relaxed);
	if (requiredSlots <= currentCapacity) {
		TracyPlot("MaterialManager.MaterialBufferGrow", int64_t{ 0 });
		return;
	}
	TracyPlot("MaterialManager.MaterialBufferGrow", int64_t{ 1 });

	unsigned int newCapacity = std::max(kInitialMaterialBufferCapacity, currentCapacity);
	while (newCapacity < requiredSlots) {
		newCapacity *= 2u;
	}
	TracyPlot("MaterialManager.MaterialBufferOldCapacity", static_cast<int64_t>(currentCapacity));
	TracyPlot("MaterialManager.MaterialBufferNewCapacity", static_cast<int64_t>(newCapacity));

	m_materialBufferCapacity.store(newCapacity, std::memory_order_release);
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
	RegistryLock registryLock(m_registryMutex);
	return m_compileFlagsRegistry.TryGet(flags, slot);
}

bool MaterialManager::RequestExternalMaterialTextureReadback(
	const std::shared_ptr<org::PixelBuffer>& image,
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
	// Scheduled onto TaskDomain::MaterialAcceptance, so it owns the journals and
	// signatures outright; only the compile-flag and raster-bucket reads below
	// belong to the registry.
	MaterialAcceptanceScope acceptance;
	DrainPostedMaterialMutations();
	BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot");
	unsigned int compileFlagsSlotsUsed = 0;
	{
		RegistryLock registryLock(m_registryMutex);
		compileFlagsSlotsUsed = m_compileFlagsRegistry.GetSlotsUsed();
	}
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
	std::vector<MaterialRasterFlags> rasterBucketFlags;
	{
		BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot::PublishActiveFlags");
		// Registry state: import workers acquire and release compile-flag slots
		// and raster buckets concurrently with this publication.
		RegistryLock registryLock(m_registryMutex);
		const auto& registryActiveFlags = m_compileFlagsRegistry.GetActiveFlags();
		const auto& registryActiveSlots = m_compileFlagsRegistry.GetActiveSlots();
		::std::vector<br::render::MaterialCompileFlagEntryDTO> captured;
		captured.reserve(registryActiveFlags.size());
		// A slot beyond publishedSlots is silently dropped below. Material
		// evaluation then never dispatches for that compile-flag variant, so
		// geometry that rasterized correctly is never shaded. Reyes variants are
		// acquired after their regular counterpart and therefore hold the highest
		// slots, which makes them the first casualties of this clamp.
		if (publishedSlots < compileFlagsSlotsUsed) {
			static std::atomic<std::uint32_t> loggedCompileFlagClamps{ 0 };
			if (loggedCompileFlagClamps.fetch_add(1, std::memory_order_relaxed) < 32u) {
				spdlog::warn(
					"MaterialManager: compile-flag table clamped; slotsUsed={} published={} dropped={} "
					"slotResidentCapacity={} scanCoveredSlots={}",
					compileFlagsSlotsUsed, publishedSlots,
					compileFlagsSlotsUsed - publishedSlots,
					slotResidentCapacity, scanCoveredSlots);
			}
		}
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
	{
		BT_ZONE_SCOPE("MaterialManager::CommitGpuVisibleSnapshot::PublishRasterBuckets");
		RegistryLock registryLock(m_registryMutex);
		rasterBucketFlags.reserve(m_rasterBucketsUsed);
		for (unsigned int bucket = 0; bucket < m_rasterBucketsUsed; ++bucket) {
			rasterBucketFlags.push_back(bucket < m_bucketToRasterFlagMapping.size()
				? m_bucketToRasterFlagMapping[bucket]
				: MaterialRasterFlags::MaterialRasterFlagsNone);
		}
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
		for (const auto flags : rasterBucketFlags) mix(static_cast<std::uint64_t>(flags));
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
					*m_rendererStateRequests, m_uploadService, rowsRevision,
					std::move(baseCapture));
				tableRequests[1] = m_materialBufferFamilies[1]->RequestCapture(
					*m_rendererStateRequests, m_uploadService, rowsRevision,
					std::move(evalCapture));
				tableRequests[2] = m_materialBufferFamilies[2]->RequestCapture(
					*m_rendererStateRequests, m_uploadService, rowsRevision,
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
			input->rasterBucketFlags = rasterBucketFlags;
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

bool MaterialManager::TryActivatePublishedMaterialState(
	const std::shared_ptr<const br::render::PublishedRendererState>& published) {
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
	return true;
}

unsigned int MaterialManager::AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) {
	ZoneScopedN("MaterialManager::AcquireCompileFlagsSlot");
	if (count == 0u) {
		throw std::invalid_argument("AcquireCompileFlagsSlot requires a non-zero count");
	}
	RegistryLock registryLock(m_registryMutex);
	const auto result = m_compileFlagsRegistry.Acquire(flags, count);
	// This is an authoring mutation only. GPU scratch capacity is reconciled by
	// CommitGpuVisibleSnapshot, which publishes the matching material revision;
	// resizing a live scratch buffer from an import worker races frame recording.
	return result.slot;
}

bool MaterialManager::ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) {
	ZoneScopedN("MaterialManager::ReleaseCompileFlagsSlot");
	RegistryLock registryLock(m_registryMutex);
	if (!m_compileFlagsRegistry.Release(flags, count)) {
		spdlog::error(
			"MaterialManager::ReleaseCompileFlagsSlot rejected flags=0x{:X} count={}",
			static_cast<uint64_t>(flags),
			count);
		return false;
	}
	return true;
}

unsigned int MaterialManager::GetRasterBucketCount() const {
	RegistryLock registryLock(m_registryMutex);
	return m_rasterBucketsUsed;
}

unsigned int MaterialManager::GetRasterBucketForFlags(MaterialRasterFlags rasterFlags) const {
	RegistryLock registryLock(m_registryMutex);
	return GetRasterBucketForFlagsLocked(rasterFlags);
}

unsigned int MaterialManager::GetRasterBucketForFlagsLocked(MaterialRasterFlags rasterFlags) const {
	const auto it = m_rasterFlagToBucketMapping.find(static_cast<uint32_t>(rasterFlags));
	if (it != m_rasterFlagToBucketMapping.end()) {
		return it->second;
	}
	spdlog::error("Raster flags not found in mapping!");
	return 0;
}

MaterialRasterFlags MaterialManager::GetRasterFlagsForBucket(unsigned int bucketIndex) const {
	RegistryLock registryLock(m_registryMutex);
	if (bucketIndex < m_bucketToRasterFlagMapping.size()) {
		return m_bucketToRasterFlagMapping[bucketIndex];
	}
	spdlog::error("Bucket index out of range!");
	return MaterialRasterFlags::MaterialRasterFlagsNone;
}

unsigned int MaterialManager::AcquireRasterBucket(MaterialRasterFlags rasterFlags, unsigned int count) {
	if (count == 0u) {
		// A lookup is still a read of registry state: another thread may be
		// inserting a bucket, and rehashing the map underneath this find is a
		// crash rather than a stale answer.
		RegistryLock registryLock(m_registryMutex);
		return GetRasterBucketForFlagsLocked(rasterFlags);
	}
	RegistryLock registryLock(m_registryMutex);

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
	RegistryLock registryLock(m_registryMutex);
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
