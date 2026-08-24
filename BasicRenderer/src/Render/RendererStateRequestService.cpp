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
        auto& roots = m_roots[index];
        const auto existing = std::ranges::find_if(roots, [&](const ArtifactSnapshot& value) {
            return value.key == artifact.key && value.revision == artifact.revision;
        });
        if (existing != roots.end()) *existing = artifact;
        else roots.push_back(artifact);

        const auto active = m_publisher.Active();
        const auto& activeFragment = active ? active->Fragment(fragment->kind) : PublishedStateFragment{};
        const auto newest = std::ranges::max_element(roots, {}, &ArtifactSnapshot::revision);
        const auto newestKey = newest != roots.end() ? newest->key : ArtifactKey{};
        const auto newestRevision = newest != roots.end() ? newest->revision : 0u;
        std::erase_if(roots, [&](const ArtifactSnapshot& value) {
            const bool isNewest = value.key == newestKey && value.revision == newestRevision;
            const bool isActive = active && value.key == activeFragment.publicationRoot &&
                value.revision == activeFragment.revision;
            return !isNewest && !isActive;
        });
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
        input->roots.reserve(m_roots.size() * 2u);
        const auto appendRoot = [&input](const ArtifactSnapshot& root) {
            const auto duplicate = std::ranges::any_of(input->roots, [&](const ArtifactSnapshot& value) {
                return value.key == root.key && value.revision == root.revision;
            });
            if (!duplicate) input->roots.push_back(root);
        };
        for (const auto& roots : m_roots) {
            for (const auto& root : roots) appendRoot(root);
        }
        // A ready successor may have been built from an intermediate root that
        // is no longer the active or newest root for that slot. Its exact
        // dependency snapshot is the authoritative history needed to assemble
        // the coherent combination; do not require producers to republish it.
        for (std::size_t cursor = 0; cursor < input->roots.size(); ++cursor) {
            const auto artifact = input->roots[cursor].payload.Get<RendererStateFragmentArtifact>();
            if (!artifact) continue;
            for (const auto& dependency : artifact->fragment.dependencyClosure) {
                const auto dependencyRoot = dependency.payload.Get<RendererStateFragmentArtifact>();
                if (dependencyRoot && dependencyRoot->publishRoot) appendRoot(dependency);
            }
        }
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

    if (input->roots.size() >= 63u) {
        return ArtifactBuildResult::Failure("frame manifest root history exceeds selection mask capacity");
    }
    std::uint64_t selectedMask = 0;
    const auto combinationCount = std::uint64_t{ 1 } << input->roots.size();
    std::size_t bestCount = 0;
    std::uint64_t bestRevisionScore = 0;
    for (std::uint64_t mask = 1; mask < combinationCount; ++mask) {
            auto candidate = input->base ? *input->base : PublishedRendererState{};
            std::size_t selectedCount = 0;
            std::uint64_t revisionScore = 0;
            std::uint64_t selectedKinds = 0;
            bool valid = true;
            for (std::size_t index = 0; index < input->roots.size(); ++index) {
                if ((mask & (std::uint64_t{ 1 } << index)) == 0) continue;
                const auto& root = input->roots[index];
                const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
                if (!artifact || artifact->kind == PublishedFragmentKind::Count) { valid = false; break; }
                const auto kindMask = PublishedFragmentMask(artifact->kind);
                if ((selectedKinds & kindMask) != 0) { valid = false; break; }
                selectedKinds |= kindMask;
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
    for (auto& roots : m_roots) roots.clear();
}

} // namespace br::render
