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

namespace {
// Whether the jobs Declare would take from pending now differ, in order, from the declared ones: stages advance at
// submission, so a pass polls them instead of being told.
template<class Job, class DeclaredJobs, class Takes>
bool DeclarableJobsDiffer(const std::vector<std::shared_ptr<Job>>& pending, const DeclaredJobs& declared, Takes takes)
{
    size_t index = 0;
    for (const auto& job : pending) {
        if (!job || !takes(*job)) {
            continue;
        }
        if (index >= declared.size() || declared[index].job != job) {
            return true;
        }
        ++index;
    }
    return index != declared.size();
}
}

bool TextureFactory::BC7CompressionPass::DeclaredResourcesChanged() const
{
    if (m_declaredResourcesChanged.load(std::memory_order_acquire)) return true;
    std::scoped_lock lock(m_pendingMutex);
    return DeclarableJobsDiffer(m_pending, *m_declaredJobs, [](const BC7CompressionJob& job) {
        const auto stage = job.stage.load(std::memory_order_acquire);
        return job.workingTexture && job.blockBuffer &&
            (stage == BC7CompressionJob::Stage::WaitingForSourceUpload || stage == BC7CompressionJob::Stage::ReadyForCompression);
    });
}

bool TextureFactory::BC7CompressionCopyPass::DeclaredResourcesChanged() const
{
    if (m_declaredResourcesChanged.load(std::memory_order_acquire)) return true;
    std::scoped_lock lock(m_pendingMutex);
    return DeclarableJobsDiffer(m_pending, *m_declaredJobs, [](const BC7CompressionJob& job) {
        return job.blockBuffer && job.compressedTexture &&
            job.stage.load(std::memory_order_acquire) == BC7CompressionJob::Stage::CompressionRecorded;
    });
}

bool TextureFactory::BC7CompressionReadbackPass::DeclaredResourcesChanged() const
{
    if (m_declaredResourcesChanged.load(std::memory_order_acquire)) return true;
    std::scoped_lock lock(m_pendingMutex);
    return DeclarableJobsDiffer(m_pending, *m_declaredJobs, [](const BC7CompressionJob& job) {
        return job.compressedTexture && job.stage.load(std::memory_order_acquire) == BC7CompressionJob::Stage::CopyRecorded;
    });
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

TextureBC7CompressionBindings TextureFactory::BC7CompressionPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    auto declaredJobs = std::make_shared<std::vector<DeclaredJob>>();
    m_declaredJobs = declaredJobs;
    TextureBC7CompressionBindings bindings;
    bindings.declaredJobs = declaredJobs;
    if (m_pending.empty()) {
        return bindings;
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

        declaredJobs->push_back({job});
        auto& declared = bindings.jobs.emplace_back();
        declared.subresources.reserve(job->subresources.size());
        for (const auto& subresource : job->subresources) {
            declared.subresources.push_back({builder.ShaderResource(
                Subresources(job->workingTexture, org::Mip{subresource.mip, 1}, org::Slice{subresource.slice, 1}),
                org::SrvView{.mip = subresource.mip, .slice = subresource.slice}).View(),
                static_cast<uint32_t>(subresource.footprint.offset), subresource.footprint.rowPitch,
                subresource.footprint.width, subresource.footprint.height});
        }
        declared.blocks = builder.UnorderedAccess(job->blockBuffer).View();
    }
    if (!bindings.jobs.empty()) bindings.pipeline = GetOrCreatePipeline();
    return bindings;
}

TextureBC7CompressionFrame TextureFactory::BC7CompressionPass::Prepare(
    const TextureBC7CompressionBindings& bindings, const org::PassPrepareContext& preparation)
{
    TextureBC7CompressionFrame frame{};
    // The jobs this build declared: a later structural build declares again while this one's executions still prepare.
    const auto declaredJobs = std::static_pointer_cast<const std::vector<DeclaredJob>>(bindings.declaredJobs);
    if (!declaredJobs) return frame;
    std::scoped_lock lock(m_pendingMutex);
    if (m_pending.empty()) return frame;

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
        const auto declared = std::find_if(declaredJobs->begin(), declaredJobs->end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == declaredJobs->end()) {
            waiting.push_back(job);
            continue;
        }
        frame.jobs.push_back(static_cast<uint32_t>(declared - declaredJobs->begin()));
        preparation.Retain(job);
        preparation.Reserve(std::make_shared<Transition>(Transition{job, preparation.frameIndex}),
            +[](Transition& transition, org::SubmissionContext) {
                transition.job->stageFrameIndex.store(transition.frameIndex, std::memory_order_release);
                transition.job->stage.store(BC7CompressionJob::Stage::CompressionRecorded,
                    std::memory_order_release);
            });
    }
    m_pending = std::move(waiting);
    return frame;
}

