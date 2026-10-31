#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
	// Set by the texture pipeline: it writes the cache itself, and onFinished runs once the
	// job is Ready or Failed.
	bool pipelineOwnsCache = false;
	std::function<void()> onFinished;
};

class TextureProcessingManager {
public:
	static TextureProcessingManager& GetInstance();

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
	// ---- Texture pipeline stages (src/Assets/Textures/Pipeline) ----
	struct ProcessedSource {
		std::shared_ptr<TextureSourceData> prepared;  // the GPU BC7 job's input when gpuCompression
		std::shared_ptr<TextureSourceData> result;    // the processed texture otherwise
		bool gpuCompression = false;
	};
	std::string CacheKey(const TextureFileMeta& meta) const { return BuildProcessingCacheKey(meta); }
	// The format processing compresses to, when it block-compresses an uncompressed source.
	std::optional<rhi::Format> PredictedFormat(const TextureFileMeta& meta) const;
	static std::wstring ConditionedCachePath(const std::string& cacheKey);
	ProcessedSource Process(const std::shared_ptr<TextureSourceData>& source, const TextureFileMeta& meta) const;
	static std::shared_ptr<TextureSourceData> FinalizeOnCpu(
		const std::shared_ptr<TextureSourceData>& prepared, const TextureFileMeta& meta);
	// Writes the processed texture to the cache; the conditioned cache path, empty on failure.
	static std::wstring WriteCache(const std::string& cacheKey, const TextureSourceData& data);
	// A GPU BC7 job for prepared data (TextureFactory::SubmitBC7CompressionJob); onFinished
	// runs when it is Ready or Failed. The pipeline writes its cache.
	static std::shared_ptr<TextureProcessingJobHandle> MakePipelineGpuJob(
		std::shared_ptr<TextureSourceData> prepared, const TextureFileMeta& meta,
		std::string cacheKey, std::function<void()> onFinished);

	StochasticTextureArtifactResult RequestStochasticArtifactsBlocking(
		const std::shared_ptr<TextureSourceData>& sourceData,
		const TextureFileMeta& meta,
		const StochasticTextureArtifactSettings& settings);

private:
	TextureProcessingManager() = default;

	std::string BuildProcessingCacheKey(
		const TextureFileMeta& meta) const;

};
