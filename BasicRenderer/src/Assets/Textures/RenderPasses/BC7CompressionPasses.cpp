#include "TextureProcessingPasses.h"

#include <algorithm>
#include <stdexcept>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Sampler.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ReadbackRequest.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IUploadService.h"
#include "ThirdParty/stb/stb_image.h"
#include "rhi_helpers.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Materials/TextureStreaming/MaterialTextureTransferService.h"
#include "Utilities/Utilities.h"

#define A_CPU
#include "../shaders/FidelityFX/ffx_a.h"
#include "../shaders/FidelityFX/ffx_spd.h"


namespace {
    std::shared_ptr<TextureSourceData> BuildCompressedSourceDataFromReadback(
        const org::TextureDescription& desc,
        bool hasFullMipChain,
        const org::ReadbackCaptureResult& readback)
    {
        if (readback.layouts.size() != desc.imageDimensions.size()) {
            throw std::runtime_error("BuildCompressedSourceDataFromReadback: readback layout count does not match texture subresources");
        }

        auto result = std::make_shared<TextureSourceData>();
        result->desc = desc;
        result->hasFullMipChain = hasFullMipChain;
        result->isBlockCompressed = true;
        result->subresources.reserve(desc.imageDimensions.size());

        for (size_t index = 0; index < desc.imageDimensions.size(); ++index) {
            const auto& dims = desc.imageDimensions[index];
            const auto& footprint = readback.layouts[index];
            const uint32_t rows = rhi::helpers::IsBlockCompressed(desc.format)
                ? (dims.height + 3u) / 4u
                : dims.height;

            if (dims.rowPitch > footprint.rowPitch) {
                throw std::runtime_error("BuildCompressedSourceDataFromReadback: readback row pitch is smaller than the subresource row size");
            }

            const uint64_t srcEnd = rows == 0
                ? footprint.offset
                : footprint.offset + static_cast<uint64_t>(footprint.rowPitch) * (rows - 1u) + dims.rowPitch;
            if (srcEnd > readback.data.size()) {
                throw std::runtime_error("BuildCompressedSourceDataFromReadback: readback buffer is smaller than expected");
            }

            auto bytes = std::make_shared<std::vector<uint8_t>>();
            bytes->resize(static_cast<size_t>(dims.slicePitch));

            const auto* srcBase = reinterpret_cast<const uint8_t*>(readback.data.data()) + footprint.offset;
            for (uint32_t row = 0; row < rows; ++row) {
                std::memcpy(
                    bytes->data() + static_cast<size_t>(row) * dims.rowPitch,
                    srcBase + static_cast<size_t>(row) * footprint.rowPitch,
                    static_cast<size_t>(dims.rowPitch));
            }

            result->subresources.push_back(std::move(bytes));
        }

        return result;
    }
}

void TextureFactory::BC7CompressionPass::EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job)
{
    if (!job) {
        return;
    }

    std::scoped_lock lock(m_pendingMutex);
    m_pending.push_back(job);
    m_declaredResourcesChanged = true;
}

void TextureFactory::BC7CompressionPass::Update(const org::UpdateExecutionContext& context)
{
    (void)context;
}

org::PipelineState TextureFactory::BC7CompressionPass::CreatePipeline() const
{
    auto& psoManager = PSOManager::GetInstance();
    return psoManager.MakeComputePipeline(
        psoManager.GetComputeRootSignature().GetHandle(),
        L"shaders/Utilities/bc7_compress_mode6.hlsl",
        L"BC7CompressMode6CS",
        {},
        "BC7Compression[Mode6]");
}

org::PipelineState& TextureFactory::BC7CompressionPass::GetOrCreatePipeline()
{
    if (!m_hasPsoMode6) {
        m_psoMode6 = CreatePipeline();
        m_hasPsoMode6 = true;
    }

    return m_psoMode6;
}

