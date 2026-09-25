#pragma once

#include <memory>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "Render/ViewStateArtifacts.h"
#include "Resources/PixelBuffer.h"
#include "Render/PreparedPass.h"

namespace org { class PreparedLifecycleEffect; }

namespace br::render {

class IDepthHistoryService {
public:
    virtual ~IDepthHistoryService() = default;
    virtual std::shared_ptr<const org::PreparedLifecycleEffect>
        ReserveDepthHistoryPublication(
            std::shared_ptr<const PreparedViewFamilyState> views,
            std::uint64_t producerFrameNumber) = 0;
};

// Submission-owned temporal state. ViewManager still produces view resources,
// but accepted frames never read or mutate its notion of "previous frame".
class DepthHistoryPublicationService final : public IDepthHistoryService {
public:
    void Clear() {
        std::lock_guard lock(m_state->mutex);
        const auto cancel = [](const DepthHistorySelection& selection) {
            if (selection.dependency)
                selection.dependency->cancelled.store(true, std::memory_order_release);
        };
        for (auto& [_, selection] : m_state->published) cancel(selection);
        for (auto& [_, selection] : m_state->pending) cancel(selection);
        m_state->published.clear();
        m_state->pending.clear();
        ++m_state->generation;
    }

    DepthHistorySelection Select(std::uint64_t viewID,
        const std::shared_ptr<org::PixelBuffer>& current) const {
        if (!current) return {};
        std::lock_guard lock(m_state->mutex);
        auto it = m_state->pending.find(viewID);
        const DepthHistorySelection* selected = it == m_state->pending.end() ? nullptr : &it->second;
        if (!selected) {
            const auto published = m_state->published.find(viewID);
            if (published == m_state->published.end()) return {};
            selected = &published->second;
        }
        if (!selected->resource ||
            selected->resource->GetGlobalResourceID() != current->GetGlobalResourceID() ||
            selected->resource->GetBackingGeneration() != current->GetBackingGeneration()) {
            return {};
        }
        auto result = *selected;
        if (result.dependency)
            result.producerSubmissionID = result.dependency->submissionID.load(std::memory_order_acquire);
        return result;
    }

    std::shared_ptr<const org::PreparedLifecycleEffect>
        ReserveDepthHistoryPublication(
            std::shared_ptr<const PreparedViewFamilyState> views,
            std::uint64_t producerFrameNumber) override {
        struct Reservation {
            std::shared_ptr<State> state;
            std::shared_ptr<const PreparedViewFamilyState> views;
            std::uint64_t producerFrameNumber = 0;
            std::uint64_t generation = 0;
            std::shared_ptr<DepthHistoryDependency> dependency;
        };
        auto dependency = std::make_shared<DepthHistoryDependency>();
        auto reservation = std::make_shared<Reservation>(Reservation{
            m_state, std::move(views), producerFrameNumber, 0, dependency });
        {
            std::lock_guard lock(reservation->state->mutex);
            reservation->generation = reservation->state->generation;
            if (reservation->views) {
                for (const auto& view : reservation->views->views) {
                    if (!view.linearDepthMap) continue;
                    reservation->state->pending[view.id] = {view.linearDepthMap,
                        view.linearDepthMap->GetBackingGeneration(), 0,
                        producerFrameNumber, dependency};
                }
            }
        }
        const auto submitted = [](Reservation& value, org::SubmissionContext context) {
            if (!value.state || !value.views) return;
            std::lock_guard lock(value.state->mutex);
            if (value.generation != value.state->generation) {
                value.dependency->cancelled.store(true, std::memory_order_release);
                return;
            }
            value.dependency->submissionID.store(context.submissionID, std::memory_order_release);
            for (const auto& view : value.views->views) {
                if (!view.linearDepthMap) continue;
                auto selection = DepthHistorySelection{
                    view.linearDepthMap,
                    view.linearDepthMap->GetBackingGeneration(),
                    context.submissionID,
                    value.producerFrameNumber,
                    value.dependency };
                value.state->published[view.id] = selection;
                const auto pending = value.state->pending.find(view.id);
                if (pending != value.state->pending.end()
                    && pending->second.dependency == value.dependency)
                    value.state->pending.erase(pending);
            }
        };
        const auto abandoned = [](Reservation& value, org::AbandonReason) {
            if (!value.state || !value.dependency) return;
            value.dependency->cancelled.store(true, std::memory_order_release);
            std::lock_guard lock(value.state->mutex);
            for (auto it = value.state->pending.begin(); it != value.state->pending.end();) {
                if (it->second.dependency == value.dependency) it = value.state->pending.erase(it);
                else ++it;
            }
        };
        return std::make_shared<const org::PreparedOwnedLifecycle<Reservation>>(
            std::move(reservation), submitted, nullptr, abandoned);
    }

private:
    struct State {
        mutable std::mutex mutex;
        std::unordered_map<std::uint64_t, DepthHistorySelection> published;
        std::unordered_map<std::uint64_t, DepthHistorySelection> pending;
        std::uint64_t generation = 1;
    };
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace br::render
