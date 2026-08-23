#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Render/AsyncStateGraph.h"

namespace br::render {

struct PublishedStateFragment {
    std::uint64_t revision = 0;
    ArtifactPayload payload;
    std::vector<ArtifactSnapshot> dependencyClosure;
};

struct PublishedRendererState {
    std::uint64_t epoch = 0;
    PublishedStateFragment materials;
    PublishedStateFragment geometry;
    PublishedStateFragment drawRecords;
    PublishedStateFragment activeDrawLists;
    PublishedStateFragment indirectWorkloads;
    std::vector<std::shared_ptr<const void>> resourceHolds;
};

struct RendererStateCandidate {
    std::uint64_t baseEpoch = 0;
    std::shared_ptr<const PublishedRendererState> state;
};

struct RendererStatePublisherStats {
    std::uint64_t candidates = 0;
    std::uint64_t committed = 0;
    std::uint64_t replacedCandidates = 0;
    std::uint64_t rejectedBaseEpoch = 0;
    std::uint64_t commitMicros = 0;
    std::size_t retainedFrameStates = 0;
};

class RendererStatePublisher {
public:
    explicit RendererStatePublisher(std::size_t framesInFlight = 0);

    void Bootstrap(std::shared_ptr<const PublishedRendererState> fallback, std::size_t framesInFlight);
    bool PublishCandidate(RendererStateCandidate candidate);
    bool PublishArtifact(const ArtifactSnapshot& artifact);

    // Must be called after the frame slot fence has completed. This releases
    // that slot's old state and captures the selected state for the new frame.
    std::shared_ptr<const PublishedRendererState> Commit(std::size_t frameSlot);
    void ReleaseFrameSlot(std::size_t frameSlot);
    void DiscardCandidate();

    [[nodiscard]] std::shared_ptr<const PublishedRendererState> Active() const;
    [[nodiscard]] std::uint64_t ActiveEpoch() const;
    [[nodiscard]] RendererStatePublisherStats Stats() const;

private:
    mutable std::mutex m_mutex;
    RendererStateCandidate m_candidate;
    std::shared_ptr<const PublishedRendererState> m_active;
    std::vector<std::shared_ptr<const PublishedRendererState>> m_frameStates;
    RendererStatePublisherStats m_stats;
};

} // namespace br::render
