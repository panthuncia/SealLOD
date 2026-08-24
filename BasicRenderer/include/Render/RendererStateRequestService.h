#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <optional>

#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"

namespace br::render {

class RendererStateRequestService {
public:
    RendererStateRequestService(AsyncStateGraph& graph, RendererStatePublisher& publisher);
    ~RendererStateRequestService();

    bool Request(ArtifactKey key, std::uint64_t revision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {});
    bool Invalidate(ArtifactKey key, std::uint64_t revision);
    void Cancel(ArtifactKey key);
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    void Stop();

    // Scheduler-boundary callback. Public so Renderer can wire it without
    // exposing graph producer registration to RendererHost.
    void OnArtifactReady(const ArtifactSnapshot& artifact);
    void OnCandidateRejected(std::uint64_t activeEpoch);

private:
    struct ManifestInput {
        std::uint64_t baseEpoch = 0;
        std::shared_ptr<const PublishedRendererState> base;
        std::vector<ArtifactSnapshot> roots;
    };
    struct ManifestSelection { std::uint64_t rootMask = 0; };

    void RequestManifest();
    static ArtifactBuildResult BuildManifest(const ArtifactBuildContext& context);

    AsyncStateGraph& m_graph;
    RendererStatePublisher& m_publisher;
    mutable std::mutex m_mutex;
    std::array<std::optional<ArtifactSnapshot>, kPublishedFragmentCount> m_roots;
    std::uint64_t m_manifestRevision = 0;
    std::atomic_bool m_accepting{ true };
};

} // namespace br::render