void TextureFactory::BC7CompressionPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    m_declaredJobs.clear();
    if (m_pending.empty()) {
        return;
    }

    for (const auto& job : m_pending) {
        if (!job || !job->workingTexture || !job->blockBuffer) {
            continue;
        }
        const auto stage = job->stage.load(std::memory_order_acquire);
        if (stage != BC7CompressionJob::Stage::WaitingForSourceUpload &&
            stage != BC7CompressionJob::Stage::ReadyForCompression) {
            continue;
        }

        auto& declared = m_declaredJobs.emplace_back();
        declared.job = job;
        declared.sources.reserve(job->subresources.size());
        for (const auto& subresource : job->subresources) {
            declared.sources.push_back(builder.ShaderResource(
                Subresources(job->workingTexture, org::Mip{subresource.mip, 1}, org::Slice{subresource.slice, 1}),
                org::SrvView{.mip = subresource.mip, .slice = subresource.slice}).View());
        }
        declared.blocks = builder.UnorderedAccess(job->blockBuffer).View();
    }
}

br::render::PreparedComputePipelineSequence TextureFactory::BC7CompressionPass::Prepare(
    const org::PassPrepareContext& preparation)
{
    br::render::PreparedComputePipelineSequence data{};
    std::scoped_lock lock(m_pendingMutex);
    if (m_pending.empty()) return data;
    const auto* context = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    if (!context) throw std::logic_error("BC7 compression requires the owned renderer snapshot");
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();

    struct Transition { std::shared_ptr<BC7CompressionJob> job; uint32_t frameIndex = 0; };
    std::vector<std::shared_ptr<BC7CompressionJob>> waiting;
    waiting.reserve(m_pending.size());
    for (const auto& job : m_pending) {
        if (!job || !job->workingTexture || !job->blockBuffer) continue;
        const auto stage = job->stage.load(std::memory_order_acquire);
        if (stage == BC7CompressionJob::Stage::WaitingForSourceUpload) {
            const uint32_t remaining =
                job->sourceUploadWaitExecutions.fetch_sub(1u, std::memory_order_acq_rel);
            if (remaining <= 1u) {
                job->stage.store(BC7CompressionJob::Stage::ReadyForCompression,
                    std::memory_order_release);
                job->stageFrameIndex.store(preparation.frameIndex, std::memory_order_release);
            }
            waiting.push_back(job);
            continue;
        }
        if (stage != BC7CompressionJob::Stage::ReadyForCompression ||
            job->stageFrameIndex.load(std::memory_order_acquire) == preparation.frameIndex) {
            waiting.push_back(job);
            continue;
        }
        const auto declared = std::find_if(m_declaredJobs.begin(), m_declaredJobs.end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == m_declaredJobs.end()) {
            waiting.push_back(job);
            continue;
        }
        const auto binding = preparation.CaptureProgramBinding(GetOrCreatePipeline());
        for (size_t index = 0; index < job->subresources.size(); ++index) {
            const auto& subresource = job->subresources[index];
            br::render::PreparedComputePipelineSequence::Step step{};
            step.program = binding.program;
            step.descriptorIndices = binding.descriptorIndices;
            step.constants[UintRootConstant0] = preparation.Resolve(declared->sources.at(index)).index;
            step.constants[UintRootConstant1] = preparation.Resolve(declared->blocks).index;
            step.constants[UintRootConstant2] = static_cast<uint32_t>(subresource.footprint.offset);
            step.constants[UintRootConstant3] = subresource.footprint.rowPitch;
            step.constants[UintRootConstant4] = subresource.footprint.width;
            step.constants[UintRootConstant5] = subresource.footprint.height;
            const uint32_t blocksX = (subresource.footprint.width + 3u) / 4u;
            const uint32_t blocksY = (subresource.footprint.height + 3u) / 4u;
            step.groupsX = (blocksX + 7u) / 8u;
            step.groupsY = (blocksY + 7u) / 8u;
            data.steps.push_back(std::move(step));
        }
        preparation.Retain(job);
        preparation.Reserve(std::make_shared<Transition>(Transition{job, preparation.frameIndex}),
            +[](Transition& transition, org::SubmissionContext) {
                transition.job->stageFrameIndex.store(transition.frameIndex, std::memory_order_release);
                transition.job->stage.store(BC7CompressionJob::Stage::CompressionRecorded,
                    std::memory_order_release);
            });
    }
    m_pending = std::move(waiting);
    m_declaredResourcesChanged = true;
    return data;
}

