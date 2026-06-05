#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <string>
#include <variant>

#include "Import/Filetypes.h"
#include "Materials/MaterialTextureStreaming.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Factories/TextureFactory.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Resources/Sampler.h"

struct RenderContext;
struct TextureProcessingJobHandle;
enum class TextureProcessingJobState : uint8_t;

enum class TextureSemantic : uint8_t {
    Unknown = 0,
    BaseColor,
    Emissive,
    Normal,
    Height,
    AO,
    Opacity,
    Metallic,
    Roughness,
    MetallicRoughness,
    OpenPBRColor,
    OpenPBRScalar,
};

enum class NormalMapConvention : uint8_t {
    DirectX = 0,
    OpenGL,
};

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
    settings.preferSRGB = preferSRGB;
    settings.preservePackedChannels = preservePackedChannels;
    settings.normalConvention = normalConvention;
    settings.sourceIdentity = std::move(sourceIdentity);
    return settings;
}

struct TextureSourceData {
    using BytesPtr = std::shared_ptr<std::vector<uint8_t>>;
    using BytesList = std::vector<BytesPtr>;

    TextureDescription desc;
    BytesList subresources;
    bool hasFullMipChain = false;
    bool isBlockCompressed = false;
};

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

enum class TextureReloadJobState : uint8_t {
    Queued = 0,
    BuildingSourceData,
    Ready,
    Failed,
};

struct TextureReloadJobHandle {
    std::atomic<TextureReloadJobState> state = TextureReloadJobState::Queued;
    std::mutex mutex;
    uint32_t targetTopMip = 0;
    uint32_t sourceTotalMipCount = 1;
    uint32_t sourceFullWidth = 0;
    uint32_t sourceFullHeight = 0;
    std::shared_ptr<TextureSourceData> sourceData;
    std::string error;
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
    std::shared_ptr<PixelBuffer> uploadedImage;
    DirectStorageAsyncRequestHandle requestHandle;
    std::string error;
};

// Helper for std::visit with multiple lambdas
template<class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

class TextureAsset {
public:
    using BytesPtr = std::shared_ptr<std::vector<uint8_t>>;
    using BytesList = std::vector<BytesPtr>;
    // Variant for different ways an asset might be stored
    // On-disk file, in-memory bytes, etc.
    using StorageVariant = std::variant<
        std::monostate,
        BytesList,
        std::string,
        std::shared_ptr<PixelBuffer>>;
    StorageVariant m_initialStorage;

    static std::shared_ptr<TextureAsset> CreateShared(TextureDescription desc,
        StorageVariant initialStorage,
        std::shared_ptr<Sampler> defaultSampler,
        TextureFileMeta meta) {
	    return std::shared_ptr<TextureAsset>(new TextureAsset(
            std::move(desc),
            std::move(initialStorage),
            std::move(defaultSampler),
            std::move(meta)
		));
    }

	// Resolve to a vector of bytes
    const BytesList& ResolveToBytes() const
    {
        // Something we can safely return by reference in "empty" cases.
        static const BytesList kEmpty = {};

        return std::visit(Overloaded{
            [&](std::monostate) -> const BytesList& {
                throw std::runtime_error("ResolveToBytes: no initial storage set");
                return kEmpty;
            },
            [&](const BytesList& p) -> const BytesList& {
                return p;
            },
            [&](const std::string& path) -> const BytesList& {
				throw std::runtime_error("Not implemented");
            },
        [&](const std::shared_ptr<PixelBuffer>& pb) -> const BytesList& {
	            throw std::runtime_error("ResolveToBytes: Not implemented for PixelBuffer");
	            return kEmpty;
			},
            }, m_initialStorage);
    }

    PixelBuffer& Image() const { return *m_image; }
    std::shared_ptr<PixelBuffer> ImagePtr() const { return m_image; }

    Sampler& SamplerState() const { return *m_sampler; }
    UINT SamplerDescriptorIndex() const { return m_sampler->GetDescriptorIndex(); }

    const TextureFileMeta& Meta() const { return m_meta; }
    TextureFileMeta& Meta() { return m_meta; }
    const TextureDescription& Description() const { return m_desc; }

    const TextureProcessingSettings& ProcessingSettings() const { return m_meta.processing; }
    void SetProcessingSettings(TextureProcessingSettings settings);

