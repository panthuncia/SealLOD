#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string_view>

#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Managers/Singletons/PSOManager.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
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

    class MipmappingPass : public org::TypedRenderGraphPass<MipmappingPass, br::render::PreparedComputePipelineSequence>, public org::IDynamicDeclaredResources {
    public:
        // Called by TextureFactory when you create a texture with only mip0 uploaded.
        void EnqueueJob(const std::shared_ptr<org::PixelBuffer>& tex, bool isSrgb, bool preserveAlphaCoverage = false);

        void Declare(org::PassBuilder& builder);

        br::render::PreparedComputePipelineSequence Prepare(const org::PassPrepareContext& preparation);
        static void Record(const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
            br::render::RecordPreparedComputePipelineSequence(data, recording);
        }

        bool DeclaredResourcesChanged() const override {
            return m_declaredResourcesChanged || m_jobs.ReadCounters().pending != 0;
        }

    private:
        enum class MipmapValueType
        {
            Float1,
            Float2,
            Float4,
        };

        struct MipmapSpdConstants
        {
            uint32_t srcSize[2];
            uint32_t mips;
            uint32_t numWorkGroups;

            uint32_t workGroupOffset[2];
            float    invInputSize[2];

            uint32_t mipUavDescriptorIndices[12];
            uint32_t flags;
            uint32_t srcMip;
            uint32_t pad0;
            uint32_t pad1;
        };

        struct Job
        {
            std::shared_ptr<org::PixelBuffer> texture;
            std::shared_ptr<org::BufferView> constantsView;
            std::shared_ptr<org::LazyDynamicStructuredBuffer<MipmapSpdConstants>> constantsBuffer;
            std::shared_ptr<org::GloballyIndexedResource> counter;
            std::shared_ptr<org::Buffer> alphaStats;
            std::shared_ptr<org::Buffer> alphaScales;

            MipmapSpdConstants cpuConstants{};
            uint32_t constantsIndex = 0;

            uint32_t dispatchThreadGroupCountXY[2]{};
            uint32_t sliceCount = 1;
            uint32_t mipsToGenerate = 0;

            bool isArray = false;
            bool isSrgb = false;
            bool preserveAlphaCoverage = false;
            MipmapValueType valueType = MipmapValueType::Float4;
        };

        org::runtime::FrameWorkQueue<Job> m_jobs;
        org::runtime::FrameWorkQueue<Job>::Snapshot m_declaredJobs;

        org::PipelineState m_psoFloat1_2D;
        org::PipelineState m_psoFloat1_Array;
        org::PipelineState m_psoFloat2_2D;
        org::PipelineState m_psoFloat2_Array;
        org::PipelineState m_psoFloat4_2D;
        org::PipelineState m_psoFloat4_Array;
        org::PipelineState m_psoAlphaReset;
        org::PipelineState m_psoAlphaDownsample;
        org::PipelineState m_psoAlphaResolveScale;
        org::PipelineState m_psoAlphaApplyScale;

        bool m_hasPsoFloat1_2D = false;
        bool m_hasPsoFloat1_Array = false;
        bool m_hasPsoFloat2_2D = false;
        bool m_hasPsoFloat2_Array = false;
        bool m_hasPsoFloat4_2D = false;
        bool m_hasPsoFloat4_Array = false;
        bool m_hasPsoAlphaReset = false;
        bool m_hasPsoAlphaDownsample = false;
        bool m_hasPsoAlphaResolveScale = false;
        bool m_hasPsoAlphaApplyScale = false;

        static bool TryGetValueType(const org::PixelBuffer& tex, MipmapValueType& outValueType);
        org::PipelineState& GetOrCreatePipeline(MipmapValueType valueType, bool isArray);
        org::PipelineState CreatePipeline(MipmapValueType valueType, bool isArray) const;
        org::PipelineState& GetOrCreateAlphaPipeline(const wchar_t* entryPoint, org::PipelineState& pso, bool& hasPso, const char* debugName);

        std::atomic_bool m_declaredResourcesChanged = true;
    };

    class BC7CompressionPass
        : public org::TypedRenderGraphPass<BC7CompressionPass,
              br::render::PreparedComputePipelineSequence>,
          public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        void Declare(org::PassBuilder& builder);
        br::render::PreparedComputePipelineSequence Prepare(const org::PassPrepareContext& preparation);
        static void Record(const br::render::PreparedComputePipelineSequence& data,
            org::PassRecordContext& recording) {
            br::render::RecordPreparedComputePipelineSequence(data, recording);
        }

        bool DeclaredResourcesChanged() const override {
            return m_declaredResourcesChanged.load(std::memory_order_acquire);
        }

    private:
        org::PipelineState& GetOrCreatePipeline();
        org::PipelineState CreatePipeline() const;

        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        org::PipelineState m_psoMode6;
        bool m_hasPsoMode6 = false;
        std::atomic_bool m_declaredResourcesChanged = true;
    };

    struct BC7CompressionCopyFrameData {
        struct Copy {
            org::PreparedResourceReference source{}, destination{};
            rhi::CopyableFootprint footprint{};
            uint32_t mip = 0, slice = 0;
        };
        std::vector<Copy> copies;
    };

    class BC7CompressionCopyPass
        : public org::TypedRenderGraphPass<BC7CompressionCopyPass, BC7CompressionCopyFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        void Declare(org::PassBuilder& builder);
        BC7CompressionCopyFrameData Prepare(const org::PassPrepareContext& preparation);
        static void Record(const BC7CompressionCopyFrameData& data, org::PassRecordContext& recording);

        bool DeclaredResourcesChanged() const override {
            return m_declaredResourcesChanged.load(std::memory_order_acquire);
        }

    private:
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        std::atomic_bool m_declaredResourcesChanged = true;
    };

    struct BC7CompressionReadbackFrameData {
        struct Copy {
            org::PreparedResourceReference source{};
            rhi::ResourceHandle destination{};
            rhi::CopyableFootprint footprint{};
            uint32_t mip = 0, slice = 0;
        };
        std::vector<Copy> copies;
    };

    class BC7CompressionReadbackPass
        : public org::TypedRenderGraphPass<BC7CompressionReadbackPass,
              BC7CompressionReadbackFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        void SetReadbackService(std::shared_ptr<org::runtime::IReadbackService> readbackService);
        bool HasReadbackService() const { return static_cast<bool>(m_readbackService); }
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        void Declare(org::PassBuilder& builder);
        BC7CompressionReadbackFrameData Prepare(const org::PassPrepareContext& preparation);
        static void Record(const BC7CompressionReadbackFrameData& data,
            org::PassRecordContext& recording);

        bool DeclaredResourcesChanged() const override {
            return m_declaredResourcesChanged.load(std::memory_order_acquire);
        }

    private:
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        std::shared_ptr<org::runtime::IReadbackService> m_readbackService;
        std::atomic_bool m_declaredResourcesChanged = true;
    };

    explicit TextureFactory(std::shared_ptr<org::runtime::IUploadService> uploadService)
        : m_uploadService(std::move(uploadService)) {
		m_mipmappingPass = std::make_shared<MipmappingPass>();
		m_bc7CompressionPass = std::make_shared<BC7CompressionPass>();
		m_bc7CompressionCopyPass = std::make_shared<BC7CompressionCopyPass>();
		m_bc7CompressionReadbackPass = std::make_shared<BC7CompressionReadbackPass>();
    }

	std::shared_ptr<org::RenderPass> m_mipmappingPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionCopyPass;
	std::shared_ptr<org::RenderPass> m_bc7CompressionReadbackPass;
    std::shared_ptr<org::runtime::IUploadService> m_uploadService;
    std::shared_ptr<std::atomic_uint32_t> m_bc7InFlightJobs = std::make_shared<std::atomic_uint32_t>(0u);
};
