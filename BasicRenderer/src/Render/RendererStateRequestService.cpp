#include "Render/RendererStateRequestService.h"

#include <algorithm>

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
        const auto index = static_cast<std::size_t>(fragment->kind);
        if (index >= m_roots.size()) return;
        auto& root = m_roots[index];
        if (!root || artifact.revision >= root->revision) root = artifact;
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
        for (const auto& root : m_roots) if (root) input->roots.push_back(*root);
        ++m_manifestRevision;
    }
    (void)m_graph.Request({ ArtifactKind::FrameManifest, 0, 0 }, m_manifestRevision, {},
        ArtifactPayload::Make<ManifestInput>(std::move(input)));
}

ArtifactBuildResult RendererStateRequestService::BuildManifest(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<ManifestInput>();
    if (!input) return ArtifactBuildResult::Failure("frame manifest input type mismatch");

    const auto coherent = [](const PublishedRendererState& candidate) {
        for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
            const auto kind = static_cast<PublishedFragmentKind>(index);
            const auto& fragment = candidate.Fragment(kind);
            if (fragment.revision == 0) continue;
            for (const auto& dependency : fragment.dependencyClosure) {
                const auto root = dependency.payload.Get<RendererStateFragmentArtifact>();
                if (!root || !root->publishRoot) continue;
                const auto& selected = candidate.Fragment(root->kind);
                if (selected.publicationRoot != dependency.key ||
                    selected.revision != dependency.revision) return false;
            }
        }
        return true;
    };

    std::uint64_t selectedMask = 0;
    if (const auto checkpoint = context.checkpoint.Get<ManifestSelection>()) {
        selectedMask = checkpoint->rootMask;
    } else {
        const auto combinationCount = std::uint64_t{ 1 } << input->roots.size();
        std::size_t bestCount = 0;
        std::uint64_t bestRevisionScore = 0;
        for (std::uint64_t mask = 1; mask < combinationCount; ++mask) {
            auto candidate = input->base ? *input->base : PublishedRendererState{};
            std::size_t selectedCount = 0;
            std::uint64_t revisionScore = 0;
            bool valid = true;
            for (std::size_t index = 0; index < input->roots.size(); ++index) {
                if ((mask & (std::uint64_t{ 1 } << index)) == 0) continue;
                const auto& root = input->roots[index];
                const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
                if (!artifact || artifact->kind == PublishedFragmentKind::Count) { valid = false; break; }
                auto fragment = artifact->fragment;
                fragment.publicationRoot = root.key;
                candidate.Fragment(artifact->kind) = std::move(fragment);
                ++selectedCount;
                revisionScore += root.revision;
            }
            if (!valid || !coherent(candidate)) continue;
            if (selectedCount > bestCount ||
                (selectedCount == bestCount && revisionScore > bestRevisionScore)) {
                selectedMask = mask;
                bestCount = selectedCount;
                bestRevisionScore = revisionScore;
            }
        }
        if (selectedMask == 0) return ArtifactBuildResult::Cancelled();
        std::vector<ArtifactRequirement> requirements;
        requirements.reserve(input->roots.size());
        for (std::size_t index = 0; index < input->roots.size(); ++index) {
            if ((selectedMask & (std::uint64_t{ 1 } << index)) == 0) continue;
            const auto& root = input->roots[index];
            requirements.push_back({ root.key, root.revision, ArtifactReadiness::GpuReady });
        }
        auto selection = std::make_shared<ManifestSelection>();
        selection->rootMask = selectedMask;
        return ArtifactBuildResult::Needs(std::move(requirements),
            ArtifactPayload::Make<ManifestSelection>(std::move(selection)));
    }

    auto state = input->base ? std::make_shared<PublishedRendererState>(*input->base)
                             : std::make_shared<PublishedRendererState>();
    auto catalog = state->resourceCatalog
        ? std::make_shared<PublishedResourceCatalog>(*state->resourceCatalog)
        : std::make_shared<PublishedResourceCatalog>();
    for (std::size_t index = 0; index < input->roots.size(); ++index) {
        if ((selectedMask & (std::uint64_t{ 1 } << index)) == 0) continue;
        const auto& root = input->roots[index];
        const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
        if (!artifact) return ArtifactBuildResult::Failure("manifest root payload type mismatch");
        auto fragment = artifact->fragment;
        fragment.publicationRoot = root.key;
        state->Fragment(artifact->kind) = std::move(fragment);
        const auto ownerMask = artifact->catalogOwnerMask != 0
            ? artifact->catalogOwnerMask : PublishedFragmentMask(artifact->kind);
        for (auto entry = catalog->entries.begin(); entry != catalog->entries.end();) {
            if ((ownerMask & PublishedFragmentMask(entry->first.owner)) != 0) entry = catalog->entries.erase(entry);
            else ++entry;
        }
        for (const auto& [key, resources] : artifact->catalogEntries) catalog->entries[key] = resources;
    }
    if (!coherent(*state)) return ArtifactBuildResult::Failure("manifest dependency closure changed during build");
    state->resourceCatalog = std::move(catalog);
    auto manifest = std::make_shared<FrameManifestPayload>();
    manifest->baseEpoch = input->baseEpoch;
    manifest->state = std::move(state);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<FrameManifestPayload>(std::move(manifest)));
}

void RendererStateRequestService::Stop() {
    if (!m_accepting.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard lock(m_mutex);
    for (auto& root : m_roots) root.reset();
}

} // namespace br::render
