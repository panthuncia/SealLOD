#pragma once

#include <memory>
#include <mutex>
#include <chrono>

#include "Materials/Material.h"
#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/ResourceGroup.h"
#include "Render/IndirectCommand.h"
#include "Render/RasterBucketFlags.h"

namespace rg::runtime {
class IReadbackService;
}

class TextureFactory;

struct MaterialTextureStreamingStats {
	uint32_t uniqueMaterialTextureCount = 0;
	uint32_t uniqueStreamableTextureCount = 0;
	uint32_t uniqueStreamingEnabledTextureCount = 0;
	uint32_t fullResolutionResidentTextureCount = 0;
	uint32_t streamableFullResolutionResidentTextureCount = 0;
	uint32_t pendingReloadTextureCount = 0;
	uint64_t totalResidentBytes = 0;
	uint64_t streamableResidentBytes = 0;
	std::vector<uint32_t> residentTopMipHistogram = {};
};

// Manages buffers for per-material-compile-flag work (e.g., visibility buffer per-material)
class MaterialManager : public IResourceProvider {
public:
	static std::unique_ptr<MaterialManager> CreateUnique() {
		return std::unique_ptr<MaterialManager>(new MaterialManager());
	}
	unsigned int GetCompileFlagsSlot(MaterialCompileFlags flags);
	unsigned int GetMaterialSlot(unsigned int materialID, std::optional<PerMaterialCB> data = std::nullopt);
	unsigned int AcquireRasterBucket(MaterialRasterFlags rasterFlags, unsigned int count = 1u);
	void ReleaseRasterBucket(MaterialRasterFlags rasterFlags);

	unsigned int IncrementMaterialUsageCount(Material& material, TextureFactory* textureFactory = nullptr, unsigned int count = 1u);
	void DecrementMaterialUsageCount(const Material& material);
	void BeginTextureStreamingFeedbackFrame(uint64_t frameIndex);
	void ProcessPendingMaterialUpdates(uint64_t frameIndex, TextureFactory& textureFactory);
	void RequestTextureStreamingFeedbackReadback(rg::runtime::IReadbackService* readbackService);
	MaterialTextureStreamingStats GetMaterialTextureStreamingStats() const;

	void UpdateMaterialDataBuffer(Material& material);
	void MarkMaterialDirty(Material& material);
	void UpdateOpenPBRMaterialDataBuffer(unsigned int materialSlot, const PerMaterialOpenPBRCB& data) {
		m_perMaterialOpenPBRDataBuffer->UpdateAt(materialSlot, data);
	}

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

	const std::vector<unsigned int>& GetActiveCompileFlagsSlots() const { return m_activeCompileFlagsSlots; }
	const std::vector<MaterialCompileFlags>& GetActiveCompileFlags() const { return m_activeCompileFlags; }
	unsigned int GetCompileFlagsSlotsUsed() const { return m_compileFlagsSlotsUsed; }