    const TextureStreamingState& GetStreamingState() const { return m_streamingState; }
    uint32_t GetStreamingTextureID() const { return m_streamingState.streamingTextureID; }
    bool IsMipStreamingEligible() const { return m_streamingState.eligible; }
    bool IsMipStreamingEnabled() const { return m_streamingState.enabled; }
    bool IsUsingFallbackImage() const { return m_hasUploadedPlaceholder && !m_hasUploadedFinalImage; }
    bool HasUsableImage() const { return m_image && m_image->HasValidBackingResource(); }
    uint64_t GetBindingRevision() const { return m_streamingState.bindingRevision; }
    uint64_t GetStreamingStateRevision() const { return m_streamingState.stateRevision; }
    bool HasPendingUploadWork() const {
        const bool needsStreamingReload =
            m_hasUploadedFinalImage &&
            HasStreamingSourceData() &&
            ((m_streamingState.enabled &&
              m_streamingState.pendingTopMip != m_streamingState.residency.residentTopMip) ||
             (!m_streamingState.enabled && m_streamingState.residency.residentTopMip != 0u));
        const bool hasAsyncHandle =
            m_processingHandle != nullptr ||
            m_reloadHandle != nullptr ||
            m_directStorageReloadHandle != nullptr;
        return !HasUsableImage() ||
            needsStreamingReload ||
            hasAsyncHandle;
    }
    TexturePendingDebugInfo GetPendingDebugInfo() const;
    const std::string& DebugName() const { return m_name; }
    uint32_t GetFullMip0Width() const { return m_sourceFullWidth != 0u ? m_sourceFullWidth : GetWidth(); }
    uint32_t GetFullMip0Height() const { return m_sourceFullHeight != 0u ? m_sourceFullHeight : GetHeight(); }
    bool ApplyStreamingSystemRequest(uint32_t topMip, uint64_t frameIndex = 0, bool forceResidencyChange = false);
    void EnableMipStreaming(bool enabled);
    void SetMipStreamingSuppressed(bool suppressed);

    void AdoptUploadedImage(std::shared_ptr<PixelBuffer> image);
    void RecordLoadPath(TextureLoadPathTelemetry path, std::string detail = {});
    void RecordUploadPath(TextureUploadPathTelemetry path, std::string detail = {});

    std::shared_ptr<TextureSourceData> BuildSourceData();
    std::shared_ptr<TextureSourceData> BuildProcessingSourceData();

    void SetName(const std::string& name)
    {
        m_name = name;
        if (HasUsableImage()) {
            m_image->SetName(name);
        }
    }

    DirectStorageAsyncRequestHandle QueueInitialDirectStorageUploadIfNeeded();
    void EnsureUploaded(const TextureFactory& factory);

    unsigned int GetWidth() const {
        return m_desc.imageDimensions[0].width;
    }
    unsigned int GetHeight() const {
        return m_desc.imageDimensions[0].height;
    }
    void SetGenerateMipmaps(bool generate) {
	    m_desc.generateMipMaps = generate;
    }

private:
    static uint32_t NextStreamingTextureID();
    TextureAsset(TextureDescription desc,
        StorageVariant initialStorage,
        std::shared_ptr<Sampler> defaultSampler,
        TextureFileMeta meta)
        : m_desc(std::move(desc))
        , m_initialStorage(std::move(initialStorage))
        , m_sampler(defaultSampler ? std::move(defaultSampler) : Sampler::GetDefaultSampler())
        , m_meta(std::move(meta)) {
        m_streamingState.streamingTextureID = NextStreamingTextureID();
        if (std::holds_alternative<std::shared_ptr<PixelBuffer>>(m_initialStorage)) { // Already initialized
            m_image = std::get<std::shared_ptr<PixelBuffer>>(m_initialStorage);
			m_hasUploadedFinalImage = true;
        }
		if (std::holds_alternative<std::string>(m_initialStorage)) { // Store path for potential re-use
            m_initialDataString = std::get<std::string>(m_initialStorage);
		}
        if (std::holds_alternative<BytesList>(m_initialStorage)) {
            m_originalSourceDesc = m_desc;
            m_originalSourceBytes = std::get<BytesList>(m_initialStorage);
        }
        RefreshStreamingStateFromDescription();
        if (m_streamingState.eligible && !m_suppressMipStreaming && IsMaterialTextureStreamingEnabledSetting()) {
            m_streamingState.enabled = true;
			ApplyStreamingBootstrapTopMip();
            InvalidateResidentImageForStreamingRequest();
        }
    }
	TextureDescription m_desc;
    std::shared_ptr<PixelBuffer> m_image;
    std::shared_ptr<Sampler> m_sampler;
    TextureFileMeta m_meta;
	std::shared_ptr<TextureProcessingJobHandle> m_processingHandle;
    std::shared_ptr<TextureReloadJobHandle> m_reloadHandle;
    std::shared_ptr<TextureDirectStorageReloadJobHandle> m_directStorageReloadHandle;
    TextureStreamingState m_streamingState;
    bool m_suppressMipStreaming = false;
	uint32_t m_sourceTotalMipCount = 0;
    uint32_t m_sourceFullWidth = 0;
    uint32_t m_sourceFullHeight = 0;
    std::string m_initialDataString;
    TextureDescription m_originalSourceDesc;
    BytesList m_originalSourceBytes;
    std::string m_name;
	bool m_hasUploadedPlaceholder = false;
	bool m_hasUploadedFinalImage = false;
    TextureLoadPathTelemetry m_lastReportedLoadPath = TextureLoadPathTelemetry::Unknown;
    TextureUploadPathTelemetry m_lastReportedUploadPath = TextureUploadPathTelemetry::Unknown;

    void RefreshStreamingStateFromDescription();
	void UpdateSourceShapeFromDescription(const TextureDescription& desc, uint32_t totalMipCountHint = 0u);
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
};
