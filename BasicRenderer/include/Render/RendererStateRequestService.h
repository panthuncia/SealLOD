#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <optional>
#include <map>

#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"

namespace br::render {

class RendererStateRequestService {
public:
    RendererStateRequestService(AsyncStateGraph& graph, RendererStatePublisher& publisher);
    void RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration) {
        m_graph.RegisterProducer(kind, std::move(registration));
    }
    [[nodiscard]] std::function<void(std::uint64_t)> MakeSuspensionNotifier() {
        return m_graph.MakeSuspensionNotifier();
    }
    [[nodiscard]] std::uint64_t AllocateSuspensionIdentity() const noexcept {
        return AsyncStateGraph::AllocateSuspensionIdentity();
    }
    ~RendererStateRequestService();

    ArtifactRequestResult Request(ArtifactAddress address, std::uint64_t revision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t inputFingerprint = 0);
    ArtifactRequestResult RequestExact(ArtifactAddress address, std::uint64_t revision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t inputFingerprint = 0);
    ArtifactRequestResult SubmitLatest(ArtifactIntent intent);
    std::vector<ArtifactRequestResult> SubmitLatestBatch(std::vector<ArtifactIntent> intents);
    bool Invalidate(ArtifactKey key, std::uint64_t revision);
    void Cancel(ArtifactKey key);
    void Release(ArtifactKey key) { m_graph.Release(key); }
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactAddress address) const {
        return m_graph.Snapshot(address);
    }
    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactVersionID version) const {
        return m_graph.Snapshot(version);
    }
    [[nodiscard]] ArtifactObservation ObserveWithSnapshot(ArtifactAddress address,
        std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback) {
        return m_graph.ObserveWithSnapshot(address, std::move(callback));
    }
	[[nodiscard]] ArtifactObservation ObserveKind(ArtifactKind kind,
		std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback) {
		return m_graph.ObserveKind(kind, std::move(callback));
	}
	[[nodiscard]] ArtifactAwaiter AwaitExact(ArtifactVersionHandle handle,
		ArtifactReadiness milestone, TaskLane lane, TaskDomain domain,
		std::function<void(const ArtifactSnapshot&)> continuation) {
		return m_graph.AwaitExact(std::move(handle), milestone, lane, domain,
			std::move(continuation));
	}
    [[nodiscard]] AsyncStateGraphStats Stats() const { return m_graph.Stats(); }
    [[nodiscard]] std::uint64_t Outstanding(ArtifactKind kind) const {
        return m_graph.Outstanding(kind);
    }
    [[nodiscard]] bool TraceActive() const { return m_graph.TraceActive(); }
    void TraceEvent(AsyncStateGraphTraceEventID event, ArtifactAddress address,
        std::uint64_t revision = 0, std::uint64_t generation = 0,
        AsyncStateGraphTracePayload payload = {}, ArtifactAddress related = {},
        std::uint64_t relatedRevision = 0) {
        m_graph.TraceEvent(event, address, revision, generation, payload,
            related, relatedRevision);
    }
    [[nodiscard]] std::uint64_t SubscribeReady(
        std::function<void(const ArtifactSnapshot&)> callback) {
        return m_graph.AddReadyCallback(std::move(callback));
    }
    void UnsubscribeReady(std::uint64_t subscription) {
        m_graph.RemoveReadyCallback(subscription);
    }
    void Stop();

    // Scheduler-boundary callback. Public so Renderer can wire it without
    // exposing graph producer registration to RendererHost.
    void OnArtifactReady(const ArtifactSnapshot& artifact);
    void OnCandidateRejected(std::uint64_t activeEpoch);
    // Cheap level-triggered wake used once per frame. It only submits work when
    // a retained ready root is newer than the active published fragment.
    void RefreshPublication();

private:
    struct PublicationNodeCache {
        std::mutex mutex;
        std::map<ArtifactVersionID, std::weak_ptr<const PublicationBundle>> nodes;
    };
    struct ManifestInput {
        std::uint64_t baseEpoch = 0;
        std::shared_ptr<const PublishedRendererState> base;
        std::vector<ArtifactSnapshot> roots;
        std::shared_ptr<PublicationNodeCache> publicationNodes;
    };
    void RequestManifest();
    static ArtifactBuildResult BuildManifest(const ArtifactBuildContext& context);

    AsyncStateGraph& m_graph;
    RendererStatePublisher& m_publisher;
    mutable std::mutex m_mutex;
    // Each slot retains the active root and the newest ready successor. A
    // single latest-root entry loses the old half of a coherent combination
    // when interdependent slots complete out of order.
    std::array<std::vector<ArtifactSnapshot>, kPublishedFragmentCount> m_roots;
    std::uint64_t m_manifestRevision = 0;
    std::shared_ptr<PublicationNodeCache> m_publicationNodes =
        std::make_shared<PublicationNodeCache>();
    std::atomic_bool m_accepting{ true };
};

} // namespace br::render
