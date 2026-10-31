#pragma once

#include <atomic>
#include "Interfaces/IDynamicDeclaredResources.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string_view>

#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/PixelBuffer.h"
#include "Resources/TextureDescription.h"
#include "Resources/Buffers/Buffer.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/IReadbackService.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
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
    // Any thread creates textures: the factory keeps only the producer (mailbox) side of each upload service.
    void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService) { m_uploadService.store(std::move(uploadService)); }
    // The texture job graph's upload queue (br::render::TextureJobGraph): a texture whose mips the GPU generates uploads its
    // source through it, so the job graph's own upload pass lands it ahead of the mip pass.
    void SetJobUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService);
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
    std::shared_ptr<org::RenderPass> GetMipHandoffPass() const { return m_mipHandoffPass; }
    // Whether any GPU texture job (mips, their handoff, BC7) has work left: the texture job graph executes only then.
    bool HasPendingGpuJobs() const;
    // Calls ready once the GPU has generated texture's mips and handed it to shader-resource state, on the thread that retires
    // the texture job graph's executions; what gates the texture's readiness. False, and no call, when it has no GPU mip job
    // pending: its image is final as created.
    bool WhenMipsReady(const org::PixelBuffer& texture, std::function<void()> ready) const;
    std::shared_ptr<org::RenderPass> GetBC7CompressionPass() const { return m_bc7CompressionPass; }
    std::shared_ptr<org::RenderPass> GetBC7CompressionCopyPass() const { return m_bc7CompressionCopyPass; }
    std::shared_ptr<org::RenderPass> GetBC7CompressionReadbackPass() const { return m_bc7CompressionReadbackPass; }

    void SetReadbackService(std::shared_ptr<org::runtime::IReadbackService> readbackService);
    // Whether BC7 jobs can run at all (their readback service is attached).
    bool CanRunBC7CompressionJobs() const;
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

                ~BC7CompressionJob() { ReleaseBudget(); }

        // Returns the job's estimate to the BC7 VRAM budget, once: when the job completes or fails, since a finished job
        // stays referenced by the texture job graph's last execution until that graph runs again, which it does only for
        // new jobs; the budget must not wait on that. Destruction releases it otherwise.
        void ReleaseBudget()
        {
            if (inFlightBytes && !budgetReleased.exchange(true, std::memory_order_acq_rel)) {
                inFlightBytes->fetch_sub(budgetBytes, std::memory_order_acq_rel);
            }
        }

        std::string debugName;
        std::shared_ptr<TextureProcessingJobHandle> handle;
        std::shared_ptr<org::PixelBuffer> workingTexture;
        std::shared_ptr<org::PixelBuffer> compressedTexture;
        std::shared_ptr<org::Buffer> blockBuffer;
        std::vector<BC7CompressionSubresource> subresources;
        // The job's estimated GPU memory, held against the BC7 VRAM budget while it lives.
        std::shared_ptr<std::atomic_uint64_t> inFlightBytes;
                uint64_t budgetBytes = 0;
        std::atomic_bool budgetReleased = false;
        std::atomic<Stage> stage = Stage::WaitingForSourceUpload;
        std::atomic<uint32_t> stageFrameIndex = UINT32_MAX;
        std::atomic<uint32_t> sourceUploadWaitExecutions = 4u;
        uint64_t outputByteSize = 0;
        bool outputHasFullMipChain = true;
    };

    class MipmappingPass;
    class MipHandoffPass;
    class BC7CompressionPass;
    class BC7CompressionCopyPass;
    class BC7CompressionReadbackPass;
    struct BC7CompressionCopyFrameData;
    struct BC7CompressionCopyBindings;
    struct BC7CompressionReadbackFrameData;
    struct BC7CompressionReadbackBindings;

    explicit TextureFactory(std::shared_ptr<org::runtime::IUploadService> uploadService);

	std::shared_ptr<org::RenderPass> m_mipmappingPass;
	std::shared_ptr<org::RenderPass> m_mipHandoffPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionCopyPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionReadbackPass;
    std::atomic<std::shared_ptr<org::runtime::IUploadProducer>> m_uploadService;
    std::atomic<std::shared_ptr<org::runtime::IUploadProducer>> m_jobUploadService;  // SetJobUploadService
    // Textures with a GPU mip job pending, and who waits for each (WhenMipsReady).
    struct MipWaiters {
        std::mutex mutex;
        std::unordered_map<const org::PixelBuffer*, std::vector<std::function<void()>>> pending;
    };
    std::shared_ptr<MipWaiters> m_mipWaiters = std::make_shared<MipWaiters>();
    std::shared_ptr<std::atomic_uint64_t> m_bc7InFlightBytes = std::make_shared<std::atomic_uint64_t>(0u);
};