void TextureFactory::BC7CompressionCopyPass::EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job)
{
    if (!job) {
        return;
    }

    std::scoped_lock lock(m_pendingMutex);
    m_pending.push_back(job);
    m_declaredResourcesChanged = true;
}

void TextureFactory::BC7CompressionCopyPass::Update(const org::UpdateExecutionContext& context)
{
    (void)context;
}

void TextureFactory::BC7CompressionCopyPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    m_declaredJobs.clear();
    if (m_pending.empty()) {
        return;
    }

    for (const auto& job : m_pending) {
        if (!job || !job->blockBuffer || !job->compressedTexture) {
            continue;
        }
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CompressionRecorded) {
            continue;
        }

        m_declaredJobs.push_back({job, builder.CopySource(job->blockBuffer),
            builder.CopyDestination(job->compressedTexture)});
    }
}

TextureFactory::BC7CompressionCopyFrameData TextureFactory::BC7CompressionCopyPass::Prepare(
    const org::PassPrepareContext& preparation)
{
    BC7CompressionCopyFrameData frame;
    std::scoped_lock lock(m_pendingMutex);
    struct Transition { std::shared_ptr<BC7CompressionJob> job; uint32_t frameIndex = 0; };
    std::vector<std::shared_ptr<BC7CompressionJob>> waiting;
    waiting.reserve(m_pending.size());
    for (const auto& job : m_pending) {
        if (!job || !job->blockBuffer || !job->compressedTexture) continue;
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CompressionRecorded ||
            job->stageFrameIndex.load(std::memory_order_acquire) == preparation.frameIndex) {
            waiting.push_back(job);
            continue;
        }
        const auto declared = std::find_if(m_declaredJobs.begin(), m_declaredJobs.end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == m_declaredJobs.end()) {
            waiting.push_back(job);
            continue;
        }
        const auto source = preparation.CaptureResource(declared->source);
        const auto destination = preparation.CaptureResource(declared->destination);
        for (const auto& subresource : job->subresources) {
            frame.copies.push_back({source, destination, subresource.footprint,
                subresource.mip, subresource.slice});
        }
        preparation.Retain(job);
        preparation.Reserve(std::make_shared<Transition>(Transition{job, preparation.frameIndex}),
            +[](Transition& transition, org::SubmissionContext) {
                transition.job->stageFrameIndex.store(transition.frameIndex, std::memory_order_release);
                transition.job->stage.store(BC7CompressionJob::Stage::CopyRecorded,
                    std::memory_order_release);
            });
    }
    m_pending = std::move(waiting);
    m_declaredResourcesChanged = true;
    return frame;
}

void TextureFactory::BC7CompressionCopyPass::Record(
    const BC7CompressionCopyFrameData& frame, org::PassRecordContext& recording)
{
    for (const auto& copy : frame.copies) {
        rhi::BufferTextureCopyFootprint region{};
        region.buffer = recording.Resolve(copy.source).GetHandle();
        region.texture = recording.Resolve(copy.destination).GetHandle();
        region.mip = copy.mip;
        region.arraySlice = copy.slice;
        region.footprint = copy.footprint;
        recording.Commands().CopyBufferToTexture(region);
    }
}

void TextureFactory::BC7CompressionReadbackPass::SetReadbackService(
    std::shared_ptr<org::runtime::IReadbackService> readbackService)
{
    m_readbackService = std::move(readbackService);
}

