#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string_view>

#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Render/Runtime/FrameWorkQueue.h"

namespace org { class PixelBuffer; }
namespace org { class Sampler; }
namespace org { class BufferView; }
namespace org { class Buffer; }
struct TextureProcessingJobHandle;
class MaterialTextureTransferService;

namespace org::runtime {
    class IReadbackService;
    class IUploadService;
}

// Central API for textures that have initial texel data.
class TextureFactory {
public:
    static std::unique_ptr<TextureFactory> CreateUnique(std::shared_ptr<org::runtime::IUploadService> uploadService) {
		return std::unique_ptr<TextureFactory>(new TextureFactory(std::move(uploadService)));
    }
    void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService) { m_uploadService = std::move(uploadService); }
    // Owned initial texel bytes for a texture creation request.
	// Subresource order is: [slice0 mip0..mipN-1, slice1 ...].
    struct TextureInitialData {
        std::vector<std::shared_ptr<std::vector<uint8_t>>> subresources;

        bool Empty() const noexcept { return subresources.empty(); }

        static TextureInitialData FromBytes(const std::vector<std::shared_ptr<std::vector<uint8_t>>>& bytes) {
            TextureInitialData d;
            d.subresources = bytes;
            return d;
        }
    };

    std::shared_ptr<org::PixelBuffer> CreateAlwaysResidentPixelBuffer(
        org::TextureDescription desc,
        TextureInitialData initialData,
        std::string_view debugName = {},
        bool preserveAlphaCoverage = false,
        bool forceSrgbMipEncoding = false,
        uint32_t maxMipLevels = 0u) const;

	// Creates a final material residency image whose upload and immutable SRV
	// transition are owned outside the render graph.
	std::shared_ptr<org::PixelBuffer> CreateMaterialResidentPixelBuffer(
		org::TextureDescription desc,
		TextureInitialData initialData,
		std::string_view debugName = {},
		uint32_t maxMipLevels = 0u) const;
	void SetMaterialTextureTransferService(MaterialTextureTransferService* service) {
		m_materialTextureTransferService = service;
	}

    std::shared_ptr<org::RenderPass> GetMipmappingPass() const { return m_mipmappingPass; }
    std::shared_ptr<org::RenderPass> GetBC7CompressionPass() const { return m_bc7CompressionPass; }
    std::shared_ptr<org::RenderPass> GetBC7CompressionCopyPass() const { return m_bc7CompressionCopyPass; }
    std::shared_ptr<org::RenderPass> GetBC7CompressionReadbackPass() const { return m_bc7CompressionReadbackPass; }

    void SetReadbackService(std::shared_ptr<org::runtime::IReadbackService> readbackService);
    bool SubmitBC7CompressionJob(
        const std::shared_ptr<TextureProcessingJobHandle>& handle,
        std::string_view debugName = {}) const;

private:
	MaterialTextureTransferService* m_materialTextureTransferService = nullptr;

    struct BC7CompressionSubresource {
        rhi::CopyableFootprint footprint{};
        uint32_t mip = 0;
        uint32_t slice = 0;
    };

    struct BC7CompressionJob {
        enum class Stage : uint8_t {
            WaitingForSourceUpload,
            ReadyForCompression,
            CompressionRecorded,
            CopyRecorded,
            ReadbackRecorded,
            Completed,
        };

        ~BC7CompressionJob()
        {
            if (inFlightCounter) {
                inFlightCounter->fetch_sub(1u, std::memory_order_acq_rel);
            }
        }

        std::string debugName;
        std::shared_ptr<TextureProcessingJobHandle> handle;
        std::shared_ptr<org::PixelBuffer> workingTexture;
        std::shared_ptr<org::PixelBuffer> compressedTexture;
        std::shared_ptr<org::Buffer> blockBuffer;
        std::vector<BC7CompressionSubresource> subresources;
        std::shared_ptr<std::atomic_uint32_t> inFlightCounter;
        std::atomic<Stage> stage = Stage::WaitingForSourceUpload;
        std::atomic<uint32_t> stageFrameIndex = UINT32_MAX;
        std::atomic<uint32_t> sourceUploadWaitExecutions = 4u;
        uint64_t outputByteSize = 0;
        bool outputHasFullMipChain = true;
    };

    class MipmappingPass;
    class BC7CompressionPass;
    class BC7CompressionCopyPass;
    class BC7CompressionReadbackPass;
    struct BC7CompressionCopyFrameData;
    struct BC7CompressionReadbackFrameData;

    explicit TextureFactory(std::shared_ptr<org::runtime::IUploadService> uploadService);

	std::shared_ptr<org::RenderPass> m_mipmappingPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionCopyPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionReadbackPass;
    std::shared_ptr<org::runtime::IUploadService> m_uploadService;
    std::shared_ptr<std::atomic_uint32_t> m_bc7InFlightJobs = std::make_shared<std::atomic_uint32_t>(0u);
};
