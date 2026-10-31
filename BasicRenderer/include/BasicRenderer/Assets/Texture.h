#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <string>
#include <string_view>
#include <variant>

#include "BasicRenderer/Assets/Import/Filetypes.h"
#include <BasicRenderer/Assets/TextureTypes.h>
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "Resources/PixelBuffer.h"
#include "Resources/TextureDescription.h"
#include "Render/Runtime/IDescriptorService.h"

class TextureFactory;
namespace br::textures { struct TextureRequest; }
#include "Resources/Sampler.h"
#include "BasicRenderer/Assets/TextureResidency.h"
#include <BasicRenderer/Streaming/DirectStorageRequest.h>

struct RenderContext;
struct TextureProcessingJobHandle;
enum class TextureProcessingJobState : uint8_t;

enum class TextureLoadPathTelemetry : uint8_t {
    Unknown = 0,
    DirectStorageGpuDirect,
    DirectStorageSystemMemoryRead,
    CpuFileRead,
    MemoryMappedFileRead,
    InMemoryContainer,
    DeferredFileReference,
};

enum class TextureUploadPathTelemetry : uint8_t {
    Unknown = 0,
    DirectStorageGpuDirect,
    CpuImmediateUpload,
    AsyncProcessingPlaceholder,
    AsyncProcessingReadyUpload,
    ProcessingCacheUpload,
    ProcessingFailedFallback,
    DeferredPlaceholder,
};

struct TextureProcessingSettings {
    TextureSemantic semantic = TextureSemantic::Unknown;
    bool isParticipatingMaterialTexture = false;
    bool requestMipChain = false;
    bool requestBlockCompression = false;
    bool allowAsyncPlaceholder = false;
    bool allowCpuBootstrapBeforeAsyncProcessing = false;
    bool preferSRGB = false;
    bool preservePackedChannels = false;
    NormalMapConvention normalConvention = NormalMapConvention::DirectX;
	// Zero retains the complete chain. A value N retains mip indices [0, N).
	uint32_t maxMipLevels = 0;
    std::string sourceIdentity;
};

inline TextureProcessingSettings MakeMaterialTextureProcessingSettings(
    TextureSemantic semantic,
    bool preferSRGB,
    std::string sourceIdentity = {},
    bool preservePackedChannels = false,
    NormalMapConvention normalConvention = NormalMapConvention::DirectX)
{
    TextureProcessingSettings settings{};
    settings.semantic = semantic;
    settings.isParticipatingMaterialTexture = true;
    settings.requestMipChain = true;
    settings.requestBlockCompression = semantic != TextureSemantic::Height;
    settings.allowAsyncPlaceholder = true;
    settings.allowCpuBootstrapBeforeAsyncProcessing = true;
    settings.preferSRGB = preferSRGB;
    settings.preservePackedChannels = preservePackedChannels;
    settings.normalConvention = normalConvention;
    settings.sourceIdentity = std::move(sourceIdentity);
    return settings;
}

struct TextureSourceData {
    using BytesPtr = std::shared_ptr<std::vector<uint8_t>>;
    using BytesList = std::vector<BytesPtr>;

    org::TextureDescription desc;
    BytesList subresources;
    bool hasFullMipChain = false;
    bool isBlockCompressed = false;
};

std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromConditionedCacheFilePath(
    const std::string& path,
	const std::string& reason = {});
void ReleaseSharedProcessingPlaceholderTextures();
std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromDDSFilePath(
    const std::string& path,
    bool preferSRGB,
    const std::string& reason = {});
std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromFilePath(
    const std::string& path,
    bool preferSRGB,
    const std::string& reason = {});
// Decodes an encoded image (DDS, HDR, TGA, or anything WIC reads) held in memory.
std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromImageBytes(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    bool preferSRGB,
    std::string_view formatHint,
    const std::string& label);

//enum class ImageFiletype {
//	UNKNOWN,
//	HDR,
//	DDS,
//	TGA,
//	WIC
//};

