#include "Render/TextureImageTableArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {

ArtifactBuildResult BuildTextureImageTable(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureImageTableBuildInput>();
    if (!input || input->contentEpoch != context.revision) {
        return ArtifactBuildResult::Failure("texture-image-table input identity mismatch");
    }
    std::shared_ptr<const PublishedGpuBufferVersion> buffer;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key != input->bufferKey) continue;
        const auto fragment = dependency.payload.Get<RendererStateFragmentArtifact>();
        buffer = fragment ? fragment->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
        if (buffer && buffer->writeSequence == input->contentEpoch) break;
        buffer.reset();
    }
    if (!buffer) return ArtifactBuildResult::Retry(std::chrono::milliseconds(1));

    auto table = std::make_shared<PublishedTextureImageTable>();
    table->bindingEpoch = context.revision;
    table->contentEpoch = input->contentEpoch;
    table->logicalExtent = input->logicalExtent;
    table->table = buffer;
    table->holdChunks = input->holdChunks;

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::TextureImages;
    root->fragment.revision = context.revision;
    root->fragment.dependencyClosure = context.dependencies;
    for (const auto& chunk : table->holdChunks) root->fragment.resourceHolds.push_back(chunk);
    root->fragment.payload = ArtifactPayload::Make<PublishedTextureImageTable>(table);
    auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    resources->push_back(buffer->resource);
    root->catalogEntries.emplace_back(PublishedResourceKey{
        PublishedFragmentKind::TextureImages, PublishedResourceUsage::ShaderResource,
        0, 0, kTextureImageTableBufferVariant }, std::move(resources));

    auto result = ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
    if (input->bufferFamily) {
        auto family = input->bufferFamily;
        result.acceptance = { TaskLane::Streaming, TaskDomain::MaterialAcceptance,
            [family, buffer, epoch = input->contentEpoch](const ArtifactSnapshot&) {
                family->Acknowledge(buffer);
                basic_telemetry::SetGauge("SARP.TextureStreaming.ImageTablePublishedEpoch",
                    static_cast<std::int64_t>(epoch));
                basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableEpochPublished");
            } };
    }
    return result;
}

} // namespace

void RegisterTextureImageTableProducer(AsyncStateGraph& graph) {
    graph.RegisterTypedProducer<TextureImageTableBuildInput, RendererStateFragmentArtifact>(
        ArtifactKind::TextureImageTable, TaskLane::Streaming, TaskDomain::GraphPublication,
        "TextureImageTableArtifact::Build",
        [](const ArtifactBuildContext& context,
            std::shared_ptr<const TextureImageTableBuildInput>) {
            return BuildTextureImageTable(context);
        });
}

} // namespace br::render
