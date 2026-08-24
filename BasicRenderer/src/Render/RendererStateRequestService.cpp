#include "Render/RendererStateRequestService.h"

namespace br::render {

RendererStateRequestService::RendererStateRequestService(
    AsyncStateGraph& graph, RendererStatePublisher& publisher)
    : m_graph(graph), m_publisher(publisher) {
    m_graph.RegisterProducer(ArtifactKind::FrameManifest, {
        TaskLane::Streaming, TaskDomain::RendererState,
        "RendererStateRequestService::BuildManifest", &RendererStateRequestService::BuildManifest });
}

RendererStateRequestService::~RendererStateRequestService() { Stop(); }

bool RendererStateRequestService::Request(ArtifactKey key, std::uint64_t revision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input) {
    return m_accepting.load(std::memory_order_acquire) &&
        m_graph.Request(key, revision, std::move(requirements), std::move(input));
}

bool RendererStateRequestService::Invalidate(ArtifactKey key, std::uint64_t revision) {
    return m_accepting.load(std::memory_order_acquire) && m_graph.Invalidate(key, revision);
}

void RendererStateRequestService::Cancel(ArtifactKey key) { m_graph.Cancel(key); }
ArtifactDiagnostic RendererStateRequestService::Diagnose(ArtifactKey key) const { return m_graph.Diagnose(key); }

void RendererStateRequestService::OnArtifactReady(const ArtifactSnapshot& artifact) {
    if (!m_accepting.load(std::memory_order_acquire)) return;
    if (artifact.key.kind == ArtifactKind::FrameManifest) {
        (void)m_publisher.PublishArtifact(artifact);
        return;
    }
    const auto fragment = artifact.payload.Get<RendererStateFragmentArtifact>();
    if (!fragment || !fragment->publishRoot) return;
    {
        std::lock_guard lock(m_mutex);
        m_roots.insert_or_assign(artifact.key, artifact);
    }
    RequestManifest();
}

void RendererStateRequestService::OnCandidateRejected(std::uint64_t) {
    if (m_accepting.load(std::memory_order_acquire)) RequestManifest();
}

void RendererStateRequestService::RequestManifest() {
    auto input = std::make_shared<ManifestInput>();
    {
        std::lock_guard lock(m_mutex);
        input->base = m_publisher.Active();
        input->baseEpoch = input->base ? input->base->epoch : 0u;
        input->roots.reserve(m_roots.size());
        for (const auto& [_, root] : m_roots) input->roots.push_back(root);
        ++m_manifestRevision;
    }
    (void)m_graph.Request({ ArtifactKind::FrameManifest, 0, 0 }, m_manifestRevision, {},
        ArtifactPayload::Make<ManifestInput>(std::move(input)));
}

ArtifactBuildResult RendererStateRequestService::BuildManifest(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<ManifestInput>();
    if (!input) return ArtifactBuildResult::Failure("frame manifest input type mismatch");
    auto state = input->base ? std::make_shared<PublishedRendererState>(*input->base)
                             : std::make_shared<PublishedRendererState>();
    auto catalog = state->resourceCatalog
        ? std::make_shared<PublishedResourceCatalog>(*state->resourceCatalog)
        : std::make_shared<PublishedResourceCatalog>();
    for (const auto& root : input->roots) {
        const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
        if (!artifact) return ArtifactBuildResult::Failure("manifest root payload type mismatch");
        switch (artifact->kind) {
        case PublishedFragmentKind::Materials: state->materials = artifact->fragment; break;
        case PublishedFragmentKind::Terrain: state->terrain = artifact->fragment; break;
        case PublishedFragmentKind::Geometry: state->geometry = artifact->fragment; break;
        case PublishedFragmentKind::DrawRecords: state->drawRecords = artifact->fragment; break;
        case PublishedFragmentKind::ActiveDrawLists: state->activeDrawLists = artifact->fragment; break;
        case PublishedFragmentKind::IndirectWorkloads: state->indirectWorkloads = artifact->fragment; break;
        }
        for (auto entry = catalog->entries.begin(); entry != catalog->entries.end();) {
            const bool ownedByRoot = entry->first.owner == artifact->kind ||
                (artifact->kind == PublishedFragmentKind::IndirectWorkloads &&
                    entry->first.owner == PublishedFragmentKind::ActiveDrawLists);
            if (ownedByRoot) entry = catalog->entries.erase(entry);
            else ++entry;
        }
        for (const auto& [key, resources] : artifact->catalogEntries) catalog->entries[key] = resources;
    }
    state->resourceCatalog = std::move(catalog);
    auto manifest = std::make_shared<FrameManifestPayload>();
    manifest->baseEpoch = input->baseEpoch;
    manifest->state = std::move(state);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<FrameManifestPayload>(std::move(manifest)));
}

void RendererStateRequestService::Stop() {
    if (!m_accepting.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard lock(m_mutex);
    m_roots.clear();
}

} // namespace br::render