struct TextureFileMeta {
	std::string filePath;
	ImageFiletype fileType = ImageFiletype::UNKNOWN;
	ImageLoader loader{};
	bool alphaIsAllOpaque = true;
    bool preferSRGB = false;
    bool isProcessingCacheArtifact = false;
    TextureLoadPathTelemetry loadPath = TextureLoadPathTelemetry::Unknown;
    TextureUploadPathTelemetry uploadPath = TextureUploadPathTelemetry::Unknown;
    std::string loadPathDetail;
    std::string uploadPathDetail;
    TextureProcessingSettings processing = {};
};

struct TextureMipResidencyWindow {
    uint32_t totalMipCount = 1;
    uint32_t residentTopMip = 0;
    uint32_t residentMipCount = 1;

    uint32_t ResidentLastMip() const {
        if (residentMipCount == 0) {
            return residentTopMip;
        }
        return residentTopMip + residentMipCount - 1u;
    }

    bool IsFullChainResident() const {
        return residentTopMip == 0u && residentMipCount >= totalMipCount;
    }
};

struct TextureStreamingState {
    uint32_t streamingTextureID = 0;
    bool eligible = false;
    bool enabled = false;
    TextureMipResidencyWindow residency = {};
    uint32_t requestedTopMip = 0;
    uint32_t pendingTopMip = 0;
    uint32_t lastFeedbackTopMip = UINT32_MAX;
    uint64_t lastSeenFrame = 0;
    uint64_t stateRevision = 0;
    uint64_t bindingRevision = 0;
};

struct TexturePendingDebugInfo {
    std::string label;
    std::string debugName;
    std::string sourceIdentity;
    std::string filePath;
    std::string initialData;
    bool hasUsableImage = false;
    bool hasFinalImage = false;
    bool hasPlaceholder = false;
    bool needsStreamingReload = false;
    bool hasProcessingHandle = false;
    bool hasReloadHandle = false;
    bool hasDirectStorageHandle = false;
    bool isProcessingCacheArtifact = false;
    uint32_t streamingTextureID = 0;
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
};

enum class TextureDirectStorageReloadJobState : uint8_t {
    Queued = 0,
    CreatingResource,
    Uploading,
    Ready,
    Failed,
};

struct TextureDirectStorageReloadJobHandle {
    std::atomic<TextureDirectStorageReloadJobState> state = TextureDirectStorageReloadJobState::Queued;
    std::atomic_bool cancelRequested = false;
    std::mutex mutex;
    std::atomic<uint32_t> targetTopMip = 0;
    std::shared_ptr<org::PixelBuffer> uploadedImage;
    DirectStorageAsyncRequestHandle requestHandle;
    std::string error;
};

class TextureAsset {
public:
    // A texture loaded by the texture pipeline (br::textures::RequestTexture); its shape is
    // unknown until the pipeline reports it (ApplyPipelineSourceShape).
    static std::shared_ptr<TextureAsset> CreateForPipeline(
        std::shared_ptr<const br::textures::TextureRequest> request, std::string cacheKey);
    bool IsPipelineTexture() const { return m_pipelineRequest != nullptr; }
    const std::shared_ptr<const br::textures::TextureRequest>& PipelineRequest() const { return m_pipelineRequest; }
    const std::string& PipelineCacheKey() const { return m_pipelineCacheKey; }
    // The full source shape (all mips) the pipeline found, which decides mip streaming.
    void ApplyPipelineSourceShape(const org::TextureDescription& fullDesc);
    // Whether the resident image is missing or is not the mip window streaming wants.
    bool NeedsPipelineResidency() const;
    // The top mip of the window streaming wants resident.
    uint32_t DesiredResidentTopMip() const { std::scoped_lock lock(m_uploadAdvanceMutex); return GetDesiredResidentTopMip(); }
    // Makes a pipeline residency job's image the texture's prepared image.
    void AdoptPipelineImage(std::shared_ptr<org::PixelBuffer> image) {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        AdoptUploadedImage(std::move(image));
    }
    // Ends the load after a terminal failure; the texture keeps its fallback.
    void FailPipelineLoad(std::string_view reason) { MarkTerminalLoadFailure(reason); }

    // A texture over an image something else produced (a GPU-generated cubemap); not a load.
    static std::shared_ptr<TextureAsset> CreateFromImage(std::shared_ptr<org::PixelBuffer> image,
        std::shared_ptr<org::Sampler> sampler = nullptr, std::string name = {});

