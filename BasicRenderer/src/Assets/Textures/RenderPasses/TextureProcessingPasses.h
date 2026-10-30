#pragma once

#include "Assets/Textures/TextureFactory.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include <vector>

// The mip chains taken at declaration, in queue order, with everything their dispatches need.
struct TextureMipmappingBindings {
        struct Job {
            bool hasTexture = false;
            bool preserveAlphaCoverage = false;
            bool isSrgb = false;
            org::PipelineState pipeline;
            org::DeclaredViewToken constants, source, counter, alphaStats, alphaScales;
            std::vector<org::DeclaredViewToken> alphaSources, outputs;
            uint32_t constantsIndex = 0;
            uint32_t dispatchThreadGroupCountXY[2]{};
            uint32_t sliceCount = 1;
            uint32_t mipsToGenerate = 0;
            uint32_t width = 0, height = 0;
        };
        org::PipelineState alphaReset, alphaDownsample, alphaResolveScale, alphaApplyScale;
        std::vector<Job> jobs;
    };

class TextureFactory::MipmappingPass : public org::TypedRenderGraphPass<MipmappingPass, org::EmptyPassFrameData, TextureMipmappingBindings>, public org::IDynamicDeclaredResources {
    public:
        // Called by TextureFactory when you create a texture with only mip0 uploaded.
        void EnqueueJob(const std::shared_ptr<org::PixelBuffer>& tex, bool isSrgb, bool preserveAlphaCoverage = false);

        TextureMipmappingBindings Declare(org::PassBuilder& builder);

        org::EmptyPassFrameData Prepare(const TextureMipmappingBindings& bindings, const org::PassPrepareContext& preparation);
        static void Record(const TextureMipmappingBindings& bindings, org::PassRecordContext& context);

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

// The jobs taken at declaration, each with its per-subresource dispatch inputs.
struct TextureBC7CompressionBindings {
        struct Subresource {
            org::DeclaredViewToken source;
            uint32_t footprintOffset = 0, rowPitch = 0, width = 0, height = 0;
        };
        struct Job {
            std::vector<Subresource> subresources;
            org::DeclaredViewToken blocks;
        };
        org::PipelineState pipeline;
        std::vector<Job> jobs;
    };
// Indices into TextureBC7CompressionBindings::jobs of the jobs compressed this frame.
struct TextureBC7CompressionFrame {
        std::vector<uint32_t> jobs;
    };

class TextureFactory::BC7CompressionPass
        : public org::TypedRenderGraphPass<BC7CompressionPass,
              TextureBC7CompressionFrame, TextureBC7CompressionBindings>,
          public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        TextureBC7CompressionBindings Declare(org::PassBuilder& builder);
        TextureBC7CompressionFrame Prepare(const TextureBC7CompressionBindings& bindings,
            const org::PassPrepareContext& preparation);
        static void Record(const TextureBC7CompressionBindings& bindings, const TextureBC7CompressionFrame& frame,
            org::PassRecordContext& context);

        // A job enqueued or failed, or the jobs Declare would take now (by stage) differ from those it took.
        bool DeclaredResourcesChanged() const override;

    private:
        org::PipelineState& GetOrCreatePipeline();
        org::PipelineState CreatePipeline() const;

        // The jobs Declare took, in the order of TextureBC7CompressionBindings::jobs.
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
        };
        std::vector<DeclaredJob> m_declaredJobs;
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        org::PipelineState m_psoMode6;
        bool m_hasPsoMode6 = false;
        std::atomic_bool m_declaredResourcesChanged = true;
    };

struct TextureFactory::BC7CompressionCopyFrameData {
        struct Copy {
            org::ResourceBindingToken source{}, destination{};
            rhi::CopyableFootprint footprint{};
            uint32_t mip = 0, slice = 0;
        };
        std::vector<Copy> copies;
    };

class TextureFactory::BC7CompressionCopyPass
        : public org::TypedRenderGraphPass<BC7CompressionCopyPass, BC7CompressionCopyFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        void Declare(org::PassBuilder& builder);
        BC7CompressionCopyFrameData Prepare(const org::PassPrepareContext& preparation);
        static void Record(const BC7CompressionCopyFrameData& data, org::PassRecordContext& recording);

        // A job enqueued or failed, or the jobs Declare would take now (by stage) differ from those it took.
        bool DeclaredResourcesChanged() const override;

    private:
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
            org::ResourceBindingToken source, destination;
        };
        std::vector<DeclaredJob> m_declaredJobs;
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        std::atomic_bool m_declaredResourcesChanged = true;
    };

struct TextureFactory::BC7CompressionReadbackFrameData {
        struct Copy {
            org::ResourceBindingToken source{};
            std::shared_ptr<org::Buffer> destination;
            rhi::CopyableFootprint footprint{};
            uint32_t mip = 0, slice = 0;
        };
        std::vector<Copy> copies;
    };

class TextureFactory::BC7CompressionReadbackPass
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

        // A job enqueued or failed, or the jobs Declare would take now (by stage) differ from those it took.
        bool DeclaredResourcesChanged() const override;

    private:
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
            org::ResourceBindingToken source;
        };
        std::vector<DeclaredJob> m_declaredJobs;
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        std::shared_ptr<org::runtime::IReadbackService> m_readbackService;
        std::atomic_bool m_declaredResourcesChanged = true;
    };
