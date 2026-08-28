#include "Render/RendererStateRequestService.h"

#include <algorithm>
#include <tuple>

namespace br::render {

RendererStateRequestService::RendererStateRequestService(
    AsyncStateGraph& graph, RendererStatePublisher& publisher)
    : m_graph(graph), m_publisher(publisher) {
    m_graph.RegisterProducer(ArtifactKind::FrameManifest, {
        TaskLane::Streaming, TaskDomain::RendererState,
        "RendererStateRequestService::BuildManifest", &RendererStateRequestService::BuildManifest });
}

RendererStateRequestService::~RendererStateRequestService() { Stop(); }

ArtifactRequestResult RendererStateRequestService::Request(ArtifactAddress key, std::uint64_t revision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t inputFingerprint) {
    if (!m_accepting.load(std::memory_order_acquire)) {
        return { ArtifactRequestStatus::ShuttingDown, 0, {} };
    }
    return m_graph.Request(key, revision, std::move(requirements), std::move(input),
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
            return value.Version() == artifact.Version();
        });
        if (existing != roots.end()) *existing = artifact;
        else roots.push_back(artifact);

        const auto active = m_publisher.Active();
        const auto& activeFragment = active ? active->Fragment(fragment->kind) : PublishedStateFragment{};
        const auto newest = std::ranges::max_element(roots, [](const ArtifactSnapshot& left,
            const ArtifactSnapshot& right) {
            return std::tie(left.revision, left.generation) <
                std::tie(right.revision, right.generation);
        });
        const auto newestVersion = newest != roots.end() ? newest->Version() : ArtifactVersionID{};
        std::erase_if(roots, [&](const ArtifactSnapshot& value) {
            const bool isNewest = value.Version() == newestVersion;
            const bool isActive = active && value.Version() == activeFragment.publicationRoot;
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
                return value.Version() == root.Version();
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
                if (selected.publicationRoot != dependency.Version()) return false;
            }
        }
        return true;
    };

    using Selection = std::array<std::optional<ArtifactSnapshot>, kPublishedFragmentCount>;
    const auto sameArtifact = [](const ArtifactSnapshot& left, const ArtifactSnapshot& right) {
        return left.Version() == right.Version();
    };
    const auto publicationReady = [](auto&& self, const ArtifactSnapshot& root,
        std::vector<ArtifactVersionID>& visited) -> bool {
        const auto requiredReadiness = root.gpuSubmissions
            ? ArtifactReadiness::UploadSubmitted : ArtifactReadiness::CpuReady;
        if (!root.Version() || !ArtifactReachedMilestone(
            root.readiness, requiredReadiness)) return false;
        if (std::ranges::contains(visited, root.Version())) return true;
        visited.push_back(root.Version());
        const auto artifact = root.payload.Get<RendererStateFragmentArtifact>();
        if (!artifact) return true;
        return std::ranges::all_of(artifact->fragment.dependencyClosure,
            [&](const ArtifactSnapshot& dependency) {
                return self(self, dependency, visited);
            });
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
    const auto makeBundle = [](const ArtifactSnapshot& root) {
        auto bundle = std::make_shared<PublicationBundle>();
        bundle->root = root.Version();
        const auto append = [&](auto&& self, const ArtifactSnapshot& snapshot) -> bool {
            const auto requiredReadiness = snapshot.gpuSubmissions
                ? ArtifactReadiness::UploadSubmitted : ArtifactReadiness::CpuReady;
            if (!snapshot.Version() || !ArtifactReachedMilestone(
                snapshot.readiness, requiredReadiness)) return false;
            if (std::ranges::any_of(bundle->versions, [&](const ArtifactVersionID& value) {
                return value == snapshot.Version();
            })) return true;
            bundle->versions.push_back(snapshot.Version());
            bundle->leases.Add(snapshot.lease);
            if (snapshot.gpuSubmissions && !std::ranges::contains(
                bundle->gpuSubmissions, snapshot.gpuSubmissions)) {
                bundle->gpuSubmissions.push_back(snapshot.gpuSubmissions);
            }
            const auto fragment = snapshot.payload.Get<RendererStateFragmentArtifact>();
            if (!fragment) return true;
            bundle->resourceHolds.insert(bundle->resourceHolds.end(),
                fragment->fragment.resourceHolds.begin(),
                fragment->fragment.resourceHolds.end());
            for (const auto& dependency : fragment->fragment.dependencyClosure) {
                if (!self(self, dependency)) return false;
            }
            return true;
        };
        return append(append, root) ? bundle : std::shared_ptr<PublicationBundle>{};
    };
    const auto materialize = [&](const Selection& selection, bool buildBundles) {
        auto candidate = input->base ? *input->base : PublishedRendererState{};
        for (std::size_t index = 0; index < selection.size(); ++index) {
            if (!selection[index]) continue;
            const auto artifact = selection[index]->payload.Get<RendererStateFragmentArtifact>();
            auto fragment = artifact->fragment;
            fragment.publicationRoot = selection[index]->Version();
            if (buildBundles) fragment.publicationBundle = makeBundle(*selection[index]);
            candidate.Fragment(static_cast<PublishedFragmentKind>(index)) = std::move(fragment);
        }
        return candidate;
    };
    const auto monotonicSuccessor = [&](const Selection& selection) {
        if (!input->base) return true;
        for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
            if (!selection[index]) continue;
            const auto kind = static_cast<PublishedFragmentKind>(index);
            const auto artifact = selection[index]->payload.Get<RendererStateFragmentArtifact>();
            auto successor = artifact->fragment;
            successor.publicationRoot = selection[index]->Version();
            if (!IsMonotonicFragmentSuccessor(
                input->base->Fragment(kind), successor)) return false;
        }
        return true;
    };

    std::vector<Selection> closures;
    closures.reserve(input->roots.size());
    for (const auto& root : input->roots) {
        std::vector<ArtifactVersionID> visited;
        if (!publicationReady(publicationReady, root, visited)) continue;
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
    const auto candidateScore = [&](const Selection& selection) {
        std::size_t advancingFragments = 0;
        std::uint64_t advancement = 0;
        const auto [fragmentCount, revisionSum] = closureScore(selection);
        for (std::size_t index = 0; index < selection.size(); ++index) {
            if (!selection[index]) continue;
            const auto kind = static_cast<PublishedFragmentKind>(index);
            const auto activeRoot = input->base
                ? input->base->Fragment(kind).publicationRoot : ArtifactVersionID{};
            if (activeRoot == selection[index]->Version()) continue;
            ++advancingFragments;
            if (activeRoot.address == selection[index]->key &&
                selection[index]->revision >= activeRoot.revision) {
                advancement += selection[index]->revision - activeRoot.revision;
            } else {
                advancement += selection[index]->revision;
            }
        }
        return std::tuple{ advancingFragments, advancement, fragmentCount, revisionSum };
    };
    std::ranges::sort(closures, [&](const Selection& left, const Selection& right) {
        return closureScore(left) > closureScore(right);
    });

    Selection selected;
    std::optional<decltype(candidateScore(selected))> bestScore;
    for (std::size_t seed = 0; seed < closures.size(); ++seed) {
        auto trial = closures[seed];
        for (std::size_t index = 0; index < closures.size(); ++index) {
            if (index == seed) continue;
            auto merged = trial;
            if (merge(merged, closures[index])) trial = std::move(merged);
        }
        const auto score = candidateScore(trial);
        if ((bestScore && score <= *bestScore) || !monotonicSuccessor(trial)) continue;
        const auto candidate = materialize(trial, false);
        if (coherent(candidate)) {
            bestScore = score;
            selected = std::move(trial);
        }
    }
    if (!bestScore) return ArtifactBuildResult::Cancelled();

    auto patch = std::make_shared<PublishedStatePatch>();
    patch->sourceEpoch = input->baseEpoch;
    std::uint64_t selectedMask = 0;
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (!selected[index]) continue;
        const auto kind = static_cast<PublishedFragmentKind>(index);
        if (input->base && input->base->Fragment(kind).publicationRoot ==
            selected[index]->Version()) continue;
        const auto artifact = selected[index]->payload.Get<RendererStateFragmentArtifact>();
        auto fragment = artifact->fragment;
        fragment.publicationRoot = selected[index]->Version();
        fragment.publicationBundle = makeBundle(*selected[index]);
        patch->fragments[index] = std::move(fragment);
        selectedMask |= PublishedFragmentMask(kind);
        patch->catalogOwnerMask |= artifact->catalogOwnerMask != 0
            ? artifact->catalogOwnerMask : PublishedFragmentMask(kind);
        patch->catalogEntries.insert(patch->catalogEntries.end(),
            artifact->catalogEntries.begin(), artifact->catalogEntries.end());
        for (const auto& [key, resources] : artifact->catalogEntries) {
            PublishedResourceSelection selection;
            selection.resources = resources;
            selection.contentVersion = selected[index]->revision;
            selection.sourceArtifact = selected[index]->Version();
            selection.lifetimeHolds = artifact->fragment.resourceHolds;
            selection.publicationBundle = patch->fragments[index]->publicationBundle;
            if (selection.publicationBundle) {
                selection.gpuSubmissions = selection.publicationBundle->gpuSubmissions;
            }
            patch->catalogSelections.emplace_back(key, std::move(selection));
        }
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
                patch->preconditions.push_back({ kind, fragment.publicationRoot });
            }
        }
    }
    auto manifest = std::make_shared<FrameManifestPayload>();
    manifest->baseEpoch = input->baseEpoch;
    manifest->patch = std::move(patch);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<FrameManifestPayload>(std::move(manifest)));
}

void RendererStateRequestService::Stop() {
    if (!m_accepting.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard lock(m_mutex);
    for (auto& roots : m_roots) roots.clear();
}

} // namespace br::render