    struct PublishedBindingSnapshot {
        std::shared_ptr<org::PixelBuffer> image;
        uint64_t bindingRevision = 0;
        TextureStreamingState streamingState{};
    };
    PublishedBindingSnapshot GetPublishedBindingSnapshot() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return {m_publishedImage, m_publishedBindingRevision, m_publishedStreamingState};
    }
    struct PreparedBindingSnapshot {
        std::shared_ptr<org::PixelBuffer> image;
        TextureStreamingState streamingState{};
    };
    PreparedBindingSnapshot GetPreparedBindingSnapshot() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return {m_image, m_streamingState};
    }
    std::shared_ptr<org::PixelBuffer> ImagePtr() const {
        return GetPublishedBindingSnapshot().image;
    }
    std::shared_ptr<org::PixelBuffer> PreparedImagePtr() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_image;
    }
    bool PublishPreparedImage(
        uint64_t bindingRevision,
        const std::shared_ptr<org::PixelBuffer>& image,
        std::shared_ptr<org::PixelBuffer>* replacedPublishedImage = nullptr);
    bool RejectPreparedImage(uint64_t bindingRevision, const std::shared_ptr<org::PixelBuffer>& image);

    org::Sampler& SamplerState() const { return *m_sampler; }
	void SetSampler(std::shared_ptr<org::Sampler> sampler) {
		std::scoped_lock lock(m_uploadAdvanceMutex);
		m_sampler = sampler ? std::move(sampler) : org::Sampler::GetDefaultSampler();
	}
    UINT SamplerDescriptorIndex(org::runtime::IDescriptorService& descriptorService) const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_sampler->GetDescriptorIndex(descriptorService);
    }

    const TextureFileMeta& Meta() const { return m_meta; }
    TextureFileMeta& Meta() { return m_meta; }
    const org::TextureDescription& Description() const { return m_desc; }
    rhi::Format Format() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_desc.format; }

    const TextureProcessingSettings& ProcessingSettings() const { return m_meta.processing; }
    void SetProcessingSettings(TextureProcessingSettings settings);

    TextureStreamingState GetStreamingState() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_streamingState;
    }
    uint32_t GetStreamingTextureID() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_streamingState.streamingTextureID; }
    bool IsMipStreamingEligible() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_streamingState.eligible; }
    bool IsMipStreamingEnabled() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_streamingState.enabled; }
    // Set once the texture's source could not be turned into an image; its
    // placeholder is then final and must not hold back the renderables using it.
    bool HasTerminalLoadFailure() const { return m_terminalLoadFailure.load(std::memory_order_acquire); }
    bool IsUsingFallbackImage() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_hasUploadedPlaceholder && !m_hasUploadedFinalImage; }
    bool HasUsableImage() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_image && m_image->HasValidBackingResource(); }
    bool IsResidentFinalImage() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_image && m_image->HasValidBackingResource() && m_hasUploadedFinalImage && !m_hasUploadedPlaceholder;
    }
    uint64_t GetBindingRevision() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_streamingState.bindingRevision; }
    uint64_t GetStreamingStateRevision() const { std::scoped_lock lock(m_uploadAdvanceMutex); return m_streamingState.stateRevision; }
    bool HasPendingUploadWork() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_pipelineRequest ? NeedsPipelineResidency() : !HasUsableImage();
    }
    TexturePendingDebugInfo GetPendingDebugInfo() const;
    const std::string& DebugName() const { return m_name; }
    uint32_t GetFullMip0Width() const
    {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_sourceFullWidth != 0u ? m_sourceFullWidth : m_desc.imageDimensions[0].width;
    }
    uint32_t GetFullMip0Height() const
    {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_sourceFullHeight != 0u ? m_sourceFullHeight : m_desc.imageDimensions[0].height;
    }
    bool ApplyStreamingSystemRequest(uint32_t topMip, uint64_t frameIndex = 0, bool forceResidencyChange = false);
    void EnableMipStreaming(bool enabled);
    void SetMipStreamingSuppressed(bool suppressed);

    void AdoptUploadedImage(std::shared_ptr<org::PixelBuffer> image);
    void RecordLoadPath(TextureLoadPathTelemetry path, std::string detail = {});
    void RecordUploadPath(TextureUploadPathTelemetry path, std::string detail = {});

    void SetName(const std::string& name)
    {
		std::scoped_lock lock(m_uploadAdvanceMutex);
        m_name = name;
        if (HasUsableImage() && !m_hasUploadedPlaceholder) {
            m_image->SetName(name);
        }
    }

    // Drops the resident image of a streamed texture (height atlas eviction); a later load
    // makes it resident again.
    bool DropResidentImageForStreaming();

    unsigned int GetWidth() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_desc.imageDimensions[0].width;
    }
    unsigned int GetHeight() const {
        std::scoped_lock lock(m_uploadAdvanceMutex);
        return m_desc.imageDimensions[0].height;
    }
    void SetGenerateMipmaps(bool generate) {
		std::scoped_lock lock(m_uploadAdvanceMutex);
	    m_desc.generateMipMaps = generate;
    }

