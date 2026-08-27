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
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t inputFingerprint) {
    return m_accepting.load(std::memory_order_acquire) &&
        m_graph.Request(key, revision, std::move(requirements), std::move(input),
            inputFingerprint);
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
        ArtifactPayload::Make<ManifestInput>(std::move(input)), m_manifestRevision);
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

    using Selection = std::array<std::optional<ArtifactSnapshot>, kPublishedFragmentCount>;
    const auto sameArtifact = [](const ArtifactSnapshot& left, const ArtifactSnapshot& right) {
        return left.key == right.key && left.revision == right.revision;
    };
    const auto addClosure = [&](auto&& self, const ArtifactSnapshot& root, Selection& selection) -> bool {
        const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
        if (!artifact || !artifact->publishRoot || artifact->kind == PublishedFragmentKind::Count) {
            return false;
        }
        auto& slot = selection[static_cast<std::size_t>(artifact->kind)];
        if (slot) return sameArtifact(*slot, root);
        slot = root;
        for (const auto& dependency : artifact->fragment.dependencyClosure) {
            const auto dependencyRoot = dependency.payload.Get<RendererStateFragmentArtifact>();
            if (dependencyRoot && dependencyRoot->publishRoot &&
                !self(self, dependency, selection)) return false;
        }
        return true;
    };
    const auto merge = [&](Selection& destination, const Selection& source) {
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (destination[index] && source[index] &&
                !sameArtifact(*destination[index], *source[index])) return false;
        }
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (source[index]) destination[index] = source[index];
        }
        return true;
    };
    const auto materialize = [&](const Selection& selection) {
        auto candidate = input->base ? *input->base : PublishedRendererState{};
        for (std::size_t index = 0; index < selection.size(); ++index) {
            if (!selection[index]) continue;
            const auto artifact = selection[index]->payload.Get<RendererStateFragmentArtifact>();
            auto fragment = artifact->fragment;
            fragment.publicationRoot = selection[index]->key;
            candidate.Fragment(static_cast<PublishedFragmentKind>(index)) = std::move(fragment);
        }
        return candidate;
    };

    std::vector<Selection> closures;
    closures.reserve(input->roots.size());
    for (const auto& root : input->roots) {
        Selection closure;
        if (addClosure(addClosure, root, closure)) closures.push_back(std::move(closure));
    }
    const auto closureScore = [](const Selection& selection) {
        std::pair<std::size_t, std::uint64_t> result{};
        for (const auto& root : selection) {
            if (!root) continue;
            ++result.first;
            result.second += root->revision;
        }
        return result;
    };
    std::ranges::sort(closures, [&](const Selection& left, const Selection& right) {
        return closureScore(left) > closureScore(right);
    });

    Selection selected;
    std::pair<std::size_t, std::uint64_t> bestScore{};
    for (std::size_t seed = 0; seed < closures.size(); ++seed) {
        auto trial = closures[seed];
        for (std::size_t index = 0; index < closures.size(); ++index) {
            if (index == seed) continue;
            auto merged = trial;
            if (merge(merged, closures[index])) trial = std::move(merged);
        }
        const auto candidate = materialize(trial);
        const auto score = closureScore(trial);
        if (coherent(candidate) && score > bestScore) {
            bestScore = score;
            selected = std::move(trial);
        }
    }
    if (bestScore.first == 0) return ArtifactBuildResult::Cancelled();

    auto state = std::make_shared<PublishedRendererState>(materialize(selected));
    auto catalog = state->resourceCatalog
        ? std::make_shared<PublishedResourceCatalog>(*state->resourceCatalog)
        : std::make_shared<PublishedResourceCatalog>();
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (!selected[index]) continue;
        const auto& root = *selected[index];
        const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
        if (!artifact) return ArtifactBuildResult::Failure("manifest root payload type mismatch");
        const auto ownerMask = artifact->catalogOwnerMask != 0
            ? artifact->catalogOwnerMask : PublishedFragmentMask(artifact->kind);
        for (auto entry = catalog->entries.begin(); entry != catalog->entries.end();) {
            if ((ownerMask & PublishedFragmentMask(entry->first.owner)) != 0) {
                catalog->contentVersions.erase(entry->first);
                entry = catalog->entries.erase(entry);
            } else ++entry;
        }
        for (const auto& [key, resources] : artifact->catalogEntries) {
            catalog->entries[key] = resources;
            catalog->contentVersions[key] = root.revision;
        }
    }
    if (!coherent(*state)) return ArtifactBuildResult::Failure("manifest dependency closure changed during build");
    state->resourceCatalog = std::move(catalog);
    auto patch = std::make_shared<PublishedStatePatch>();
    patch->sourceEpoch = input->baseEpoch;
    std::uint64_t selectedMask = 0;
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (!selected[index]) continue;
        const auto kind = static_cast<PublishedFragmentKind>(index);
        const auto artifact = selected[index]->payload.Get<RendererStateFragmentArtifact>();
        auto fragment = artifact->fragment;
        fragment.publicationRoot = selected[index]->key;
        patch->fragments[index] = std::move(fragment);
        selectedMask |= PublishedFragmentMask(kind);
        patch->catalogOwnerMask |= artifact->catalogOwnerMask != 0
            ? artifact->catalogOwnerMask : PublishedFragmentMask(kind);
        patch->catalogEntries.insert(patch->catalogEntries.end(),
            artifact->catalogEntries.begin(), artifact->catalogEntries.end());
    }
    // Only unchanged fragments whose exact closure mentions a replaced slot
    // constrain rebasing. Completely independent fragments may advance while
    // this manifest is being built.
    if (input->base) {
        for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
            const auto kind = static_cast<PublishedFragmentKind>(index);
            if ((selectedMask & PublishedFragmentMask(kind)) != 0) continue;
            const auto& fragment = input->base->Fragment(kind);
            const bool observesReplacement = std::ranges::any_of(
                fragment.dependencyClosure, [&](const ArtifactSnapshot& dependency) {
                    const auto root = dependency.payload.Get<RendererStateFragmentArtifact>();
                    return root && root->publishRoot &&
                        (selectedMask & PublishedFragmentMask(root->kind)) != 0;
                });
            if (observesReplacement) {
                patch->preconditions.push_back(
                    { kind, fragment.publicationRoot, fragment.revision });
            }
        }
    }
    auto manifest = std::make_shared<FrameManifestPayload>();
    manifest->baseEpoch = input->baseEpoch;
    manifest->state = std::move(state);
    manifest->patch = std::move(patch);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<FrameManifestPayload>(std::move(manifest)));
}

void RendererStateRequestService::Stop() {
    if (!m_accepting.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard lock(m_mutex);
    for (auto& roots : m_roots) roots.clear();
}

} // namespace br::render
