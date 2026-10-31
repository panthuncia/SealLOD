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
        // The pass's queue entries these views were declared for (its private snapshot type).
        // Kept with the bindings: a later structural build declares again while this build's
        // executions still prepare.
        std::shared_ptr<const void> declaredJobs;
    };

// The textures whose mip chains were submitted since the last handoff.
struct TextureMipHandoffBindings {
    size_t textures = 0;
    std::shared_ptr<const void> declaredJobs;  // as TextureMipmappingBindings::declaredJobs
};

// Takes each texture whose mips the GPU generated (submitted in an earlier execution of the job graph) to shader-resource
// state on the graphics queue, where bindless sampling expects it: its mips leave the mip pass in unordered-access state, on the
// compute queue. Records nothing; its declarations are the handoff.
class TextureFactory::MipHandoffPass : public org::TypedRenderGraphPass<MipHandoffPass, org::EmptyPassFrameData, TextureMipHandoffBindings>, public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(std::shared_ptr<org::PixelBuffer> texture);
        // Called with each texture once the GPU has completed its handoff (on the thread retiring the job graph's executions).
        void SetCompleted(std::function<void(const org::PixelBuffer&)> completed) { m_completed = std::move(completed); }

        TextureMipHandoffBindings Declare(org::PassBuilder& builder);
        org::EmptyPassFrameData Prepare(const TextureMipHandoffBindings& bindings, const org::PassPrepareContext& preparation);
        static void Record(const TextureMipHandoffBindings&, org::PassRecordContext&) {}

        bool DeclaredResourcesChanged() const override {
            return m_declaredResourcesChanged || m_jobs.ReadCounters().pending != 0;
        }

    private:
        struct Job {
            std::shared_ptr<org::PixelBuffer> texture;
            std::shared_ptr<const std::function<void(const org::PixelBuffer&)>> completed;
            // The GPU completed the execution it was submitted with (FrameWorkQueue).
            void Complete() const { if (completed && *completed && texture) (*completed)(*texture); }
        };
        std::shared_ptr<const std::function<void(const org::PixelBuffer&)>> m_completedShared;
        std::function<void(const org::PixelBuffer&)> m_completed;
        org::runtime::FrameWorkQueue<Job> m_jobs;
        std::atomic_bool m_declaredResourcesChanged = true;
};

// Indices into TextureMipmappingBindings::jobs of the jobs this execution took (FrameWorkQueue::ReserveAvailable).
struct TextureMipmappingFrame {
    std::vector<uint32_t> jobs;
};

class TextureFactory::MipmappingPass : public org::TypedRenderGraphPass<MipmappingPass, TextureMipmappingFrame, TextureMipmappingBindings>, public org::IDynamicDeclaredResources {
    public:
        // Each texture whose mips are submitted is handed to it (MipHandoffPass).
        void SetHandoff(std::shared_ptr<MipHandoffPass> handoff) { m_handoff = std::move(handoff); }
        // The job graph's upload queue: each job's constants upload through it, at once (BufferBase::PinUploadService), so its
        // upload pass lands them ahead of the dispatch.
        void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploads) { m_uploads = std::move(uploads); }
        // Called by TextureFactory when you create a texture with only mip0 uploaded. False: no job (the texture cannot be mipped
        // on the GPU), so no handoff will follow.
        bool EnqueueJob(const std::shared_ptr<org::PixelBuffer>& tex, bool isSrgb, bool preserveAlphaCoverage = false);

        TextureMipmappingBindings Declare(org::PassBuilder& builder);

        TextureMipmappingFrame Prepare(const TextureMipmappingBindings& bindings, const org::PassPrepareContext& preparation);
        static void Record(const TextureMipmappingBindings& bindings, const TextureMipmappingFrame& frame,
            org::PassRecordContext& context);

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

            std::shared_ptr<MipHandoffPass> handoff;
            // Submitted (FrameWorkQueue): the texture goes on to its handoff.
            void Commit() const { if (handoff && texture) handoff->EnqueueJob(texture); }
        };

        std::shared_ptr<MipHandoffPass> m_handoff;
        std::shared_ptr<org::runtime::IUploadService> m_uploads;
        org::runtime::FrameWorkQueue<Job> m_jobs;

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
        // The pass's DeclaredJob list for `jobs` (its private type), as TextureMipmappingBindings::declaredJobs.
        std::shared_ptr<const void> declaredJobs;
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
        bool HasPendingJobs() const { std::scoped_lock lock(m_pendingMutex); return !m_pending.empty(); }

    private:
        org::PipelineState& GetOrCreatePipeline();
        org::PipelineState CreatePipeline() const;

        // The jobs Declare took, in the order of TextureBC7CompressionBindings::jobs.
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
        };
        // The latest declaration's (what DeclaredResourcesChanged compares against); each build's own is in its bindings.
        std::shared_ptr<const std::vector<DeclaredJob>> m_declaredJobs = std::make_shared<const std::vector<DeclaredJob>>();
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