void TextureFactory::BC7CompressionPass::Record(const TextureBC7CompressionBindings& bindings,
    const TextureBC7CompressionFrame& frame, org::PassRecordContext& context)
{
    for (const uint32_t jobIndex : frame.jobs) {
        const auto& job = bindings.jobs.at(jobIndex);
        for (const auto& subresource : job.subresources) {
            br::render::MiscConstants constants{};
            constants[UintRootConstant0] = context.Index(subresource.source);
            constants[UintRootConstant1] = context.Index(job.blocks);
            constants[UintRootConstant2] = subresource.footprintOffset;
            constants[UintRootConstant3] = subresource.rowPitch;
            constants[UintRootConstant4] = subresource.width;
            constants[UintRootConstant5] = subresource.height;
            const uint32_t blocksX = (subresource.width + 3u) / 4u;
            const uint32_t blocksY = (subresource.height + 3u) / 4u;
            br::render::DispatchCompute(context, bindings.pipeline, constants, (blocksX + 7u) / 8u, (blocksY + 7u) / 8u);
        }
    }
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

TextureFactory::BC7CompressionCopyBindings TextureFactory::BC7CompressionCopyPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    auto declaredJobs = std::make_shared<std::vector<DeclaredJob>>();
    m_declaredJobs = declaredJobs;
    BC7CompressionCopyBindings bindings{declaredJobs};
    if (m_pending.empty()) {
        return bindings;
    }

    for (const auto& job : m_pending) {
        if (!job || !job->blockBuffer || !job->compressedTexture) {
            continue;
        }
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CompressionRecorded) {
            continue;
        }

        declaredJobs->push_back({job, builder.CopySource(job->blockBuffer),
            builder.CopyDestination(job->compressedTexture)});
    }
    return bindings;
}

TextureFactory::BC7CompressionCopyFrameData TextureFactory::BC7CompressionCopyPass::Prepare(
    const BC7CompressionCopyBindings& bindings, const org::PassPrepareContext& preparation)
{
    BC7CompressionCopyFrameData frame;
    // The jobs this build declared (see BC7CompressionPass::Prepare).
    const auto declaredJobs = std::static_pointer_cast<const std::vector<DeclaredJob>>(bindings.declaredJobs);
    if (!declaredJobs) return frame;
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
        const auto declared = std::find_if(declaredJobs->begin(), declaredJobs->end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == declaredJobs->end()) {
            waiting.push_back(job);
            continue;
        }
        for (const auto& subresource : job->subresources) {
            frame.copies.push_back({declared->source, declared->destination, subresource.footprint,
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
    return frame;
}

void TextureFactory::BC7CompressionCopyPass::Record(const BC7CompressionCopyBindings&,
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

TextureFactory::BC7CompressionReadbackBindings TextureFactory::BC7CompressionReadbackPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_pendingMutex);
    m_declaredResourcesChanged.store(false, std::memory_order_release);
    auto declaredJobs = std::make_shared<std::vector<DeclaredJob>>();
    m_declaredJobs = declaredJobs;
    BC7CompressionReadbackBindings bindings{declaredJobs};
    if (m_pending.empty()) {
        return bindings;
    }

    builder.PreferQueue(org::QueueKind::Copy);
    for (const auto& job : m_pending) {
        if (!job || !job->compressedTexture) {
            continue;
        }
        if (job->stage.load(std::memory_order_acquire) != BC7CompressionJob::Stage::CopyRecorded) {
            continue;
        }

        declaredJobs->push_back({job, builder.CopySource(job->compressedTexture)});
    }
    return bindings;
}

TextureFactory::BC7CompressionReadbackFrameData
TextureFactory::BC7CompressionReadbackPass::Prepare(const BC7CompressionReadbackBindings& bindings,
    const org::PassPrepareContext& preparation)
{
    BC7CompressionReadbackFrameData frame;
    // The jobs this build declared (see BC7CompressionPass::Prepare). Without them the pending jobs still fail when the
    // readback service is missing.
    static const std::vector<DeclaredJob> kNone;
    const auto declaredBindings = std::static_pointer_cast<const std::vector<DeclaredJob>>(bindings.declaredJobs);
    const auto& declaredJobs = declaredBindings ? *declaredBindings : kNone;
    std::scoped_lock lock(m_pendingMutex);
    if (m_pending.empty()) return frame;
    if (!m_readbackService) {
        for (const auto& job : m_pending) if (job && job->handle) {
            TextureProcessingManager::GetInstance().FailProcessing(
                job->handle, "TextureFactory: BC7 readback service is unavailable");
            job->ReleaseBudget();
        }
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
        const auto declared = std::find_if(declaredJobs.begin(), declaredJobs.end(),
            [&](const DeclaredJob& entry) { return entry.job == job; });
        if (declared == declaredJobs.end()) {
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
        for (size_t index = 0; index < job->subresources.size(); ++index) {
            const auto& subresource = job->subresources[index];
            frame.copies.push_back({declared->source, readback,
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
                job->ReleaseBudget();
            } catch (const std::exception& ex) {
                TextureProcessingManager::GetInstance().FailProcessing(job->handle, ex.what());
                job->stage.store(BC7CompressionJob::Stage::Completed, std::memory_order_release);
                job->ReleaseBudget();
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
    return frame;
}

void TextureFactory::BC7CompressionReadbackPass::Record(const BC7CompressionReadbackBindings&,
    const BC7CompressionReadbackFrameData& frame, org::PassRecordContext& recording)
{
    for (const auto& copy : frame.copies) {
        rhi::BufferTextureCopyFootprint region{};
        region.texture = recording.Resolve(copy.source).GetHandle();
        region.buffer = copy.destination->GetAPIResource().GetHandle();
        region.mip = copy.mip;
        region.arraySlice = copy.slice;
        region.footprint = copy.footprint;
        recording.Commands().CopyTextureToBuffer(region);
    }
}

