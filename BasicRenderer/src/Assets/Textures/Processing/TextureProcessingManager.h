#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <DirectXMath.h>

#include "BasicRenderer/Assets/Texture.h"
#include <BasicRenderer/Assets/TextureProcessingRequests.h>

enum class TextureProcessingJobState : uint8_t {
	Queued = 0,
	CpuPreparing,
	GpuReadyToSubmit,
	GpuSubmitted,
	ReadbackPending,
	Ready,
	Failed,
};

struct TextureProcessingJobHandle {
	std::atomic<TextureProcessingJobState> state = TextureProcessingJobState::Queued;
	std::mutex mutex;
	TextureFileMeta requestMeta;
	std::string processingKey;
	std::string cacheKey;
	std::string conditionedCachePath;
	std::shared_ptr<TextureSourceData> preparedSourceData;
	std::shared_ptr<TextureSourceData> result;
	std::shared_ptr<org::PixelBuffer> uploadedImage;
	bool loadedFromCache = false;
	bool requiresGpuCompression = false;
	bool completedOnGpu = false;
	std::string error;
};

class TextureProcessingManager {
public:
	static TextureProcessingManager& GetInstance();

	std::shared_ptr<TextureProcessingJobHandle> RequestProcessing(
		const std::shared_ptr<TextureSourceData>& sourceData,
		const TextureFileMeta& meta);
	void MarkGpuJobSubmitted(const std::shared_ptr<TextureProcessingJobHandle>& handle);
	void MarkGpuJobReadbackPending(const std::shared_ptr<TextureProcessingJobHandle>& handle);
	void CompleteGpuProcessing(
		const std::shared_ptr<TextureProcessingJobHandle>& handle,
		std::shared_ptr<TextureSourceData> result,
		std::shared_ptr<org::PixelBuffer> uploadedImage = {},
		bool writeCacheArtifact = true);
	void FailProcessing(const std::shared_ptr<TextureProcessingJobHandle>& handle, std::string error);

	bool ShouldProcess(const TextureFileMeta& meta) const;
	bool NeedsProcessing(const TextureSourceData& sourceData, const TextureFileMeta& meta) const;
	std::wstring GetExistingCachePathForFile(const TextureFileMeta& meta) const;
	StochasticTextureArtifactResult RequestStochasticArtifactsBlocking(
		const std::shared_ptr<TextureSourceData>& sourceData,
		const TextureFileMeta& meta,
		const StochasticTextureArtifactSettings& settings);

private:
	TextureProcessingManager() = default;

	std::string BuildProcessingCacheKey(
		const TextureFileMeta& meta) const;
	std::string BuildProcessingJobKey(
		const std::shared_ptr<TextureSourceData>& sourceData,
		const TextureFileMeta& meta) const;

	std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<TextureProcessingJobHandle>> m_jobsByKey;
};