void TextureFactory::BC7CompressionReadbackPass::EnqueueJob(const std::shared_ptr<BC7CompressionJob>& job)
{
    if (!job) {
        return;
    }

    std::scoped_lock lock(m_pendingMutex);
    m_pending.push_back(job);
    m_declaredResourcesChanged = true;
}

void TextureFactory::BC7CompressionReadbackPass::Update(const org::UpdateExecutionContext& context)
{
    (void)context;
}

void TextureFactory::BC7CompressionReadbackPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    m_declaredJobs.clear();
    if (m_pending.empty()) {
        return;
    }

    builder.PreferQueue(org::QueueKind::Copy);
    for (const auto& job : m_pending) {
        if (!job || !job->compressedTexture) {
            continue;
        }
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CopyRecorded) {
            continue;
        }

        m_declaredJobs.push_back({job, builder.CopySource(job->compressedTexture)});
    }
}

TextureFactory::BC7CompressionReadbackFrameData
TextureFactory::BC7CompressionReadbackPass::Prepare(const org::PassPrepareContext& preparation)
{
    BC7CompressionReadbackFrameData frame;
    std::scoped_lock lock(m_pendingMutex);
    if (m_pending.empty()) return frame;
    if (!m_readbackService) {
        for (const auto& job : m_pending) if (job && job->handle)
            TextureProcessingManager::GetInstance().FailProcessing(
                job->handle, "TextureFactory: BC7 readback service is unavailable");
        m_pending.clear();
        m_declaredResourcesChanged = true;
        return frame;
    }

    class Reservation final : public org::PreparedLifecycleEffect {
    public:
        Reservation(std::shared_ptr<org::runtime::IReadbackService> service,
            std::shared_ptr<rhi::TimelinePtr> timeline,
            std::vector<org::ReadbackCaptureRequest> requests,
            std::vector<std::shared_ptr<BC7CompressionJob>> jobs,
            uint32_t frameIndex)
            : m_service(service), m_timeline(std::move(timeline)),
              m_requests(std::move(requests)), m_jobs(std::move(jobs)),
              m_frameIndex(frameIndex), m_signal{m_timeline->Get(), 1u} {}
        std::span<const org::ExternalTimelinePoint> SignalsAfterCompletion() const override {
            return {&m_signal, 1u};
        }
        void Submitted(org::SubmissionContext) const override {
            if (m_resolved.exchange(true)) return;
            for (auto& request : m_requests) {
                const auto token = m_service->EnqueueCapture(std::move(request));
                m_service->FinalizeCapture(token, org::QueueKind::Copy, m_timeline, 1u);
            }
            for (const auto& job : m_jobs) {
                job->stageFrameIndex.store(m_frameIndex, std::memory_order_release);
                job->stage.store(BC7CompressionJob::Stage::ReadbackRecorded,
                    std::memory_order_release);
                TextureProcessingManager::GetInstance().MarkGpuJobReadbackPending(job->handle);
            }
        }
        void Abandoned(org::AbandonReason) const override {
            if (m_resolved.exchange(true)) return;
            for (const auto& job : m_jobs) {
                auto expected = BC7CompressionJob::Stage::ReadbackRecorded;
                job->stage.compare_exchange_strong(expected,
                    BC7CompressionJob::Stage::CopyRecorded, std::memory_order_acq_rel);
            }
        }
    private:
        std::shared_ptr<org::runtime::IReadbackService> m_service;
        std::shared_ptr<rhi::TimelinePtr> m_timeline;
        mutable std::vector<org::ReadbackCaptureRequest> m_requests;
        std::vector<std::shared_ptr<BC7CompressionJob>> m_jobs;
        uint32_t m_frameIndex;
        org::ExternalTimelinePoint m_signal{};
        mutable std::atomic<bool> m_resolved{false};
    };

    auto timeline = std::make_shared<rhi::TimelinePtr>();
    DeviceManager::GetInstance().GetDevice().CreateTimeline(*timeline);
    std::vector<org::ReadbackCaptureRequest> requests;
    std::vector<std::shared_ptr<BC7CompressionJob>> admitted;
    std::vector<std::shared_ptr<BC7CompressionJob>> waiting;
    for (const auto& job : m_pending) {
        if (!job || !job->compressedTexture || !job->handle) continue;
        if (job->stage.load(std::memory_order_acquire) == BC7CompressionJob::Stage::Completed)
            continue;
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CopyRecorded ||
            job->stageFrameIndex.load(std::memory_order_acquire) == preparation.frameIndex) {
            waiting.push_back(job);
            continue;
        }
        const auto declared = std::find_if(m_declaredJobs.begin(), m_declaredJobs.end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == m_declaredJobs.end()) {
            waiting.push_back(job);
            continue;
        }
        std::vector<rhi::CopyableFootprint> footprints(job->subresources.size());
        rhi::FootprintRangeDesc range{};
        range.texture = job->compressedTexture->GetAPIResource().GetHandle();
        range.mipCount = static_cast<uint32_t>(job->subresources.size());
        range.arraySize = 1; range.planeCount = 1;
        const auto info = DeviceManager::GetInstance().GetDevice().GetCopyableFootprints(
            range, footprints.data(), static_cast<uint32_t>(footprints.size()));
        auto readback = org::Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
        if (!job->debugName.empty()) readback->SetName(job->debugName + "[BC7Readback]");
        preparation.Retain(readback);
        const auto source = preparation.CaptureResource(declared->source);
        for (size_t index = 0; index < job->subresources.size(); ++index) {
            const auto& subresource = job->subresources[index];
            frame.copies.push_back({source, readback->GetAPIResource().GetHandle(),
                footprints[index], subresource.mip, subresource.slice});
        }
        org::ReadbackCaptureRequest request{};
        request.desc.kind = org::ReadbackResourceKind::Texture;
        request.desc.resourceId = job->compressedTexture->GetGlobalResourceID();
        request.readbackBuffer = readback;
        request.layouts = footprints;
        request.totalSize = info.totalBytes;
        request.format = job->compressedTexture->GetFormat();
        request.width = job->compressedTexture->GetWidth();
        request.height = job->compressedTexture->GetHeight();
        request.depth = 1;
        request.callback = [job](org::ReadbackCaptureResult&& result) {
            try {
                auto data = BuildCompressedSourceDataFromReadback(
                    job->compressedTexture->GetDescription(), job->outputHasFullMipChain, result);
                TextureProcessingManager::GetInstance().CompleteGpuProcessing(
                    job->handle, std::move(data), job->compressedTexture, true);
                job->stage.store(BC7CompressionJob::Stage::Completed, std::memory_order_release);
            } catch (const std::exception& ex) {
                TextureProcessingManager::GetInstance().FailProcessing(job->handle, ex.what());
                job->stage.store(BC7CompressionJob::Stage::Completed, std::memory_order_release);
            }
        };
        requests.push_back(std::move(request));
        admitted.push_back(job);
        job->stage.store(BC7CompressionJob::Stage::ReadbackRecorded, std::memory_order_release);
        waiting.push_back(job);
    }
    m_pending = std::move(waiting);
    if (!admitted.empty()) preparation.Reserve(std::make_shared<Reservation>(
        m_readbackService, std::move(timeline), std::move(requests),
        std::move(admitted), preparation.frameIndex));
    m_declaredResourcesChanged = true;
    return frame;
}

void TextureFactory::BC7CompressionReadbackPass::Record(
    const BC7CompressionReadbackFrameData& frame, org::PassRecordContext& recording)
{
    for (const auto& copy : frame.copies) {
        rhi::BufferTextureCopyFootprint region{};
        region.texture = recording.Resolve(copy.source).GetHandle();
        region.buffer = copy.destination;
        region.mip = copy.mip;
        region.arraySlice = copy.slice;
        region.footprint = copy.footprint;
        recording.Commands().CopyTextureToBuffer(region);
    }
}
