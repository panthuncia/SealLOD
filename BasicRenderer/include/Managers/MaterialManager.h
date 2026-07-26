#pragma once

#include <memory>
#include <mutex>
#include <chrono>

#include "Materials/Material.h"
#include "Managers/TextureStreamingManager.h"
#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/ResourceGroup.h"
#include "Render/IndirectCommand.h"
#include "Render/MaterialCompileFlagsSlotRegistry.h"
#include "Render/RasterBucketFlags.h"

namespace rg::runtime {
class IReadbackService;
}

class TextureFactory;
class CopyPass;

// Manages buffers for per-material-compile-flag work (e.g., visibility buffer per-material)
class MaterialManager : public IResourceProvider {
public:
	static std::unique_ptr<MaterialManager> CreateUnique() {
		return std::unique_ptr<MaterialManager>(new MaterialManager());
	}
	unsigned int AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u);
	bool ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u);
	bool TryGetCompileFlagsSlot(MaterialCompileFlags flags, unsigned int& slot) const;
	unsigned int GetMaterialSlot(unsigned int materialID, std::optional<PerMaterialCB> data = std::nullopt);
	unsigned int AcquireRasterBucket(MaterialRasterFlags rasterFlags, unsigned int count = 1u);
	void ReleaseRasterBucket(MaterialRasterFlags rasterFlags);

	unsigned int IncrementMaterialUsageCount(Material& material, TextureFactory* textureFactory = nullptr, unsigned int count = 1u);
	void DecrementMaterialUsageCount(const Material& material);
	void InitializeTextureStreaming(TextureFactory& textureFactory, uint32_t framesInFlight);
	void ShutdownTextureStreaming();
	void BeginTextureStreamingFeedbackFrame(uint64_t frameIndex);
	void ProcessPendingMaterialUpdates(uint64_t frameIndex);
	std::shared_ptr<CopyPass> CreateTextureStreamingFeedbackReadbackPass();
	void SetTextureStreamingFeedbackSuppressed(bool suppressed) { m_textureStreamingFeedbackSuppressed = suppressed; }
	MaterialTextureStreamingStats GetMaterialTextureStreamingStats() const;
	void RegisterStreamingTexture(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory);
	TextureStreamingManager* GetTextureStreamingManager() const { return m_textureStreamingManager.get(); }

	void UpdateMaterialDataBuffer(Material& material);
	void MarkMaterialDirty(Material& material);
	void UpdateOpenPBRMaterialDataBuffer(unsigned int materialSlot, const PerMaterialOpenPBRCB& data) {
		m_perMaterialOpenPBRDataBuffer->UpdateAt(materialSlot, data);
	}

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

	void CommitGpuVisibleSnapshot();
	const std::vector<unsigned int>& GetActiveCompileFlagsSlots() const { return m_publishedActiveCompileFlagsSlots; }
	const std::vector<MaterialCompileFlags>& GetActiveCompileFlags() const { return m_publishedActiveCompileFlags; }
	unsigned int GetCompileFlagsSlotsUsed() const { return m_publishedCompileFlagsSlotsUsed; }

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
	bool MaterialTextureAssetBindingsChanged(const Material& material) const;
	void FlushDirtyMaterial(Material& material, TextureFactory* textureFactory = nullptr);
	void EnsureMaterialBufferCapacity(unsigned int requiredSlots);
	void EnsureCompileFlagsBufferCapacity(unsigned int requiredSlots);
	std::vector<std::shared_ptr<Resource>> CollectActiveMaterialTextureResources() const;

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<IResourceResolver>, ResourceIdentifier::Hasher> m_resolvers;
	std::shared_ptr<ResourceGroup> m_activeMaterialTextureGroup;
	std::unordered_map<uint64_t, uint32_t> m_materialTextureUsageCounts;
	std::unordered_map<uint32_t, std::vector<std::shared_ptr<Resource>>> m_trackedMaterialTextures;
	std::unordered_map<uint32_t, Material*> m_activeMaterialsByID;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_materialTextureStreamingBindingIDs;
	std::unordered_map<uint32_t, std::vector<uint32_t>> m_materialTextureStreamingTextureIDs;
	bool m_textureStreamingFeedbackSuppressed = false;
	MaterialCompileFlagsSlotRegistry m_compileFlagsRegistry;
	std::vector<unsigned int> m_publishedActiveCompileFlagsSlots;
	std::vector<MaterialCompileFlags> m_publishedActiveCompileFlags;
	unsigned int m_publishedCompileFlagsSlotsUsed = 1;

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
	unsigned int m_materialBufferCapacity = 0u;

	static constexpr unsigned int kBufferGrowthSize = 100;
	static constexpr unsigned int kInitialMaterialBufferCapacity = 4096;
	static constexpr bool kForceMaterialBufferResizeEveryMaterial = false;

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
	std::unique_ptr<TextureStreamingManager> m_textureStreamingManager;
	std::vector<uint32_t> m_dirtyMaterialIDs;
	std::unordered_set<uint32_t> m_dirtyMaterialIDSet;
	std::chrono::steady_clock::time_point m_lastMaterialUpdateStatsLog = {};
};
