#include "Assets/Textures/Pipeline/TexturePipelineArtifacts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>

#include <spdlog/spdlog.h>

#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "Assets/Textures/TextureFactory.h"
#include "Runtime/IO/DirectStorageManager.h"
#include "Utilities/Utilities.h"

namespace br::render {
namespace {

using namespace std::chrono_literals;

// Resumes a TextureProcessed job suspended on its GPU BC7 job.
struct GpuJobWake {
    std::mutex mutex;
    std::uint64_t identity = 0;
    bool finished = false;
    std::function<void(std::uint64_t)> notify;

    void Finish() {
        std::uint64_t identityToNotify = 0;
        {
            std::lock_guard lock(mutex);
            finished = true;
            identityToNotify = std::exchange(identity, 0);
        }
        if (identityToNotify != 0) notify(identityToNotify);
    }
    // False when the job already finished: there is nothing to wait for.
    bool Arm(std::uint64_t identityValue) {
        std::lock_guard lock(mutex);
        if (finished) return false;
        identity = identityValue;
        return true;
    }
};

struct ProcessedCheckpoint {
    std::shared_ptr<TextureProcessingJobHandle> gpuJob;
    std::shared_ptr<TextureSourceData> prepared;
    std::shared_ptr<GpuJobWake> wake;
    bool submitted = false;
    std::uint32_t submitAttempts = 0;
};

struct ResidencyCheckpoint {
    std::shared_ptr<TextureDirectStorageReloadJobHandle> upload;
    std::uint32_t topMip = 0;
};

bool GpuCompressible(const TextureSourceData& data) {
    return !data.isBlockCompressed && !data.desc.isArray && !data.desc.isCubemap;
}

ArtifactBuildResult ProcessedReady(const std::string& cacheKey,
    std::shared_ptr<TextureSourceData> data, std::shared_ptr<org::PixelBuffer> gpuImage)
{
    if (!data || data->desc.imageDimensions.empty()) {
        return ArtifactBuildResult::Failure("texture processing produced no texture");
    }
    auto published = std::make_shared<PublishedTextureProcessed>();
    published->cacheKey = cacheKey;
    published->fullDesc = data->desc;
    published->result = std::make_shared<TextureProcessedResult>();
    published->result->data = std::move(data);
    published->result->gpuImage = std::move(gpuImage);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<PublishedTextureProcessed>(std::move(published)));
}

ArtifactBuildResult SubmitGpuJob(const TextureProcessedInput& input, const TextureFileMeta& meta,
    ProcessedCheckpoint checkpoint)
{
    bool gpuCanRun = false;
    {
        std::shared_lock lock(input.services->mutex);
        if (!input.services->factory) return ArtifactBuildResult::Cancelled();
        gpuCanRun = input.services->factory->CanRunBC7CompressionJobs() && GpuCompressible(*checkpoint.prepared);
        checkpoint.submitted = gpuCanRun && input.services->factory->SubmitBC7CompressionJob(
            checkpoint.gpuJob, input.request->debugName);
    }
    if (!checkpoint.submitted) {
        // The GPU cannot take this texture: compress it on the CPU. Otherwise the BC7 VRAM
        // budget is full; wait for room however long it takes (CPU BC7 is far slower, and
        // cannot be stopped at shutdown).
        if (!gpuCanRun) {
            return ProcessedReady(input.cacheKey,
                TextureProcessingManager::FinalizeOnCpu(checkpoint.prepared, meta), {});
        }
        ++checkpoint.submitAttempts;
        return ArtifactBuildResult::Retry(20ms,
            ArtifactPayload::Make<ProcessedCheckpoint>(std::make_shared<ProcessedCheckpoint>(std::move(checkpoint))));
    }
    const auto identity = AllocateArtifactSuspensionIdentity();
    const bool armed = checkpoint.wake->Arm(identity);
    auto payload = ArtifactPayload::Make<ProcessedCheckpoint>(std::make_shared<ProcessedCheckpoint>(std::move(checkpoint)));
    if (!armed) return ArtifactBuildResult::Retry(0ms, std::move(payload));
    return ArtifactBuildResult::Suspend(ArtifactSuspension::External(identity, "GPU BC7 job"), std::move(payload));
}

ArtifactBuildResult BuildTextureProcessed(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureProcessedInput>();
    if (!input || !input->request || !input->services) {
        return ArtifactBuildResult::Failure("texture processed input missing");
    }
    if (TextureCacheKeyHash(input->cacheKey) != context.key.primaryID) {
        return ArtifactBuildResult::Failure("texture processed identity mismatch");
    }
    if (context.stopRequested && context.stopRequested()) return ArtifactBuildResult::Cancelled();
    const auto meta = br::textures::TextureProcessingMeta(*input->request);

    try {
        if (const auto checkpoint = context.checkpoint.Get<ProcessedCheckpoint>()) {
            if (!checkpoint->submitted) return SubmitGpuJob(*input, meta, *checkpoint);
            const auto state = checkpoint->gpuJob->state.load(std::memory_order_acquire);
            if (state == TextureProcessingJobState::Ready) {
                std::shared_ptr<TextureSourceData> result;
                std::shared_ptr<org::PixelBuffer> image;
                {
                    std::scoped_lock lock(checkpoint->gpuJob->mutex);
                    result = checkpoint->gpuJob->result;
                    image = checkpoint->gpuJob->uploadedImage;
                }
                return ProcessedReady(input->cacheKey, std::move(result), std::move(image));
            }
            if (state == TextureProcessingJobState::Failed) {
                return ProcessedReady(input->cacheKey,
                    TextureProcessingManager::FinalizeOnCpu(checkpoint->prepared, meta), {});
            }
            const auto identity = AllocateArtifactSuspensionIdentity();
            if (!checkpoint->wake->Arm(identity)) return ArtifactBuildResult::Retry(0ms, context.checkpoint);
            return ArtifactBuildResult::Suspend(ArtifactSuspension::External(identity, "GPU BC7 job"), context.checkpoint);
        }

        if (input->request->preparation == br::textures::TexturePreparation::LoadOnly) {
            return ProcessedReady(input->cacheKey, br::textures::DecodeTextureSource(*input->request), {});
        }

        // A hit needs only the cache file's header.
        const auto cachePath = TextureProcessingManager::ConditionedCachePath(input->cacheKey);
        std::error_code error;
        if (std::filesystem::exists(cachePath, error) && !error) {
            if (auto desc = br::textures::ReadConditionedCacheDescription(cachePath)) {
                auto published = std::make_shared<PublishedTextureProcessed>();
                published->cacheKey = input->cacheKey;
                published->fullDesc = std::move(*desc);
                published->cachePath = cachePath;
                return ArtifactBuildResult::Ready(ArtifactPayload::Make<PublishedTextureProcessed>(std::move(published)));
            }
            spdlog::warn("Texture pipeline: unreadable cache file '{}' for '{}'; reprocessing",
                ws2s(cachePath), input->request->source.identity);
        }

        auto source = br::textures::DecodeTextureSource(*input->request);
        auto processed = TextureProcessingManager::GetInstance().Process(source, meta);
        if (!processed.gpuCompression) return ProcessedReady(input->cacheKey, std::move(processed.result), {});

        ProcessedCheckpoint checkpoint;
        checkpoint.prepared = processed.prepared;
        checkpoint.wake = std::make_shared<GpuJobWake>();
        checkpoint.wake->notify = input->services->notifySuspension;
        checkpoint.gpuJob = TextureProcessingManager::MakePipelineGpuJob(
            processed.prepared, meta, input->cacheKey, [wake = checkpoint.wake] { wake->Finish(); });
        return SubmitGpuJob(*input, meta, std::move(checkpoint));
    }
    catch (const std::exception& ex) {
        spdlog::warn("Texture pipeline: '{}' failed to load: {}", input->request->source.identity, ex.what());
        return ArtifactBuildResult::Failure(ex.what());
    }
}

ArtifactBuildResult BuildTextureCacheWrite(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureCacheWriteInput>();
    if (!input || !input->processed) return ArtifactBuildResult::Failure("texture cache write input missing");
    const auto& result = input->processed->result;
    auto done = ArtifactPayload::Make<TextureCacheWriteInput>(input);
    if (!result) return ArtifactBuildResult::Ready(std::move(done));
    std::shared_ptr<TextureSourceData> data;
    {
        std::lock_guard lock(result->mutex);
        data = result->data;
    }
    if (!data) return ArtifactBuildResult::Ready(std::move(done));
    const auto path = TextureProcessingManager::WriteCache(input->processed->cacheKey, *data);
    if (path.empty()) {
        spdlog::warn("Texture pipeline: could not write the cache for '{}'", input->processed->cacheKey);
        return ArtifactBuildResult::Ready(std::move(done));
    }
    // From here residency reads mip windows from the cache file; release the result.
    std::lock_guard lock(result->mutex);
    result->cachePath = path;
    result->data.reset();
    result->gpuImage.reset();
    return ArtifactBuildResult::Ready(std::move(done));
}

std::shared_ptr<org::PixelBuffer> UploadWindow(const TextureFactory& factory, const TextureAsset& texture,
    const br::textures::TextureRequest& request, const std::shared_ptr<TextureSourceData>& window)
{
    if (!window || window->desc.imageDimensions.empty()) return {};
    auto desc = window->desc;
    desc.hasRTV = request.allowRTV;
    desc.hasUAV = request.allowUAV;
    // The GPU builds missing levels only for formats it can write (not block-compressed ones, whose authored levels stand).
    desc.generateMipMaps = request.preparation == br::textures::TexturePreparation::LoadOnly &&
        request.generateMipMaps && !window->hasFullMipChain && !rhi::helpers::IsBlockCompressed(desc.format);
    auto initialData = TextureFactory::TextureInitialData::FromBytes(window->subresources);
    const auto& name = request.debugName.empty() ? request.source.identity : request.debugName;
    if (request.preparation == br::textures::TexturePreparation::Processed &&
        request.processing.isParticipatingMaterialTexture) {
        return factory.CreateMaterialResidentPixelBuffer(std::move(desc), std::move(initialData), name,
            request.processing.maxMipLevels);
    }
    return factory.CreateAlwaysResidentPixelBuffer(std::move(desc), std::move(initialData), name,
        false, false, request.processing.maxMipLevels);
}

ArtifactBuildResult BuildTextureResidency(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureResidencyInput>();
    if (!input || !input->services) return ArtifactBuildResult::Failure("texture residency input missing");
    const auto texture = input->texture.lock();
    if (!texture || (context.stopRequested && context.stopRequested())) return ArtifactBuildResult::Cancelled();
    if (texture->GetStreamingTextureID() != context.key.primaryID || !texture->PipelineRequest()) {
        return ArtifactBuildResult::Failure("texture residency identity mismatch");
    }
    const auto processedKey = TextureProcessedAddress(input->processedKeyHash);
    const auto dependency = std::ranges::find_if(context.dependencies,
        [&](const ArtifactSnapshot& snapshot) { return snapshot.key == processedKey; });
    const auto processed = dependency != context.dependencies.end()
        ? dependency->payload.Get<PublishedTextureProcessed>() : nullptr;
    if (!processed) return ArtifactBuildResult::Failure("texture residency has no processed texture");
    if (processed->cacheKey != texture->PipelineCacheKey()) {
        return ArtifactBuildResult::Failure("texture residency cache key collision");
    }

    texture->ApplyPipelineSourceShape(processed->fullDesc);
    const std::uint32_t topMip = texture->DesiredResidentTopMip();
    const auto& request = *texture->PipelineRequest();
    auto cachePath = processed->cachePath;
    std::shared_ptr<TextureSourceData> data;
    std::shared_ptr<org::PixelBuffer> gpuImage;
    if (processed->result) {
        std::lock_guard lock(processed->result->mutex);
        if (!processed->result->cachePath.empty()) cachePath = processed->result->cachePath;
        data = processed->result->data;
        gpuImage = processed->result->gpuImage;
    }

    std::shared_lock servicesLock(input->services->mutex);
    const auto* factory = input->services->factory;
    if (!factory) return ArtifactBuildResult::Cancelled();
    const auto adopt = [&](std::shared_ptr<org::PixelBuffer> image) {
        if (!image || !image->HasValidBackingResource()) {
            return ArtifactBuildResult::Failure("texture residency produced no image");
        }
        texture->AdoptPipelineImage(std::move(image));
        auto published = std::make_shared<PublishedTextureResidency>();
        published->streamingTextureID = texture->GetStreamingTextureID();
        published->topMip = topMip;
        published->bindingRevision = texture->GetBindingRevision();
        return ArtifactBuildResult::Ready(ArtifactPayload::Make<PublishedTextureResidency>(std::move(published)));
    };

    try {
        // A miss shows its result before its cache file exists.
        if (gpuImage && topMip == 0) return adopt(std::move(gpuImage));
        if (data && request.preparation == br::textures::TexturePreparation::LoadOnly) {
            // Loaded once, as it is: its pixels are not needed after the upload.
            auto result = adopt(UploadWindow(*factory, *texture, request, data));
            std::lock_guard lock(processed->result->mutex);
            processed->result->data.reset();
            return result;
        }
        if (data) return adopt(UploadWindow(*factory, *texture, request, br::textures::ShapeSourceForTopMip(data, topMip)));
        if (cachePath.empty()) return ArtifactBuildResult::Failure("texture residency has nothing to load");

        const auto checkpoint = context.checkpoint.Get<ResidencyCheckpoint>();
        if (checkpoint && checkpoint->upload && checkpoint->topMip == topMip) {
            auto state = checkpoint->upload->state.load(std::memory_order_acquire);
            if (state == TextureDirectStorageReloadJobState::Uploading) {
                const auto status = DirectStorageManager::GetInstance().PollRequest(checkpoint->upload->requestHandle);
                if (status.state == DirectStorageAsyncRequestState::Ready) state = TextureDirectStorageReloadJobState::Ready;
                else if (status.state == DirectStorageAsyncRequestState::Failed ||
                    status.state == DirectStorageAsyncRequestState::Invalid) state = TextureDirectStorageReloadJobState::Failed;
            }
            if (state == TextureDirectStorageReloadJobState::Ready) {
                std::shared_ptr<org::PixelBuffer> image;
                {
                    std::lock_guard lock(checkpoint->upload->mutex);
                    image = checkpoint->upload->uploadedImage;
                }
                return adopt(std::move(image));
            }
            if (state != TextureDirectStorageReloadJobState::Failed) return ArtifactBuildResult::Retry(1ms, context.checkpoint);
        }
        else if (DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::Gpu)) {
            if (auto upload = br::textures::BeginCacheWindowUpload(cachePath, topMip, request.allowRTV, request.allowUAV)) {
                auto next = std::make_shared<ResidencyCheckpoint>();
                next->upload = std::move(upload);
                next->topMip = topMip;
                return ArtifactBuildResult::Retry(1ms, ArtifactPayload::Make<ResidencyCheckpoint>(std::move(next)));
            }
        }
        // DirectStorage cannot serve it: read the window on the CPU.
        auto source = LoadTextureSourceDataFromConditionedCacheFilePath(ws2s(cachePath), "texture pipeline residency");
        return adopt(UploadWindow(*factory, *texture, request, br::textures::ShapeSourceForTopMip(source, topMip)));
    }
    catch (const std::exception& ex) {
        spdlog::warn("Texture pipeline: '{}' residency failed: {}", request.source.identity, ex.what());
        return ArtifactBuildResult::Failure(ex.what());
    }
}

} // namespace

std::uint64_t TextureCacheKeyHash(const std::string& cacheKey) {
    return std::hash<std::string>{}(cacheKey) | 1u;
}

void RegisterTexturePipelineProducers(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::TextureProcessed, {
        TaskLane::Background, TaskDomain::TextureProcessing,
        "TextureProcessed::Build", BuildTextureProcessed });
    graph.RegisterProducer(ArtifactKind::TextureCacheWrite, {
        TaskLane::Background, TaskDomain::TextureProcessing,
        "TextureCacheWrite::Build", BuildTextureCacheWrite });
    graph.RegisterProducer(ArtifactKind::TextureResidency, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "TextureResidency::Build", BuildTextureResidency });
}

} // namespace br::render