private:
    static uint32_t NextStreamingTextureID();
    TextureAsset(org::TextureDescription desc,
        std::shared_ptr<org::PixelBuffer> image,
        std::shared_ptr<org::Sampler> defaultSampler,
        TextureFileMeta meta)
        : m_desc(std::move(desc))
        , m_sampler(defaultSampler ? std::move(defaultSampler) : org::Sampler::GetDefaultSampler())
        , m_meta(std::move(meta)) {
        m_streamingState.streamingTextureID = NextStreamingTextureID();
        if (image) {
            m_image = std::move(image);
            m_publishedImage = m_image;
            m_hasUploadedFinalImage = true;
        }
        RefreshStreamingStateFromDescription();
    }
	org::TextureDescription m_desc;
	mutable std::recursive_mutex m_uploadAdvanceMutex;
    std::shared_ptr<org::PixelBuffer> m_image;
    std::shared_ptr<org::PixelBuffer> m_publishedImage;
    uint64_t m_publishedBindingRevision = 0;
    TextureStreamingState m_publishedStreamingState{};
    std::shared_ptr<org::Sampler> m_sampler;
    TextureFileMeta m_meta;
    TextureStreamingState m_streamingState;
    std::shared_ptr<const br::textures::TextureRequest> m_pipelineRequest;
    std::string m_pipelineCacheKey;
	uint64_t m_streamingRequestChangedFrame = 0;
    bool m_suppressMipStreaming = false;
	uint32_t m_sourceTotalMipCount = 0;
    uint32_t m_sourceFullWidth = 0;
    uint32_t m_sourceFullHeight = 0;
    std::string m_name;
	bool m_hasUploadedPlaceholder = false;
	bool m_hasUploadedFinalImage = false;
	std::atomic<bool> m_terminalLoadFailure{ false };
    TextureLoadPathTelemetry m_lastReportedLoadPath = TextureLoadPathTelemetry::Unknown;
    TextureUploadPathTelemetry m_lastReportedUploadPath = TextureUploadPathTelemetry::Unknown;

    void RefreshStreamingStateFromDescription();
	void SetPreparedImageLocked(std::shared_ptr<org::PixelBuffer> image) {
		m_image = std::move(image);
		// Material texture publication is owned by TextureStreamingManager even when
		// mip streaming is disabled or the texture is ineligible.  Otherwise an
		// upload can change the descriptor without producing an owner-dirty event.
		if (!m_meta.processing.isParticipatingMaterialTexture) {
			m_publishedImage = m_image;
		}
	}
	void UpdateSourceShapeFromDescription(const org::TextureDescription& desc, uint32_t totalMipCountHint = 0u);
    void ApplySourceShapeHint(uint32_t fullWidth, uint32_t fullHeight, uint32_t totalMipCount);
	void ApplyStreamingBootstrapTopMip();
    bool HasStreamingSourceData() const;
    uint32_t GetDesiredResidentTopMip() const;
    void InvalidateResidentImageForStreamingRequest();
    void SetRequestedTopMip(uint32_t topMip, uint64_t frameIndex = 0);
    void SetPendingTopMip(uint32_t topMip);
    void SetResidentMipWindow(uint32_t residentTopMip, uint32_t residentMipCount);
    void NoteTextureSeen(uint64_t frameIndex);
    void BumpStreamingStateRevision();
    void BumpBindingRevision();
    void MarkTerminalLoadFailure(std::string_view reason);
};