// The pass's DeclaredJob list (its private type), as TextureMipmappingBindings::declaredJobs.
struct TextureFactory::BC7CompressionCopyBindings {
        std::shared_ptr<const void> declaredJobs;
    };

class TextureFactory::BC7CompressionCopyPass
        : public org::TypedRenderGraphPass<BC7CompressionCopyPass, BC7CompressionCopyFrameData, BC7CompressionCopyBindings>,
          public org::IDynamicDeclaredResources {
    public:
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        BC7CompressionCopyBindings Declare(org::PassBuilder& builder);
        BC7CompressionCopyFrameData Prepare(const BC7CompressionCopyBindings& bindings,
            const org::PassPrepareContext& preparation);
        static void Record(const BC7CompressionCopyBindings& bindings, const BC7CompressionCopyFrameData& data,
            org::PassRecordContext& recording);

        // A job enqueued or failed, or the jobs Declare would take now (by stage) differ from those it took.
        bool DeclaredResourcesChanged() const override;
        bool HasPendingJobs() const { std::scoped_lock lock(m_pendingMutex); return !m_pending.empty(); }

    private:
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
            org::ResourceBindingToken source, destination;
        };
        // The latest declaration's (what DeclaredResourcesChanged compares against); each build's own is in its bindings.
        std::shared_ptr<const std::vector<DeclaredJob>> m_declaredJobs = std::make_shared<const std::vector<DeclaredJob>>();
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

// The pass's DeclaredJob list (its private type), as TextureMipmappingBindings::declaredJobs.
struct TextureFactory::BC7CompressionReadbackBindings {
        std::shared_ptr<const void> declaredJobs;
    };

class TextureFactory::BC7CompressionReadbackPass
        : public org::TypedRenderGraphPass<BC7CompressionReadbackPass,
              BC7CompressionReadbackFrameData, BC7CompressionReadbackBindings>,
          public org::IDynamicDeclaredResources {
    public:
        void SetReadbackService(std::shared_ptr<org::runtime::IReadbackService> readbackService);
        bool HasReadbackService() const { return static_cast<bool>(m_readbackService); }
        void EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job);

        void Update(const org::UpdateExecutionContext& context) override;

        BC7CompressionReadbackBindings Declare(org::PassBuilder& builder);
        BC7CompressionReadbackFrameData Prepare(const BC7CompressionReadbackBindings& bindings,
            const org::PassPrepareContext& preparation);
        static void Record(const BC7CompressionReadbackBindings& bindings, const BC7CompressionReadbackFrameData& data,
            org::PassRecordContext& recording);

        // A job enqueued or failed, or the jobs Declare would take now (by stage) differ from those it took.
        bool DeclaredResourcesChanged() const override;
        bool HasPendingJobs() const { std::scoped_lock lock(m_pendingMutex); return !m_pending.empty(); }

    private:
        struct DeclaredJob {
            std::shared_ptr<BC7CompressionJob> job;
            org::ResourceBindingToken source;
        };
        // The latest declaration's (what DeclaredResourcesChanged compares against); each build's own is in its bindings.
        std::shared_ptr<const std::vector<DeclaredJob>> m_declaredJobs = std::make_shared<const std::vector<DeclaredJob>>();
        std::vector<std::shared_ptr<BC7CompressionJob>> m_pending;
        mutable std::mutex m_pendingMutex;
        std::shared_ptr<org::runtime::IReadbackService> m_readbackService;
        std::atomic_bool m_declaredResourcesChanged = true;
    };