	unsigned int GetRasterBucketCount() const { return m_rasterBucketsUsed; }
	unsigned int GetRasterBucketForFlags(MaterialRasterFlags rasterFlags) const {
		auto it = m_rasterFlagToBucketMapping.find(static_cast<uint32_t>(rasterFlags));
		if (it != m_rasterFlagToBucketMapping.end()) {
			return it->second;
		}
		spdlog::error("Raster flags not found in mapping!");
		return 0;
	}
	MaterialRasterFlags GetRasterFlagsForBucket(unsigned int bucketIndex) const {
		if (bucketIndex < m_bucketToRasterFlagMapping.size()) {
			return m_bucketToRasterFlagMapping[bucketIndex];
		}
		spdlog::error("Bucket index out of range!");
		return MaterialRasterFlags::MaterialRasterFlagsNone;
	}
private:
	MaterialManager();
	void UpdateMaterialTextureUsage(const Material& material, int delta);
	void RefreshMaterialTextureUsage(const Material& material);
	void UpdateTrackedMaterialTextureRefs(const std::vector<std::shared_ptr<Resource>>& textures, int delta);
	void TrackMaterialTextureAssets(const Material& material, int delta);
	void UpdateTextureStreamingMetadata(const Material& material);
	void UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture);
	void MarkTextureStreamingMetadataDirty(const std::shared_ptr<TextureAsset>& texture, bool needsUploadAdvance = false, const char* reason = "unknown");
	void RecordTextureDirtyReason(const char* reason);
	void FlushDirtyMaterial(Material& material, TextureFactory* textureFactory = nullptr);
	void FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture);
	void EnsureTextureUploadAdvanced(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory);

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<IResourceResolver>, ResourceIdentifier::Hasher> m_resolvers;
	std::shared_ptr<ResourceGroup> m_activeMaterialTextureGroup;
	std::unordered_map<uint64_t, uint32_t> m_materialTextureUsageCounts;
	std::unordered_map<uint32_t, std::vector<std::shared_ptr<Resource>>> m_trackedMaterialTextures;
	std::unordered_map<uint32_t, Material*> m_activeMaterialsByID;
	std::unordered_map<uint32_t, std::vector<uint32_t>> m_materialStreamingTextureIDs;
	std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_streamingTextureMaterialIDs;
	std::unordered_map <MaterialCompileFlags, unsigned int> m_compileFlagsSlotMapping;
	std::atomic<unsigned int> m_nextCompileFlagsSlot;
	std::vector<unsigned int> m_freeCompileFlagsSlots;
	std::vector<unsigned int> m_compileFlagsUsageCounts = { 0 };
	std::vector<unsigned int> m_activeCompileFlagsSlots;
	std::vector<MaterialCompileFlags> m_activeCompileFlags;
	//std::mutex m_compileFlagsSlotMappingMutex;
	unsigned int m_compileFlagsSlotsUsed = 1;

	unsigned int m_materialSlotsUsed = 0;
	std::vector<unsigned int> m_freeMaterialSlots;
	std::vector<unsigned int> m_materialUsageCounts = { };
	std::unordered_map <unsigned int, unsigned int> m_materialIDSlotMapping;
	struct MaterialGpuUploadSignature {
		PerMaterialCB materialData = {};
		PerMaterialEvalCB evalData = {};
		PerMaterialOpenPBRCB openPBRData = {};
		bool valid = false;
	};
	std::vector<MaterialGpuUploadSignature> m_materialUploadSignatures;

	static constexpr unsigned int kBufferGrowthSize = 100;

	static constexpr unsigned int kScanBlockSize = 1024;

	// Material raster flags to raster bin mapping
	std::unordered_map<uint32_t, unsigned int> m_rasterFlagToBucketMapping;
	std::vector<MaterialRasterFlags> m_bucketToRasterFlagMapping;
	std::vector<unsigned int> m_rasterBucketUsageCounts;
	unsigned int m_rasterBucketsUsed = 0;
	std::vector<unsigned int> m_freeRasterBuckets;

	// Visibility buffer
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialPixelCountBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialOffsetBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialWriteCursorBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_blockSumsBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_scannedBlockSumsBuffer;
	std::shared_ptr<DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>> m_materialEvaluationCommandBuffer;

	std::shared_ptr<DynamicStructuredBuffer<PerMaterialCB>> m_perMaterialDataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<PerMaterialEvalCB>> m_perMaterialEvalDataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<PerMaterialOpenPBRCB>> m_perMaterialOpenPBRDataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<TextureStreamingGPUInfo>> m_textureStreamingMetadataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_textureStreamingFeedbackBuffer;
	std::unordered_map<uint64_t, std::weak_ptr<TextureAsset>> m_materialTextureAssetsByImageResourceID;
	std::unordered_map<uint32_t, std::weak_ptr<TextureAsset>> m_streamingTexturesByID;
	std::unordered_map<uint32_t, uint64_t> m_textureStreamingMetadataRevisions;
	std::vector<uint32_t> m_activeTextureStreamingFeedbackIDs;
	std::unordered_set<uint32_t> m_activeTextureStreamingFeedbackIDSet;
	std::mutex m_textureStreamingFeedbackMutex;
	std::vector<std::pair<uint32_t, uint32_t>> m_pendingTextureStreamingFeedback;
	std::vector<uint32_t> m_dirtyMaterialIDs;
	std::unordered_set<uint32_t> m_dirtyMaterialIDSet;
	std::vector<uint32_t> m_dirtyTextureStreamingIDs;
	std::unordered_set<uint32_t> m_dirtyTextureStreamingIDSet;
	std::vector<uint32_t> m_texturesNeedingUploadAdvance;
	std::unordered_set<uint32_t> m_texturesNeedingUploadAdvanceSet;
	std::chrono::steady_clock::time_point m_lastMaterialUpdateStatsLog = {};
	uint64_t m_textureDirtyReasonFeedback = 0;
	uint64_t m_textureDirtyReasonIdleCoarsen = 0;
	uint64_t m_textureDirtyReasonTrackMaterial = 0;
	uint64_t m_textureDirtyReasonUploadStateRevision = 0;
	uint64_t m_textureDirtyReasonUploadPending = 0;
	uint64_t m_textureDirtyReasonOther = 0;
	uint32_t m_textureStreamingMetadataCapacity = 1u;
};
